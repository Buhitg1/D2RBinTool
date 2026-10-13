// d2datatables decompiler - bin -> txt, the inverse of the verified forward
// compile path.  Ground truth: the ported engine (excel_engine / calc_compiler),
// which reproduces every shipped .bin byte-exactly.
//
// Design (see RE_NOTES.md FINDINGS 2026-10-12c):
//  * The pipeline mirrors `d2datatables compile` exactly: same index seeds,
//    same kTables order, same Engine instance.  For every table we generate a
//    txt from the shipped bin, write it, and then feed it back through
//    eng.Compile so every intern / code add / code page append / table image
//    advances exactly like the forward pass.
//  * Fields that store only an index (types 17/18, seed columns) get
//    deterministic placeholder names "<handle>#<idx>"; fields that store real
//    text (1/7/9/10/16/19/20) emit it verbatim.  Lookups (11/13/15/21/22/23,
//    callbacks) invert through the engine's index state (TextAt / CodeAt).
//  * Columns whose txt column did not exist in the original source (all rows
//    hold the zero/sentinel init pattern) are omitted so the engine's
//    zero-init block reproduces them exactly.
#include "decompiler.h"
#include "excel_engine.h"
#include "excel_types.h"
#include "calc_compiler.h"
#include "tables.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace d2dt {
namespace {

inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline int16_t rdi16(const uint8_t* p) { int16_t v; memcpy(&v, p, 2); return v; }
inline int32_t rdi32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }

// sub_1419DAFD0 inverse: 4 packed bytes -> the code text (trailing spaces
// trimmed; interior spaces are significant and kept).
std::string UnpackCode(uint32_t code) {
  char c[4] = {(char)(uint8_t)code, (char)(uint8_t)(code >> 8),
               (char)(uint8_t)(code >> 16), (char)(uint8_t)(code >> 24)};
  std::string s(c, 4);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// A standalone code-index cell that reproduces a failed strict lookup.  An
// empty cell packs to four spaces (0x20202020) via PackCode, which is never a
// registered code, so the strict lookup fails and the parser stores the
// sentinel - exactly what the game's own .txt does: it leaves these columns
// blank (verified: base/itemtypes.txt StaffMods/Class are empty and compile to
// 0xFF byte-exactly).  So an empty cell is the faithful reconstruction.
const char* kLookupMissCode = "";
// Inside a comma separated variation list an empty token would TERMINATE the
// list (ParseVariations: `if (!tok[0]) break`), so a non-empty token is needed
// there.  "?" packs to 0x3F202020, which is never a registered code either.
const char* kVariationMissCode = "?";
const char* kLookupMissStr = "~";

const char* kCalcFnNames[9] = {"min", "max", "rand", "skill", "miss",
                               "stat", "sklvl", "sksrc", "cond"};
const char* kCondStates[8] = {"IsType", "IsClass", "Desecrated",
                              "MonsterTestElite", "ItemIsType",
                              "ItemIsModType", "Difficulty",
                              "IsDesecratedZonesEnabled"};

// The generated calc-code page a callback appends to (sub_140AE7C60..7F30).
// The names are the ground-truth index names the engine registers via
// sub_1401142A8(version, name, index) and serialises as "<name>.bin"; each of
// these strings is present in d2r.exe ("condcalc"/"monpetcalc" are not).
const char* CalcPageOf(const char* cb) {
  if (!cb) return nullptr;
  if (std::strcmp(cb, "calc_skills") == 0) return "skillscode";
  if (std::strcmp(cb, "calc_skilldesc") == 0) return "skilldesccode";
  if (std::strcmp(cb, "calc_missile") == 0) return "misscode";
  if (std::strcmp(cb, "calc_item") == 0) return "itemscode";
  if (std::strcmp(cb, "calc_monpet") == 0) return "moncode";
  if (std::strcmp(cb, "calc_condition") == 0) return "tccode";
  return nullptr;
}

struct Decomp {
  Engine& eng;
  std::string in_dir;
  std::string out_dir;
  // Texts chosen during generation but not yet compiled into the engine:
  // pending[handle][idx - engine_count] == text.
  std::map<std::string, std::vector<std::string>> pending;
  // Codes the table's own type-10 fields will add during the forward pre-pass,
  // in add order.  index of the k-th == engine_count + k (code.Add never
  // dedupes, it bumps and appends).  Needed to invert a table's own code
  // lookups (e.g. itemtypes.equivN -> itemtypes_code_index) before that table compiles.
  std::map<std::string, std::vector<uint32_t>> pend_codes;
  // Localisation reverse map (id -> key); populated from the engine's loaded
  // json (or tsv fallback) before the pipeline runs, first id wins.
  std::map<uint32_t, std::string> translation;
  // Loaded calc page images from in_dir (name -> bytes).
  std::map<std::string, std::vector<uint8_t>> pages;
  // Columns contributed by a kind-4 registration that its kind-0 twin lacks.
  // They are captured at the kind-4 replay and appended when the shared txt is
  // written by the kind-0 pass (the game reads ONE file for both registrations).
  std::map<std::string, std::vector<std::string>> extra_header;
  std::map<std::string, std::vector<std::vector<std::string>>> extra_rows;
  // Cache of source-txt columns read by name ("<txt>\x1f<col>" -> cells).
  std::map<std::string, std::vector<std::string>> src_cache;
  // Cache of resolved type 17/18 column texts ("<table>\x1f<field>" -> cells),
  // so the interning pre-pass and the main generation agree on what every
  // index slot holds.  Cleared per registration in GenerateTable.
  std::map<std::string, std::vector<std::string>> idx_col;
  long warns = 0;
  long diffs = 0;
  // Kind-4 replays run early in the pipeline; their lookup-only cells may
  // reference indexes that later tables have not built yet, which is exactly
  // how the game compiles them too.  Silence those.
  bool quiet = false;

  void warn(const std::string& s) {
    if (quiet) return;
    ++warns;
    std::fprintf(stderr, "decompile: %s\n", s.c_str());
  }

  // ------------------------------------------------------ index reversals
  uint32_t Committed(const std::string& handle) {
    return eng.GetIndex(handle)->str.count();
  }

  // index -> text, consulting the engine first and the pending overlay after.
  bool StrText(const std::string& handle, uint32_t idx, std::string* out) {
    DataIndex* ix = eng.GetIndex(handle);
    if (const std::string* s = ix->str.TextAt(idx)) { *out = *s; return true; }
    uint32_t committed = ix->str.count();
    if (idx >= committed) {
      auto it = pending.find(handle);
      uint32_t p = idx - committed;
      if (it != pending.end() && p < it->second.size()) {
        *out = it->second[p];
        return true;
      }
    }
    return false;
  }

  bool CodeText(const std::string& handle, uint32_t idx, std::string* out) {
    DataIndex* ix = eng.GetIndex(handle);
    uint32_t code;
    if (ix->code.CodeAt(idx, &code)) {
      *out = UnpackCode(code);
      return true;
    }
    // Consult the pending overlay for entries this table's own pre-pass will
    // add at compile time (code.Add numbers them engine_count + k).
    uint32_t committed = ix->code.count();
    if (idx >= committed) {
      auto it = pend_codes.find(handle);
      uint32_t k = idx - committed;
      if (it != pend_codes.end() && k < it->second.size()) {
        *out = UnpackCode(it->second[k]);
        return true;
      }
    }
    return false;
  }

  // Register a string interned by the compile of the generated txt.  New
  // entries must arrive in strict sequential order (first-occurrence order of
  // the forward parse); duplicates resolve to the existing entry.
  void Register(const std::string& handle, uint32_t stored, const std::string& text,
                const std::string& what) {
    uint32_t known = Committed(handle) + (uint32_t)pending[handle].size();
    if (stored < known) return;            // duplicate of an existing entry
    if (stored > known) {
      warn(std::string(what) + ": index gap in " + handle + " (stored " +
           std::to_string(stored) + ", expected " + std::to_string(known) + ")");
      return;
    }
    pending[handle].push_back(text);
  }

  // ------------------------------------------------------------- helpers
  bool ReadBin(const std::string& table, std::vector<uint8_t>* out) const {
    return ReadFile(in_dir + "/" + table + ".bin", *out);
  }

  static bool ReadFile(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)(n < 0 ? 0 : n));
    if (n > 0) { size_t rd = fread(out.data(), 1, out.size(), f); (void)rd; }
    fclose(f);
    return true;
  }

  static bool WriteFile(const std::string& p, const std::string& s) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    size_t wr = fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return wr == s.size();
  }

  static bool WriteFile(const std::string& p, const std::vector<uint8_t>& b) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    size_t wr = fwrite(b.data(), 1, b.size(), f);
    fclose(f);
    return wr == b.size();
  }

  const TableDef* FindTable(const std::string& name) const {
    for (int i = 0; i < kTableCount; ++i)
      if (name == kTables[i].name) return &kTables[i];
    return nullptr;
  }

  const ExcelFieldDesc* FindField(const TableDef* def, const char* field) const {
    if (!def || !def->descs) return nullptr;
    for (int i = 0; def->descs[i].name; ++i)
      if (std::strcmp(def->descs[i].name, field) == 0) return &def->descs[i];
    return nullptr;
  }

  const std::vector<uint8_t>* Page(const std::string& name) {
    auto it = pages.find(name);
    // NOTE: a miss is cached as an empty vector, so the hit path must apply the
    // same emptiness test (returning &it->second would hand out a bogus
    // non-null pointer to nothing).
    if (it != pages.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<uint8_t> raw;
    std::vector<uint8_t>* slot = &pages[name];
    if (ReadFile(in_dir + "/" + name + ".bin", raw)) *slot = std::move(raw);
    return slot->empty() ? nullptr : slot;
  }
};

// ------------------------------------------------------- calc disassembler
//
// Postfix stream -> fully parenthesised infix text.  PAREN never reaches the
// output stream (sub_141A032F0 only pushes it), so extra parentheses are free
// and recompile to identical bytes.  Operand kinds (sub_140AE8D90):
//   NUM = kind 1: resolved identifier value (skill/stat/cond words) or a
//         literal; digits lex straight to NUM, so a decimal fallback is always
//         byte-exact for NUM operands.
//   VAL = kind 0: family calc-code index (skillcalc_code_index / misscalc_code_index); must be emitted as
//         an identifier, digits would lex into NUM.
struct CalcOperand {
  std::string text;
  bool is_val = false;
  // True only for an operand that came straight from a NUM byte (a literal or a
  // resolved identifier).  Those need contextual re-resolution inside skill/
  // miss/stat/cond/min/max; a sub-expression result already carries its text
  // and must never be collapsed to its (meaningless) folded value.
  bool leaf_num = false;
  // Set on a node whose text carries an unmatched '(' (a source-data typo: a
  // ')' that was never written).  The compiler flushes such a '(' as a BBE_PAREN
  // byte at the end, at a stack depth fixed by how many operators sit below it.
  // An enclosing operator must therefore NOT add its own closing ')' (it would
  // close the stray '(' and move the byte), so it drops its parentheses.
  bool unmatched = false;
  int value = 0;
};

// True when `s` is the "<handle>#<idx>" placeholder IndexColumnText /
// SeedColumnFromBin emit for a type 17/18 (index-only) column whose source txt
// is absent -- the record stores only the index, so the name is unrecoverable.
// Such a name must never be re-emitted as a calc identifier: it is longer than
// the real one and, inside stat(), would change the NUM operand's width.
bool IsPlaceholderName(const char* handle, const std::string& s) {
  const size_t n = std::strlen(handle);
  if (s.size() <= n + 1 || s.compare(0, n, handle) != 0 || s[n] != '#')
    return false;
  for (size_t i = n + 1; i < s.size(); ++i)
    if (!std::isdigit((unsigned char)s[i])) return false;
  return true;
}

// Resolve a NUM operand of `fn` at argument index `ai` (state = cond state).
std::string ResolveNumArg(Decomp& d, int fn, int ai, int state,
                          const CalcOperand& op, CalcFamilyKind fam) {
  std::string s;
  // A resolved string-index name may contain spaces, so it must be re-emitted
  // quoted for the lexer to read it back as one token.
  auto Q = [](const std::string& t) { return "'" + t + "'"; };
  switch (fn) {
    case FN_SKILL:  // skill
      if (ai == 0) {
        if (d.StrText("skills_skill_index_p0", (uint32_t)op.value, &s)) return Q(s);
        break;
      }
      return std::to_string(op.value);
    case FN_MISS:  // miss
      if (ai == 0) {
        if (d.StrText("missiles_missile_index", (uint32_t)op.value, &s)) return Q(s);
        break;
      }
      return std::to_string(op.value);
    case FN_STAT:  // stat
      if (ai == 0) {
        // Prefer the real stat name (it is what the shipped txt holds and what
        // makes the output readable), but itemstatcost is an index-only table:
        // with no itemstatcost.txt the seeded name is a placeholder, so emit
        // the decimal id instead.  Digits lex straight to NUM, so the operand
        // re-encodes byte-identically (verified: monpet.bindchancecalc), and
        // this is what makes itemstatcost.txt optional for a byte-exact round
        // trip.  Not a warning case -- the fallback is exact by construction.
        if (d.StrText("itemstatcost_stat_index", (uint32_t)op.value, &s) &&
            !IsPlaceholderName("itemstatcost_stat_index", s))
          return Q(s);
        return std::to_string(op.value);
      }
      if (ai == 1) {
        if (op.value == 0) return "accr";
        if (op.value == 1) return "base";
        if (op.value == 2) return "mod";
        break;
      }
      return std::to_string(op.value);
    case FN_SKLVL:  // sklvl
      if (ai == 0) {
        if (d.StrText("skills_skill_index_p0", (uint32_t)op.value, &s)) return Q(s);
        break;
      }
      return std::to_string(op.value);  // operand 1 digits -> VAL? no: the
                                        // strtol path yields kind 1 -> NUM.
    case FN_SKSRC:  // sksrc
      if (ai == 0) {
        if (d.StrText("skills_skill_index_p0", (uint32_t)op.value, &s)) return Q(s);
        break;
      }
      return std::to_string(op.value);
    case FN_COND:  // cond
      if (ai == 0) {
        if (op.value >= 0 && op.value < 8) return Q(kCondStates[op.value]);
        break;
      }
      if (ai == 1) {
        switch (state) {
          case COND_ISTYPE:
            switch (op.value) {
              case CT_PLAYER: return "player"; case CT_MONSTER: return "monster";
              case CT_OBJECT: return "object"; case CT_MISSILE: return "missile";
              case CT_ITEM: return "item";   case CT_ROOMTILE: return "roomtile";
            }
            break;
          case COND_ISCLASS:  // IsClass: playerclass_code_index code -> monstats_id_index -> missiles_missile_index -> item_code_index code
            if (d.CodeText("playerclass_code_index", (uint32_t)op.value, &s)) return s;
            if (d.StrText("monstats_id_index", (uint32_t)op.value, &s)) return Q(s);
            if (d.StrText("missiles_missile_index", (uint32_t)op.value, &s)) return Q(s);
            if (d.CodeText("item_code_index", (uint32_t)op.value, &s)) return s;
            break;
          case COND_MONSTERTESTELITE:
            switch (op.value) {
              case CM_UNIQUE: return "unique"; case CM_CHAMPION: return "champion";
              case CM_SUPER: return "super";  case CM_MINION: return "minion";
              case CM_HERALD: return "herald"; case CM_HERALDMINION: return "heraldminion";
            }
            break;
          case COND_ITEMISTYPE:
            if (d.CodeText("itemtypes_code_index", (uint32_t)op.value, &s)) return s;
            break;
          case COND_ITEMISMODTYPE:
            switch (op.value) {
              case CQ_UNIQUE: return "unique"; case CQ_SET: return "set";
              case CQ_RARE: return "rare";   case CQ_MAGIC: return "magic";
              case CQ_CRAFTED: return "crafted"; case CQ_LOWQUALITY: return "lowquality";
              case CQ_NORMAL: return "normal"; case CQ_HIQUALITY: return "hiquality";
              case CQ_TEMPERED: return "tempered";
            }
            break;
          case COND_DIFFICULTY:
            switch (op.value) {
              case CD_NORMAL: return "normal"; case CD_NIGHTMARE: return "nightmare";
              case CD_HELL: return "hell";
            }
            break;
        }
        break;
      }
      return std::to_string(op.value);
    default:
      return std::to_string(op.value);  // min/max/rand literals
  }
  d.warn("calc: cannot resolve NUM(" + std::to_string(op.value) + ") of fn " +
         std::to_string(fn) + " arg " + std::to_string(ai) +
         "; falling back to decimal");
  return std::to_string(op.value);
}

// Resolve a VAL operand (family calc-code index) to an identifier.
std::string ResolveVal(Decomp& d, CalcFamilyKind fam, int value) {
  std::string s;
  const char* handle = nullptr;
  switch (fam) {
    case CALC_FAM_SKILLS:
    case CALC_FAM_SKILLDESC: handle = "skillcalc_code_index"; break;
    case CALC_FAM_MISSILES:  handle = "misscalc_code_index"; break;
#ifdef D2DT_BUILD_93854
    // 93854: monpet calc expressions resolve bare identifiers (hpct/diff/lvl/
    // elte/pcl1..pcl5) through the moncalc code index.
    case CALC_FAM_MONPET:    handle = "moncalc_code_index"; break;
#endif
    default: break;
  }
  if (handle && d.CodeText(handle, (uint32_t)value, &s)) return "'" + s + "'";
  d.warn("calc: cannot resolve VAL(" + std::to_string(value) +
         ") for family " + std::to_string((int)fam));
  return std::to_string(value);
}

// Disassemble one expression starting at `off`; returns the text and advances
// *end to the first byte after the terminating BBE_END.
std::string DisasmCalc(Decomp& d, const std::vector<uint8_t>& pg, uint32_t off,
                       CalcFamilyKind fam, size_t* end) {
  std::vector<CalcOperand> st;
  size_t i = off;
  auto pop = [&]() -> CalcOperand {
    if (st.empty()) return CalcOperand();
    CalcOperand o = std::move(st.back());
    st.pop_back();
    return o;
  };
  auto num = [](int v) {
    CalcOperand o;
    o.text = std::to_string(v);
    o.value = v;
    o.leaf_num = true;
    return o;
  };
  while (i < pg.size()) {
    uint8_t op = pg[i++];
    if (op == BBE_END) break;
    switch (op) {
      case BBE_FUNC: {
        if (i + 1 >= pg.size()) { *end = i; return ""; }
        int fn = pg[i++];
        int argc = pg[i++];
        std::vector<CalcOperand> args(argc);
        for (int a = argc - 1; a >= 0; --a) args[a] = pop();
        // cond state comes from the resolved first argument value.
        int state = (fn == FN_COND && argc > 0) ? args[0].value : -1;
        std::ostringstream oss;
        oss << kCalcFnNames[fn <= FN_COND ? fn : 0] << "(";
        bool un = false;
        for (int a = 0; a < argc; ++a) {
          if (a) oss << ",";
          if (args[a].is_val) oss << args[a].text;   // already resolved at push
          else if (args[a].leaf_num)
            oss << ResolveNumArg(d, fn, a, state, args[a], fam);
          else oss << args[a].text;                  // sub-expression / function
          un = un || args[a].unmatched;
        }
        // An unmatched '(' in the last argument means the call's own ')' would
        // close it, so leave the call unclosed.
        if (!un) oss << ")";
        CalcOperand o;
        o.text = oss.str();
        o.unmatched = un;
        st.push_back(std::move(o));
        break;
      }
      case BBE_PAREN: {
        // An unmatched source '(' : it encloses the operand built so far and
        // stays open to the end of the expression.
        CalcOperand t = pop();
        CalcOperand o;
        o.text = "(" + t.text;
        o.unmatched = true;
        st.push_back(std::move(o));
        break;
      }
      case BBE_NUM8:  if (i < pg.size()) st.push_back(num((int8_t)pg[i++])); else i = pg.size(); break;
      case BBE_NUM16: if (i + 1 < pg.size()) { st.push_back(num(rdi16(&pg[i]))); i += 2; } else i = pg.size(); break;
      case BBE_NUM32: if (i + 3 < pg.size()) { st.push_back(num(rdi32(&pg[i]))); i += 4; } else i = pg.size(); break;
      case BBE_VAL8: {
        if (i >= pg.size()) { i = pg.size(); break; }
        CalcOperand o; o.is_val = true; o.value = (int8_t)pg[i++];
        o.text = ResolveVal(d, fam, o.value);
        st.push_back(std::move(o));
        break;
      }
      case BBE_VAL16: {
        if (i + 1 >= pg.size()) { i = pg.size(); break; }
        CalcOperand o; o.is_val = true; o.value = rdi16(&pg[i]); i += 2;
        o.text = ResolveVal(d, fam, o.value);
        st.push_back(std::move(o));
        break;
      }
      case BBE_VAL32: {
        if (i + 3 >= pg.size()) { i = pg.size(); break; }
        CalcOperand o; o.is_val = true; o.value = rdi32(&pg[i]); i += 4;
        o.text = ResolveVal(d, fam, o.value);
        st.push_back(std::move(o));
        break;
      }
      case BBE_LT: case BBE_GT: case BBE_LE: case BBE_GE:
      case BBE_EQ: case BBE_NE: case BBE_ADD: case BBE_SUB:
      case BBE_MUL: case BBE_DIV: case BBE_POW:
#ifdef D2DT_BUILD_93854
      case BBE_MOD:   // 3.3 remainder operator
#endif
      {
        CalcOperand b = pop(), a = pop();
        const char* sym = "+";
        switch (op) {
          case BBE_LT: sym = "<"; break; case BBE_GT: sym = ">"; break;
          case BBE_LE: sym = "<="; break; case BBE_GE: sym = ">="; break;
          case BBE_EQ: sym = "=="; break; case BBE_NE: sym = "!="; break;
          case BBE_SUB: sym = "-"; break; case BBE_MUL: sym = "*"; break;
          case BBE_DIV: sym = "/"; break; case BBE_POW: sym = "^"; break;
#ifdef D2DT_BUILD_93854
          case BBE_MOD: sym = "%"; break;
#endif
        }
        CalcOperand o;
        bool un = a.unmatched || b.unmatched;
        o.text = un ? (a.text + sym + b.text)
                    : ("(" + a.text + sym + b.text + ")");
        o.unmatched = un;
        st.push_back(std::move(o));
        break;
      }
      case BBE_NEG: {
        CalcOperand a = pop();
        CalcOperand o;
        o.text = a.unmatched ? ("-" + a.text) : ("(-" + a.text + ")");
        o.unmatched = a.unmatched;
        st.push_back(std::move(o));
        break;
      }
      case BBE_TERNARY: {
        CalcOperand c = pop(), b = pop(), a = pop();
        CalcOperand o;
        bool un = a.unmatched || b.unmatched || c.unmatched;
        o.text = un ? (a.text + "?" + b.text + ":" + c.text)
                    : ("(" + a.text + "?" + b.text + ":" + c.text + ")");
        o.unmatched = un;
        st.push_back(std::move(o));
        break;
      }
      default:
        d.warn("calc: unexpected opcode " + std::to_string(op) + " at " +
               std::to_string(i - 1));
        *end = i;
        return "";
    }
  }
  *end = i;
  // VAL operands already carry their resolved identifier text; NUM operands are
  // literal digits, which re-lex to the identical NUM byte.
  if (st.empty()) return std::string();
  if (st.size() > 1) {
    // More than one operand left with no operator: the source had an unclosed
    // function call (e.g. `stat('x'.accr` with no ')'), so the FUNC byte was
    // never emitted and only its arguments survive.  The function identity is
    // not in the bytes.  Rebuild an unclosed call whose arguments are the
    // operand literals: a leading call token sets the "has value" flag the
    // compiler uses to skip constant folding (without it the two operands would
    // be folded away), while the missing ')' keeps the FUNC byte out of the
    // stream.  Literal arguments bypass the resolver, so the operand bytes are
    // reproduced exactly regardless of which function name is used.
    std::string j = "min(";
    for (size_t k = 0; k < st.size(); ++k) {
      if (k) j += ',';
      j += st[k].text;
    }
    return j;  // deliberately unclosed
  }
  return st.back().text;
}

// ---------------------------------------------------- callback inversions

std::string InvertCubeInput(Decomp& d, const uint8_t* slot) {
  uint16_t w0 = rd16(slot), w1 = rd16(slot + 2), v1 = rd16(slot + 4);
  uint8_t kind = slot[6], qty = slot[7];
  if (!w0 && !w1 && !v1 && !kind && !qty) return "";
  std::string head;
  std::string s;
  if ((w0 & 0x41) == 0x41) {
    if (kind == 7) {
      if (d.StrText("uniqueitems_name_index", (uint32_t)(v1 - 1), &s)) head = s;
      else d.warn("cube input: missing unique name " + std::to_string(v1 - 1));
    } else if (kind == 5) {
      if (d.StrText("setitems_name_index", (uint32_t)(v1 - 1), &s)) head = s;
      else d.warn("cube input: missing set name " + std::to_string(v1 - 1));
    } else {
      if (d.StrText("uniqueitems_name_index", (uint32_t)(v1 - 1), &s)) head = s;
    }
  } else if (w0 & 2) {
    if (d.CodeText("itemtypes_code_index", w1, &s)) head = s;
  } else if ((w0 & 1) && w1 == 0xFFFF) {
    head = "any";
  } else if (w0 & 1) {
    if (d.CodeText("item_code_index", w1, &s)) head = s;
  }
  if (head.empty()) {
    d.warn("cube input: cannot invert first token (flags " +
           std::to_string(w0) + ")");
    return "";
  }
  std::string out = head;
  static const char* kKindWords[10] = {"", "low", "nor", "hiq", "mag",
                                       "set", "rar", "uni", "crf", "tmp"};
  if (qty) out += ",qty=" + std::to_string(qty);
  if (kind >= 1 && kind <= 9) { out += ","; out += kKindWords[kind]; }
  struct { uint16_t bit; const char* word; } kWords[] = {
      {4, "nos"}, {8, "sock"}, {0x10, "eth"}, {0x20, "noe"}, {0x80, "upg"},
      {0x100, "bas"}, {0x200, "exc"}, {0x400, "eli"}, {0x800, "nru"},
      {0x1000, "id"},
  };
  for (auto& e : kWords)
    if (w0 & e.bit) { out += ","; out += e.word; }
  return out;
}

// Cube-output item type byte (slot[13]), written by sub_140B56450.  Positive
// values are the fixed portal words; the negative values are the special
// tokens the output grammar uses in place of an item type.
enum CubeOutType {
  CUBE_OUT_COW_PORTAL = 1,
  CUBE_OUT_PANDEMONIUM_PORTAL = 2,
  CUBE_OUT_PANDEMONIUM_FINALE_PORTAL = 3,
  CUBE_OUT_RED_PORTAL = 4,
  CUBE_OUT_REG_OR_USETYPE = -1,  // "reg" when (w4 & 0x40), else "usetype"
  CUBE_OUT_USEITEM = -2,
  CUBE_OUT_ITEMTYPE = -3,        // itemtypes_code_index lookup
  CUBE_OUT_UNIQUE_OR_CODE = -4,  // unique/set name when (w4 & 8), else item code
};

std::string InvertCubeOutput(Decomp& d, const uint8_t* slot, uint32_t row) {
  uint32_t rownum = rd32(slot);
  uint16_t w4 = rd16(slot + 4);
  uint16_t stat = rd16(slot + 6);
  uint16_t v1 = rd16(slot + 8);
  uint8_t kind = slot[10], qty = slot[11], lvl = slot[12];
  int8_t type = (int8_t)slot[13];
  bool rest_zero = true;
  for (int k = 4; k < 29; ++k)
    if (slot[k]) { rest_zero = false; break; }
  if (!rownum && rest_zero) return "";   // absent column or empty cell
  if (rownum == row && rest_zero) return "x";  // parse failure after row write
  if (rownum != row)
    d.warn("cube output: row number " + std::to_string(rownum) +
           " != row " + std::to_string(row));
  std::string head;
  std::string s;
  switch (type) {
    case CUBE_OUT_COW_PORTAL: head = "Cow Portal"; break;
    case CUBE_OUT_PANDEMONIUM_PORTAL: head = "Pandemonium Portal"; break;
    case CUBE_OUT_PANDEMONIUM_FINALE_PORTAL: head = "Pandemonium Finale Portal"; break;
    case CUBE_OUT_RED_PORTAL: head = "Red Portal"; break;
    // "reg" also clears the type byte (sub_140B56450), so it stands in for
    // "usetype" whenever its flag bit is set.
    case CUBE_OUT_REG_OR_USETYPE: head = (w4 & 0x40) ? "reg" : "usetype"; break;
    case CUBE_OUT_USEITEM: head = "useitem"; break;
    case CUBE_OUT_ITEMTYPE:
      if (d.CodeText("itemtypes_code_index", stat, &s)) head = s;
      break;
    case CUBE_OUT_UNIQUE_OR_CODE:
      if (w4 & 8) {
        if (kind == 5) {
          if (!d.StrText("setitems_name_index", (uint32_t)(v1 - 1), &s))
            d.warn("cube output: missing set name");
          head = s;
        } else {
          if (!d.StrText("uniqueitems_name_index", (uint32_t)(v1 - 1), &s))
            d.warn("cube output: missing unique name");
          head = s;
        }
      } else {
        if (d.CodeText("item_code_index", stat, &s)) head = s;
      }
      break;
    default:
      d.warn("cube output: cannot invert type " + std::to_string((int)type));
      return "";
  }
  if (head.empty()) return "";
  std::string out = head;
  static const char* kKindWords[10] = {"", "low", "nor", "hiq", "mag",
                                       "set", "rar", "uni", "crf", "tmp"};
  // "sock" swallows the following token as its quantity, so the stored
  // quantity travels on the sock token itself ("sock=N").
  if (w4 & 2) {
    out += ",sock";
    if (qty) out += "=" + std::to_string(qty);
  } else if (qty) {
    out += ",qty=" + std::to_string(qty);
  }
  if (kind >= 1 && kind <= 9) { out += ","; out += kKindWords[kind]; }
  struct { uint16_t bit; const char* word; } kWords[] = {
      {1, "mod"}, {4, "eth"}, {0x10, "uns"}, {0x20, "rem"},
      {0x80, "exc"}, {0x100, "eli"}, {0x200, "rep"}, {0x400, "rch"},
  };
  for (auto& e : kWords)
    if (w4 & e.bit) { out += ","; out += e.word; }
  if (lvl) out += ",lvl=" + std::to_string(lvl);
  for (int k = 0; k < 3; ++k) {
    uint16_t pre = rd16(slot + 17 + 2 * k);
    if (pre) out += ",pre=" + std::to_string(pre);
  }
  for (int k = 0; k < 3; ++k) {
    uint16_t suf = rd16(slot + 23 + 2 * k);
    if (suf) out += ",suf=" + std::to_string(suf);
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------- generation
namespace {

uint32_t eng_empty_index(Decomp& d, const char* handle, int type);
std::string InvertCallback(Decomp& d, const TableDef& def,
                           const ExcelFieldDesc& f, const uint8_t* row,
                           const char* handle, const char* cb, uint32_t r);
bool GenerateField(Decomp& d, const TableDef& def, const ExcelFieldDesc& f,
                   const uint8_t* body, uint32_t rows, int rec,
                   std::vector<std::string>* cells);
const std::vector<std::string>* SourceColumnPtr(Decomp& d, const char* txt_name,
                                                const char* col);
// Text to emit for every row of a type 17/18 (index-only) column.
const std::vector<std::string>& IndexColumnText(Decomp& d, const TableDef& def,
                                                const ExcelFieldDesc& f,
                                                const uint8_t* body,
                                                uint32_t rows, int rec,
                                                const char* handle);
void ReorderPageColumns(Decomp& d, const TableDef& def, const uint8_t* body,
                        uint32_t rows, int rec, const std::vector<int>& col_desc,
                        std::vector<std::string>* header,
                        std::vector<std::vector<std::string>>* cols);

// Produce the cell texts for one table.  `emit` receives, per descriptor,
// whether the column belongs in the txt and what every row's cell holds.
bool GenerateTable(Decomp& d, const TableDef& def,
                   const std::vector<uint8_t>& bin,
                   std::vector<std::string>* header,
                   std::vector<std::vector<std::string>>* cols);

// Mirror the forward pre-pass (sub_1419D6EA0 pass 0): walk rows outer, columns
// inner, and for every interning field that survives the absent-column
// omission, record the code/string it will add.  A table's own lookup fields
// (types 11/13/15/21/22/23) are resolved in the SAME table's main pass against
// the index the pre-pass just filled, so this overlay is what makes
// self-referential indexes (e.g. itemtypes.equivN -> itemtypes_code_index) invertible before
// the table is compiled.
void PrescanInterning(Decomp& d, const TableDef& def, const uint8_t* body,
                      uint32_t rows, int rec) {
  if (!def.descs) return;
  for (uint32_t r = 0; r < rows; ++r) {
    const uint8_t* row = body + (size_t)r * rec;
    for (int fi = 0; def.descs[fi].name; ++fi) {
      const ExcelFieldDesc& f = def.descs[fi];
      int t = f.type;
      if (t != 10 && t != 12 && t != 14 && t != 16 && t != 17 && t != 18 &&
          t != 19 && t != 20)
        continue;
      const char* raw = (const char*)f.ptr;
      if (!raw) continue;
      if (std::strncmp(raw, "cb:", 3) == 0 || std::strncmp(raw, "sub_", 4) == 0)
        continue;  // a callback, not an index handle
      const char* handle = raw;
      const uint8_t* fld = row + f.offset;
      if (t == 10 || t == 12 || t == 14) {
        uint32_t code = (uint32_t)fld[0] | ((uint32_t)fld[1] << 8) |
                        ((uint32_t)fld[2] << 16) | ((uint32_t)fld[3] << 24);
        if (code) d.pend_codes[handle].push_back(code);  // code.Add appends
      } else if (t == 16) {
        // addFlag0: appends every non-empty row (duplicates included).
        std::string s;
        for (int k = 0; k < f.len - 1 && fld[k]; ++k) s.push_back((char)fld[k]);
        if (!s.empty()) d.pending[handle].push_back(s);
      } else {
        uint32_t idx = (t == 17 || t == 19) ? rd16(fld) : rd32(fld);
        std::string s;
        if (t == 19 || t == 20) {
          size_t shift = (t == 19) ? 2 : 4;
          for (int k = 0; k < f.len && fld[shift + k]; ++k)
            s.push_back((char)fld[shift + k]);
          if (s.size() > 0x30) s.resize(0x30);
        } else {
          s = IndexColumnText(d, def, f, body, rows, rec, handle)[r];
        }
        d.Register(handle, idx, s, std::string(def.name) + "." + f.name);
      }
    }
  }
}

bool GenerateTable(Decomp& d, const TableDef& def,
                   const std::vector<uint8_t>& bin,
                   std::vector<std::string>* header,
                   std::vector<std::vector<std::string>>* cols) {
  if (bin.size() < 4 || !def.descs || def.record_size <= 0) return false;
  uint32_t rows = rd32(bin.data());
  int rec = def.record_size;
  if (bin.size() < (size_t)4 + (size_t)rows * rec) {
    d.warn(std::string(def.name) + ": bin truncated");
    return false;
  }
  const uint8_t* body = bin.data() + 4;
  // Reset the overlay and rebuild what this table's own pre-pass will add.
  d.pending.clear();
  d.pend_codes.clear();
  d.idx_col.clear();
  PrescanInterning(d, def, body, rows, rec);
  std::vector<int> col_desc;   // descriptor index of each emitted column
  for (int fi = 0; def.descs[fi].name; ++fi) {
    std::vector<std::string> cells;
    if (!GenerateField(d, def, def.descs[fi], body, rows, rec, &cells)) continue;
    header->push_back(def.descs[fi].name);
    cols->push_back(std::move(cells));
    col_desc.push_back(fi);
  }
  ReorderPageColumns(d, def, body, rows, rec, col_desc, header, cols);
  return true;
}

// The engine appends each calc expression to its code page while it parses the
// txt columns LEFT TO RIGHT, and the record stores the page size BEFORE each
// append (sub_140AE9840).  So the stored offsets are strictly increasing in the
// original txt column order - which a FieldDesc array need not follow
// (missiles.txt has Radius before DmgSymPerCalc, but the desc array puts Radius
// last).  Re-derive the page columns' relative order from the offsets so the
// recompiled page lands identically: for each pair, count the rows where both
// offsets are present and one precedes the other.
void ReorderPageColumns(Decomp& d, const TableDef& def, const uint8_t* body,
                        uint32_t rows, int rec, const std::vector<int>& col_desc,
                        std::vector<std::string>* header,
                        std::vector<std::vector<std::string>>* cols) {
  (void)d;
  std::map<std::string, std::vector<size_t>> groups;  // page -> column indices
  for (size_t c = 0; c < col_desc.size(); ++c) {
    const ExcelFieldDesc& f = def.descs[col_desc[c]];
    if (f.type != 28 || !f.ptr) continue;
    const char* cb = (const char*)f.ptr;
    if (std::strncmp(cb, "cb:", 3) == 0) cb += 3;
    if (const char* pg = CalcPageOf(cb)) groups[pg].push_back(c);
  }
  for (auto& kv : groups) {
    std::vector<size_t>& g = kv.second;
    size_t m = g.size();
    if (m < 2) continue;
    std::vector<std::vector<int>> cnt(m, std::vector<int>(m, 0));
    for (size_t a = 0; a < m; ++a)
      for (size_t b = 0; b < m; ++b) {
        if (a == b) continue;
        int offa = def.descs[col_desc[g[a]]].offset;
        int offb = def.descs[col_desc[g[b]]].offset;
        int n = 0;
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* rr = body + (size_t)r * rec;
          uint32_t va = rd32(rr + offa), vb = rd32(rr + offb);
          if (va != 0xFFFFFFFFu && vb != 0xFFFFFFFFu && va < vb) ++n;
        }
        cnt[a][b] = n;
      }
    std::vector<size_t> order(m);
    for (size_t k = 0; k < m; ++k) order[k] = k;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return cnt[a][b] > cnt[b][a]; });
    std::vector<std::vector<std::string>> tmpc(m);
    std::vector<std::string> tmph(m);
    for (size_t k = 0; k < m; ++k) {
      tmpc[k] = std::move((*cols)[g[k]]);
      tmph[k] = (*header)[g[k]];
    }
    for (size_t k = 0; k < m; ++k) {
      (*cols)[g[k]] = std::move(tmpc[order[k]]);
      (*header)[g[k]] = tmph[order[k]];
    }
  }
}

// Read a NUL-terminated cell of at most `max` chars (sub_1419D6EA0 copy).
auto txt_of = [](const uint8_t* row, size_t off, size_t max, std::string* out) {
  out->clear();
  for (size_t k = 0; k < max && row[off + k]; ++k) out->push_back((char)row[off + k]);
};

std::vector<std::string> SplitTab(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == '\t') { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

bool IEquals(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t k = 0; k < n; ++k)
    if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]))
      return false;
  return true;
}

// Recover one column from a source txt by name.  Needed where the shipped bin
// cannot be the ground truth for a column: a kind-4 registration's own columns
// exist only in that EARLY registration, so the shipped .bin (written by the
// LATE registration) simply has no field for them -- the bytes at their offset
// belong to whatever kind-0 field shares the slot (monmode.code @0 vs
// monmode.name @0, actinfo.act @0 vs actinfo.town @0).  Two fallbacks exist
// before this one: the early registration's own .bin (AliasBinColumn, e.g.
// actinfo -> actcode.bin) and, failing that, the placeholder generator.
// A type-28 calc field is NOT in this set any more: its page (tccode for
// cb:calc_condition, moncode for cb:calc_monpet) is shipped on disk and read
// straight back by sub_140925290, so the bin is ground truth for it.
// In the remaining case the one txt that fed the generator is the only ground
// truth.
std::vector<std::string> ReadSourceColumn(Decomp& d, const char* txt_name,
                                          const char* col) {
  std::vector<std::string> out;
  std::vector<uint8_t> raw;
  if (!Decomp::ReadFile(d.in_dir + "/" + txt_name + ".txt", raw)) return out;
  std::vector<std::string> lines;
  {
    std::string cur;
    for (uint8_t ch : raw) {
      if (ch == '\n') { lines.push_back(cur); cur.clear(); }
      else if (ch != '\r') cur.push_back((char)ch);
    }
    if (!cur.empty()) lines.push_back(cur);
  }
  if (lines.empty()) return out;
  std::vector<std::string> hdr = SplitTab(lines[0]);
  int ci = -1;
  for (size_t i = 0; i < hdr.size(); ++i)
    if (IEquals(hdr[i], col)) { ci = (int)i; break; }
  if (ci < 0) return out;
  for (size_t i = 1; i < lines.size(); ++i) {
    std::vector<std::string> fl = SplitTab(lines[i]);
    // BuildCtx drops any row whose first cell is exactly "Expansion", so the
    // bin's row indices are shifted by one for such tables (uniqueitems).
    if (!fl.empty() && fl[0] == "Expansion") continue;
    out.push_back(ci < (int)fl.size() ? fl[ci] : std::string());
  }
  return out;
}

// Cached variant; nullptr when the file/column is absent.
const std::vector<std::string>* SourceColumnPtr(Decomp& d, const char* txt_name,
                                                const char* col) {
  std::string key = std::string(txt_name) + '\x1f' + col;
  auto it = d.src_cache.find(key);
  if (it != d.src_cache.end()) return it->second.empty() ? nullptr : &it->second;
  std::vector<std::string> v = ReadSourceColumn(d, txt_name, col);
  auto res = d.src_cache.emplace(key, std::move(v));
  return res.first->second.empty() ? nullptr : &res.first->second;
}

// The engine's intern key for a string-index cell: lowercased, cut to the
// 0x30-char rowbuf the pre-pass interns from (sub_1419D6EA0).
std::string IdxKey(const std::string& s) {
  std::string k = s.size() > 0x30 ? s.substr(0, 0x30) : s;
  std::transform(k.begin(), k.end(), k.begin(), ::tolower);
  return k;
}

// Text to emit for every row of an index column (type 17/18, and 19/20 whose
// text could not be read from the image).
//
// The record stores the index (types 17/18 add no text of their own), so the
// name cannot always be recovered from the image.  A deterministic
// "<handle>#<idx>" placeholder reproduces the index sequence, but it leaves
// the index WITHOUT the names other tables resolve by name -- which is
// exactly what broke the parent treasureclassex round-trip (a calc
// expression's stat('heraldtier') against itemstatcost_stat_index).  The
// shipped txt is the ground truth the byte-exact forward compile consumed, so
// take the column from there whenever it provably reproduces the stored
// indices; otherwise keep the placeholder.
const std::vector<std::string>& IndexColumnText(Decomp& d, const TableDef& def,
                                                const ExcelFieldDesc& f,
                                                const uint8_t* body,
                                                uint32_t rows, int rec,
                                                const char* handle) {
  std::string key = std::string(def.name) + '\x1f' + f.name;
  auto cached = d.idx_col.find(key);
  if (cached != d.idx_col.end()) return cached->second;

  std::vector<std::string>& out = d.idx_col[key];
  out.assign(rows, std::string());
  auto stored = [&](uint32_t r) {
    const uint8_t* row = body + (size_t)r * rec + f.offset;
    // The index is the FIRST field of the record slot: 2 bytes for the 16-bit
    // variants, 4 for the 32-bit ones.  Types 19/20 (TXTIDX16T/32T) append the
    // raw text AFTER the index, so their index is still 16/32-bit at offset 0
    // -- reading 32 bits for a *T field folds the first two name characters
    // into the id (that was the skills_summon_index#1868824932 bug: 0x0164
    // "dopplezon" row read as 0x6F640164).
    switch (f.type) {
      case EXCEL_TYPE_TXTIDX16:
      case EXCEL_TYPE_TXTIDX16T: return (uint32_t)rd16(row);
      case EXCEL_TYPE_TXTIDX32:
      case EXCEL_TYPE_TXTIDX32T: return rd32(row);
      default: return 0xFFFFFFFFu;  // not an index column: forces placeholders
    }
  };
  auto use_placeholders = [&]() {
    for (uint32_t r = 0; r < rows; ++r)
      out[r] = std::string(handle) + "#" + std::to_string(stored(r));
  };

  const char* tn = def.txt_name ? def.txt_name : def.name;
  const std::vector<std::string>* sc = SourceColumnPtr(d, tn, f.name);
  if (!sc || sc->size() != rows) {
    use_placeholders();
    return out;
  }
  // Replay the pre-pass interning (add = true: a repeated key keeps its slot)
  // and require the result to match the stored index on every row.
  DataIndex* ix = d.eng.GetIndex(handle);
  uint32_t next = ix->str.count();
  std::map<std::string, uint32_t> seen;
  for (uint32_t i = 0; i < next; ++i)
    if (const std::string* s = ix->str.TextAt(i)) seen.emplace(IdxKey(*s), i);
  for (uint32_t r = 0; r < rows; ++r) {
    const std::string& nm = (*sc)[r];
    std::string k = IdxKey(nm);
    auto it = seen.find(k);
    uint32_t expect;
    if (it != seen.end()) {
      expect = it->second;
    } else {
      expect = next;
      seen.emplace(k, next);
      ++next;
    }
    if (expect != stored(r)) {
      use_placeholders();
      return out;
    }
    out[r] = nm;
  }
  return out;
}

// Generate one descriptor's column from the bin image.  Returns false when
// the column is absent from the original txt (every row holds the zero-init
// pattern), so the caller omits it and lets the engine's zero-init block
// reproduce those bytes exactly.
bool GenerateField(Decomp& d, const TableDef& def, const ExcelFieldDesc& f,
                   const uint8_t* body, uint32_t rows, int rec,
                   std::vector<std::string>* cells_ptr) {
  const std::string name = f.name;
  std::vector<std::string>& cells = *cells_ptr;
  cells.assign(rows, std::string());
  bool emit = true;
    // An index handle is a plain name; callbacks are named functions (the p0
    // duplicate registrations store them without the "cb:" prefix).
    const char* raw = (const char*)f.ptr;
    const char* handle = nullptr;
    const char* cb = nullptr;
    if (raw && std::strncmp(raw, "cb:", 3) == 0) cb = raw + 3;
    else if (raw && std::strncmp(raw, "sub_", 4) == 0) cb = raw;
    else handle = raw;

    // `all_sentinel` / `all_zero` accumulate the "column absent from the
    // original txt" signal: every row holds the zero-init pattern.
    bool all_sentinel = true, all_zero = true;

    switch (f.type) {
      case EXCEL_TYPE_TEXT: case EXCEL_TYPE_TEXT7: {
        // the main pass copies up to len chars then writes the NUL
        for (uint32_t r = 0; r < rows; ++r)
          txt_of(body + (size_t)r * rec, (size_t)f.offset,
                 (size_t)(f.len > 0 ? f.len : 0), &cells[r]);
        break;
      }
      case EXCEL_TYPE_INT32: case EXCEL_TYPE_INT32B: {
        for (uint32_t r = 0; r < rows; ++r) {
          int32_t v = rdi32(body + (size_t)r * rec + f.offset);
          cells[r] = std::to_string(v);
        }
        break;
      }
      case EXCEL_TYPE_INT16: {
        for (uint32_t r = 0; r < rows; ++r)
          cells[r] = std::to_string(rdi16(body + (size_t)r * rec + f.offset));
        break;
      }
      case EXCEL_TYPE_INT8: case EXCEL_TYPE_BYTE_OR: case EXCEL_TYPE_INT8B: {
        for (uint32_t r = 0; r < rows; ++r)
          cells[r] = std::to_string(body[(size_t)r * rec + f.offset]);
        break;
      }
      case EXCEL_TYPE_CODE: case EXCEL_TYPE_CODE_IDX32:
      case EXCEL_TYPE_CODE_IDX8: case EXCEL_TYPE_CODE_IDX16: {
        size_t n = (f.type == EXCEL_TYPE_CODE || f.type == EXCEL_TYPE_CODE_IDX32)
                       ? 4
                       : (f.type == EXCEL_TYPE_CODE_IDX8 ? 1 : 2);
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          bool z = true;
          for (size_t k = 0; k < n; ++k) if (row[k]) z = false;
          if (z) continue;      // zero row keeps the absent-column signal
          all_zero = false;
          uint32_t code = (uint32_t)row[0] | ((uint32_t)row[1] << 8) |
                          ((uint32_t)row[2] << 16) | ((uint32_t)row[3] << 24);
          cells[r] = UnpackCode(code);
        }
        if (all_zero) emit = false;
        break;
      }
      case EXCEL_TYPE_CODE_REF32: case EXCEL_TYPE_CODE_REF8:
      case EXCEL_TYPE_CODE_REF16: {
        uint32_t sentinel = f.type == EXCEL_TYPE_CODE_REF32
                                ? 0xFFFFFFFFu
                                : (f.type == EXCEL_TYPE_CODE_REF8 ? 0xFFu : 0xFFFFu);
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t v = f.type == EXCEL_TYPE_CODE_REF32
                           ? rd32(row)
                           : (f.type == EXCEL_TYPE_CODE_REF8 ? row[0] : rd16(row));
          if (v != sentinel) all_sentinel = false;
        }
        if (all_sentinel) { emit = false; break; }
        if (!handle) { emit = false; break; }
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t v = f.type == EXCEL_TYPE_CODE_REF32
                           ? rd32(row)
                           : (f.type == EXCEL_TYPE_CODE_REF8 ? row[0] : rd16(row));
          if (v == sentinel) cells[r] = kLookupMissCode;
          else if (!d.CodeText(handle, v, &cells[r])) {
            d.warn(std::string(def.name) + "." + name + ": no code at index " +
                   std::to_string(v));
            cells[r] = kLookupMissCode;
          }
        }
        break;
      }
      case EXCEL_TYPE_TEXT16: {
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          if (!row[0]) continue;   // zero row keeps the absent-column signal
          all_zero = false;
          txt_of(row, 0, (size_t)(f.len > 0 ? f.len - 1 : 0), &cells[r]);
        }
        if (all_zero) emit = false;
        break;
      }
      case EXCEL_TYPE_TXTIDX16: case EXCEL_TYPE_TXTIDX32: {
        if (!handle) { emit = false; break; }
        const std::vector<std::string>& names =
            IndexColumnText(d, def, f, body, rows, rec, handle);
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t idx = f.type == EXCEL_TYPE_TXTIDX16 ? rd16(row) : rd32(row);
          d.Register(handle, idx, names[r], std::string(def.name) + "." + name);
          cells[r] = names[r];
        }
        break;
      }
      case EXCEL_TYPE_TXTIDX16T: case EXCEL_TYPE_TXTIDX32T: {
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t idx = f.type == EXCEL_TYPE_TXTIDX16T ? rd16(row) : rd32(row);
          size_t shift = f.type == EXCEL_TYPE_TXTIDX16T ? 2 : 4;
          txt_of(row, shift, (size_t)(f.len > 0 ? f.len : 48), &cells[r]);
          if (handle) {
            // the pre-pass interns at most 0x30 chars (rowbuf bound)
            d.Register(handle, idx, cells[r].substr(0, 0x30),
                       std::string(def.name) + "." + name);
          }
        }
        break;
      }
      case EXCEL_TYPE_STRREF32: case EXCEL_TYPE_STRREF16:
      case EXCEL_TYPE_STRREF8: {
        uint32_t sentinel = f.type == EXCEL_TYPE_STRREF32
                                ? 0xFFFFFFFFu
                                : (f.type == EXCEL_TYPE_STRREF16 ? 0xFFFFu : 0xFFu);
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t v = f.type == EXCEL_TYPE_STRREF32
                           ? rd32(row)
                           : (f.type == EXCEL_TYPE_STRREF16 ? rd16(row) : row[0]);
          if (v != sentinel) all_sentinel = false;
        }
        if (all_sentinel || !handle) { emit = false; break; }
        uint32_t empty = eng_empty_index(d, handle, f.type);
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t v = f.type == EXCEL_TYPE_STRREF32
                           ? rd32(row)
                           : (f.type == EXCEL_TYPE_STRREF16 ? rd16(row) : row[0]);
          if (v == empty) { cells[r] = ""; continue; }
          if (v == sentinel) { cells[r] = kLookupMissStr; continue; }
          if (!d.StrText(handle, v, &cells[r])) {
            d.warn(std::string(def.name) + "." + name + ": no string at index " +
                   std::to_string(v));
            cells[r] = kLookupMissStr;
          }
        }
        break;
      }
      case EXCEL_TYPE_STRCALL16: case EXCEL_TYPE_STRCALL32: {
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t v = f.type == EXCEL_TYPE_STRCALL16 ? rd16(row) : rd32(row);
          if (v) all_zero = false;
        }
        if (all_zero) { emit = false; break; }
        for (uint32_t r = 0; r < rows; ++r) {
          const uint8_t* row = body + (size_t)r * rec + f.offset;
          uint32_t v = f.type == EXCEL_TYPE_STRCALL16 ? rd16(row) : rd32(row);
          auto it = d.translation.find(v);
          if (it != d.translation.end()) cells[r] = it->second;
          else {
            d.warn(std::string(def.name) + "." + name + ": no translation for id " +
                   std::to_string(v));
            cells[r] = "";
          }
        }
        break;
      }
      case EXCEL_TYPE_BOOL: case EXCEL_TYPE_INVBOOL: {
        for (uint32_t r = 0; r < rows; ++r) {
          // bit (len & 7) of the byte at row + offset + len/8 (sub_1419D6EA0)
          const uint8_t* row = body + (size_t)r * rec;
          uint8_t byte = row[(size_t)f.offset + (size_t)(f.len) / 8];
          int bit = (byte >> (f.len & 7)) & 1;
          cells[r] = std::to_string(f.type == EXCEL_TYPE_BOOL ? bit : !bit);
        }
        break;
      }
      case EXCEL_TYPE_PARSE26: case EXCEL_TYPE_PARSE27: case EXCEL_TYPE_PARSE28: {
        for (uint32_t r = 0; r < rows; ++r) {
          // Pass the ROW START: the engine callbacks apply f.offset themselves.
          const uint8_t* row = body + (size_t)r * rec;
          cells[r] = InvertCallback(d, def, f, row, handle, cb, r);
        }
        break;
      }
      default:
        d.warn(std::string(def.name) + "." + name + ": unhandled type " +
               std::to_string(f.type));
        emit = false;
        break;
    }
  return emit;
}

// helper declared above use (C++ needs it before GenerateTable; forward decl).
uint32_t eng_empty_index(Decomp& d, const char* handle, int type);

uint32_t eng_empty_index(Decomp& d, const char* handle, int type) {
  uint32_t e = d.eng.GetIndex(handle)->str.empty_index();
  if (type == EXCEL_TYPE_STRREF16) return (uint16_t)e;
  if (type == EXCEL_TYPE_STRREF8) return (uint8_t)e;
  return e;
}

// `row` is the ROW START (record base).  Every engine callback receives the row
// start plus its descriptor offset (RunParseCallback: f(0, text, row, d.offset,
// ...) and the calc pages write at row + d.offset), so the inverse must apply
// f.offset exactly the same way.  Passing row+offset here (as an earlier
// revision did) double-counts it for callbacks that use the offset internally.
std::string InvertCallback(Decomp& d, const TableDef& def,
                           const ExcelFieldDesc& f, const uint8_t* row,
                           const char* handle, const char* cb, uint32_t r) {
  const std::string name = f.name;
  const size_t O = (size_t)f.offset;   // descriptor offset (row-relative)
  // calc-expression pages
  if (cb) {
    const char* page = nullptr;
    CalcFamilyKind fam = CALC_FAM_SKILLS;
    if (std::strcmp(cb, "calc_skills") == 0) { page = "skillscode"; fam = CALC_FAM_SKILLS; }
    else if (std::strcmp(cb, "calc_skilldesc") == 0) { page = "skilldesccode"; fam = CALC_FAM_SKILLDESC; }
    else if (std::strcmp(cb, "calc_missile") == 0) { page = "misscode"; fam = CALC_FAM_MISSILES; }
    else if (std::strcmp(cb, "calc_item") == 0) { page = "itemscode"; fam = CALC_FAM_ITEMS; }
    else if (std::strcmp(cb, "calc_monpet") == 0) { page = "moncode"; fam = CALC_FAM_MONPET; }
    else if (std::strcmp(cb, "calc_condition") == 0) { page = "tccode"; fam = CALC_FAM_ITEMS2; }
    if (page) {
      uint32_t off = rd32(row + O);
      if (off == 0xFFFFFFFFu) return "";
      const std::vector<uint8_t>* pg = d.Page(page);
      if (!pg) {
        // The page is shipped (tccode.bin / moncode.bin, the index objects
        // registered at dataTable+0xD90 and dataTable+0x12C0); only a tree
        // that lacks the file lands here.  The non-sentinel offset proves the
        // source cell was non-empty, so fall back to the txt column.
        const char* tn = def.txt_name ? def.txt_name : def.name;
        const std::vector<std::string>* sc = SourceColumnPtr(d, tn, name.c_str());
        if (sc && r < sc->size() && !(*sc)[r].empty()) {
          // Opt-in self-check (D2DT_CALCCHECK): a runtime-generated page has no
          // shipped bytes to diff, so just report the recompiled encoding length
          // of the source cell.
          if (std::getenv("D2DT_CALCCHECK")) {
            uint8_t buf[512];
            int n = CompileCalcExpression(d.eng, d.eng.version(), fam,
                                          (*sc)[r].c_str(), buf, (int)sizeof(buf));
            std::fprintf(stderr, "SRCCALC %s.%s row=%u off=%u n=%d text=[%s]\n",
                         def.name, name.c_str(), r, off, n, (*sc)[r].c_str());
          }
          return (*sc)[r];
        }
        d.warn(std::string(def.name) + "." + name + ": page " + page +
               " not shipped and no source cell (offset " + std::to_string(off) + ")");
        return "";
      }
      size_t end = 0;
      std::string text = DisasmCalc(d, *pg, off, fam, &end);
      if (text.empty())
        d.warn(std::string(def.name) + "." + name + ": failed to disassemble " + page +
               " at " + std::to_string(off));
      // Opt-in self-check (D2DT_CALCCHECK): recompile the disassembled text and
      // diff it byte-for-byte against the shipped page; silent when it agrees.
      if (std::getenv("D2DT_CALCCHECK") && !text.empty()) {
        uint8_t buf[512];
        int n = CompileCalcExpression(d.eng, d.eng.version(), fam, text.c_str(), buf,
                                      (int)sizeof(buf));
        size_t want = end - off;
        if (n <= 0 || (size_t)n != want || std::memcmp(buf, pg->data() + off, (size_t)n)) {
          std::string hx, hx2;
          for (size_t k = 0; k < want; ++k) hx += std::to_string(pg->data()[off + k]) + " ";
          for (int k = 0; k < (n > 0 ? n : 0); ++k) hx2 += std::to_string(buf[k]) + " ";
          std::fprintf(stderr, "CALCCHECK %s.%s %s off=%u recomp=%d shipped=%zu\n  text=[%s]\n  got=[%s]\n  want=[%s]\n",
                       def.name, name.c_str(), page, off, n, want, text.c_str(), hx2.c_str(), hx.c_str());
        }
      }
      return text;
    }
  }
  if (cb && std::strcmp(cb, "calc_property") == 0) {
    int16_t idx = rdi16(row + O), flag = rdi16(row + O + 2);
    if (idx == -1 && flag == -1) return "";
    const char* h = flag == 0 ? "properties_code_index" : "propertygroups_code_index";
    std::string s;
    if (idx >= 0 && d.StrText(h, (uint32_t)idx, &s)) return s;
    d.warn(std::string(def.name) + "." + name + ": missing property name " +
           std::to_string(idx));
    return "";
  }
  if (cb && std::strcmp(cb, "calc_mod_param") == 0) {
    int32_t v = rdi32(row + O);
    std::string s;
    const char* hs[3] = {"skills_skill_index", "montype_type_index", "states_state_index"};
    if (v >= 0)
      for (const char* h : hs)
        if (d.StrText(h, (uint32_t)v, &s)) return s;
    return std::to_string(v);
  }
  if (cb && std::strcmp(cb, "parse_monster_ref") == 0) {
    uint16_t kind = rd16(row + O), idx = rd16(row + O + 2);
    if (kind == 0 && idx == 0xFFFF) return "";
    const char* h = kind == 2 ? "superuniques_superunique_index" : (kind == 1 ? "monstats_id_index" : nullptr);
    std::string s;
    if (h && d.StrText(h, idx, &s)) return s;
    d.warn(std::string(def.name) + "." + name + ": cannot invert monster ref");
    return "";
  }
  if (cb && std::strcmp(cb, "parse_place_ref") == 0) {
    uint8_t kind = (uint8_t)row[1];
    uint16_t idx = rd16(row + 2);
    if (kind == 0 && idx == 0) return "";
    const char* h = kind == 2 ? "superuniques_superunique_index" : (kind == 1 ? "monstats_id_index" : "monplace_index");
    std::string s;
    if (d.StrText(h, idx, &s)) return s;
    d.warn(std::string(def.name) + "." + name + ": cannot invert place ref");
    return "";
  }
  if (cb && std::strcmp(cb, "parse_variations") == 0) {
    unsigned idx = (unsigned)f.offset;
    if (idx >= 16) return "";
    uint8_t count = (uint8_t)row[idx + 21];
    if (!count) return "";
    std::string out;
    for (uint8_t k = 0; k < count && k < 12; ++k) {
      uint8_t code = (uint8_t)row[12 * idx + 38 + k];
      std::string s;
      if (code == 0xFF) {
        // a failed strict lookup stores 0xFF (sub_140A9AA40); reproduce it
        // with a token that never resolves (empty would truncate the list).
        s = kVariationMissCode;
      } else if (!d.CodeText("compcode_code_index", code, &s)) {
        d.warn(std::string(def.name) + "." + name + ": no variation code " +
               std::to_string(code));
        s = kVariationMissCode;
      }
      if (!out.empty()) out += ",";
      out += s;
    }
    return out;
  }
  if (cb && std::strcmp(cb, "parse_monstats_sound") == 0) {
    unsigned idx = (unsigned)f.offset;
    if (idx >= 8) return "";
    int16_t guard = rdi16(row + 2 * idx + 452);
    if (guard < 0) return "";
    uint8_t b = (uint8_t)row[idx + 468];
    uint16_t s = rd16(row + 2 * idx + 476);
    std::string t;
    if (b == 14) {
      if (!d.StrText("monseq_sequence_index", s, &t)) {
        d.warn(std::string(def.name) + "." + name + ": no sound name " + std::to_string(s));
        return "";
      }
      return t;
    }
    if (!d.CodeText("monmode_code_index", b, &t)) {
      d.warn(std::string(def.name) + "." + name + ": no sound code " + std::to_string(b));
      return "";
    }
    return t;
  }
  if (cb && std::strcmp(cb, "parse_cube_input") == 0) {
    return InvertCubeInput(d, row + 8 * (int64_t)f.offset + 20);
  }
  if (cb && std::strcmp(cb, "parse_cube_output") == 0) {
    return InvertCubeOutput(d, row + 89 * (int64_t)f.offset + 76, r);
  }
  if (cb && std::strcmp(cb, "parse_cube_param") == 0) {
    int32_t v = rdi32(row + O);
    std::string s;
    if (v >= 0 && d.StrText("itemstatcost_stat_index", (uint32_t)v, &s)) return s;
    return std::to_string(v);
  }
  // no callback (or unknown): the field writes nothing -> empty cell
  return "";
}

// ------------------------------------------------- kind-4 replay bin alias
//
// An early (kind-4) registration can serialise to a file of its OWN, distinct
// from the table's shipped .bin.  Ground truth, sub_140933080:
//   arg5 -> the bin-only READ path  ("<arg5>.bin", via sub_140925290)
//   arg6 -> the build-mode WRITE path ("<arg6>.bin", via sub_140932B30, and
//           only when GetGlobalInitParams()->direct is set and arg6 non-empty)
// Every registration in the sub_1409257D0 code-index pre-pass passes an EMPTY
// 5th/6th argument except one:
//
//   sub_1409257D0: sub_14018CDDE(out, inst, "actinfo", {act,10,0,0},
//                                "actcode", "actcode")
//
// so actinfo's code-index pass reads and writes actcode.bin -- and that file
// ships.  Its record is self-describing: the early registration has a single
// type-10 field at offset 0, and type 10 stores the raw 4-char code, so
// actcode.bin is exactly the serialised `act` column (5 rows x 4 bytes).
//
// plrmode/monmode pass empty strings for BOTH, so their code-index passes touch
// no file at all: nothing is read in bin-only mode and nothing is written in
// build mode, and no shipped file contains the column.
const char* Kind4AliasBin(const char* table) {
  if (std::strcmp(table, "actinfo") == 0) return "actcode";
  return nullptr;
}

// Rebuild one early-registration column from its alias .bin.  Returns false
// when the table has no alias, the file is absent, or the row count does not
// line up with the table's own (the column is written row-for-row into the
// shared txt).
bool AliasBinColumn(Decomp& d, const TableDef& def, const ExcelFieldDesc& f,
                    uint32_t rows, std::vector<std::string>* cells) {
  const char* alias = Kind4AliasBin(def.name);
  if (!alias || rows == 0) return false;
  std::vector<uint8_t> ab;
  if (!d.ReadBin(alias, &ab) || ab.size() < 4) return false;
  uint32_t ar = rd32(ab.data());
  if (ar != rows) return false;
  size_t body = ab.size() - 4;
  if (body % ar) return false;
  int rec = (int)(body / ar);
  if (rec <= 0) return false;
  return GenerateField(d, def, f, ab.data() + 4, rows, rec, cells);
}

// Deterministic placeholder for a kind-4 code column that has no shipped
// source at all (plrmode.code, monmode.code): their early registration names
// no .bin of its own, so neither the table's record nor any other file in the
// data set holds the value.
//
// The column exists only to fill a code index, and every consumer stores an
// INDEX into it.  Reproducing the exact index is therefore all the round trip
// needs -- the index must simply stay a bijection with the rows.  Fabricating
// the cell from the colliding record bytes cannot do that: it yields NULs
// (which corrupt the txt) and duplicates (plrmode rows 18/19 and monmode row
// 13 share the "GH" token; monmode rows 5/6 share the first four name bytes).
//
// So emit '#' + the row number in base 36, padded to 2 digits: exactly three
// characters, injective for up to 1296 rows, and unmistakably a placeholder --
// the same convention IndexColumnText uses for an unrecoverable type 17/18
// name.  Three, not four: sub_140A905C0 (the monstats SkNmode callback) only
// treats a cell of <= 3 characters as a mode code, and a longer one silently
// falls through to its "sequence name" branch and stores 14.  Supplying the
// real .txt replaces the placeholder with the true codes.
bool PlaceholderCodeColumn(uint32_t rows, std::vector<std::string>* cells) {
  static const char* kD36 = "0123456789abcdefghijklmnopqrstuvwxyz";
  if (rows == 0 || rows > 1296) return false;
  cells->assign(rows, std::string());
  for (uint32_t r = 0; r < rows; ++r) {
    char b[4];
    b[0] = '#';
    b[1] = kD36[(r / 36) % 36];
    b[2] = kD36[r % 36];
    b[3] = 0;
    (*cells)[r] = b;
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------ index seeds
namespace {

// data\global\excel\base -> data\global\excel (returned unchanged when in_dir
// is not a base layer).  See ReseedSoundsIndexFromParent for why the parent
// copy of the sound tables is the one that defines the index space.
static std::string ParentExcelDir(const std::string& in_dir) {
  std::string parent_dir = in_dir;
  const char* markers[] = {"/base", "\\base"};
  for (const char* m : markers) {
    size_t pos = parent_dir.rfind(m);
    if (pos != std::string::npos && pos + std::strlen(m) == parent_dir.size()) {
      parent_dir.resize(pos);
      break;
    }
  }
  return parent_dir;
}

// Mirror of SeedIndexFromColumn in main.cpp: replay one index-bearing column
// into its exact global handle, but recover the cell text from the compiled
// image instead of the (absent) txt.
//
// `type`/`len` describe the game's EARLY registration for this column (the one
// that fills the shared handle) and therefore drive only the prepass interning
// semantics.  The .bin, however, is written by the table's LATE registration,
// whose field list -- and record layout -- can differ: monstats.Id is type 17
// index-only early, but type 19 (TXTIDX16T: u16 index followed by the text) in
// the bin.  So whether the text can be read straight out of the image is
// decided by the DESCRIPTOR (the late registration), never by `type`; trusting
// `type` here is what produced the `skills_summon_index#1868824932` garbage
// (a 32-bit read over a [u16 index][text] record mixed the monster name into
// the id).  Late types with inline text (TEXT/TEXT7/TEXT16/TXTIDX16T/
// TXTIDX32T) are read directly; the index-only 17/18 fall back to
// IndexColumnText's source-txt replay.
bool SeedColumnFromBin(Decomp& d, const char* table, const char* field,
                       int type, int len, const char* handle,
                       const std::string& bin_path = std::string()) {
  const TableDef* def = d.FindTable(table);
  const ExcelFieldDesc* f = d.FindField(def, field);
  std::vector<uint8_t> bin;
  std::string path = bin_path.empty() ? (d.in_dir + "/" + table + ".bin") : bin_path;
  if (!f || !Decomp::ReadFile(path, bin) || bin.size() < 4) {
    d.warn(std::string("seed: missing ") + table + "." + field);
    return false;
  }
  uint32_t rows = rd32(bin.data());
  int rec = def->record_size;
  if (bin.size() < (size_t)4 + (size_t)rows * rec) {
    d.warn(std::string("seed: truncated ") + table + ".bin");
    return false;
  }
  const uint8_t* body = bin.data() + 4;

  const int late = f->type;
  std::vector<std::string> cells(rows);
  for (uint32_t r = 0; r < rows; ++r) {
    const uint8_t* row = body + (size_t)r * rec + f->offset;
    if (late == EXCEL_TYPE_TEXT || late == EXCEL_TYPE_TEXT7 ||
        late == EXCEL_TYPE_TEXT16) {
      size_t max = (size_t)(f->len > 0 ? f->len : 48);
      std::string& s = cells[r];
      for (size_t k = 0; k < max && row[k]; ++k) s.push_back((char)row[k]);
    } else if (late == EXCEL_TYPE_TXTIDX16T || late == EXCEL_TYPE_TXTIDX32T) {
      size_t shift = (late == EXCEL_TYPE_TXTIDX16T) ? 2 : 4;
      size_t max = (size_t)(f->len > 0 ? f->len : 48);
      std::string& s = cells[r];
      for (size_t k = 0; k < max && row[shift + k]; ++k)
        s.push_back((char)row[shift + k]);
    } else {  // 17 / 18: only the index lives in the bin
      // Recover the real names from the shipped txt so the seeded index holds
      // names, not "<handle>#<idx>" placeholders (later tables resolve these
      // indexes by name).
      cells[r] = IndexColumnText(d, *def, *f, body, rows, rec, handle)[r];
    }
  }

  std::string txt = std::string(field) + "\n";
  for (uint32_t r = 0; r < rows; ++r) {
    txt += cells[r];
    txt += '\n';
  }
  const ExcelFieldDesc descs[] = {
      { field, type, len, 0, (void*)handle },
      { nullptr, 0, 0, 0, nullptr },
  };
  TableDef prepass = { table, table, descs, rec, 0 };
  std::vector<char> tvec(txt.begin(), txt.end());
  std::vector<uint8_t> ignored;
  std::string err;
  if (!d.eng.Compile(prepass, std::move(tvec), ignored, &err)) {
    d.warn(std::string("seed compile failed for ") + table + ": " + err);
    return false;
  }
  return true;
}

// The game builds sounds_sound_index from the merged base+parent sounds in
// PARENT (expansion) order; base-layer bins (base/skills.bin stsound,
// base/monsounds.bin Taunt) are stale copies of the expansion (parent) files
// and reference those parent-shaped positions.  A base layer that ships no
// sounds.bin of its own can never build the index itself, so rebuild the
// PARENT-shaped index from the PARENT's SHIPPED sounds.bin (ground-truth
// bytes -- we never read a txt here) via SeedColumnFromBin, which interns the
// Sound names exactly like the game's forward prepass (first-wins, in parent
// source/bin order).
//
// Scoped strictly to layers that cannot build their own index: if this layer
// ships its own sounds.bin it builds sounds_sound_index in its own order while
// the sounds table is generated (3.1.1 base does), and the parent-shaped
// switch happens afterwards, in the main loop (ReseedSoundsIndexFromParent).
// The parent set is seeded by kTables itself.
void SeedSoundsIndex(Decomp& d, const std::string& in_dir) {
  std::string parent_dir = ParentExcelDir(in_dir);
  if (parent_dir == in_dir) return;  // parent (expansion) set: kTables seeds it
  // This layer builds its own index from its own sounds.bin; never perturb it.
  { std::vector<uint8_t> own; if (Decomp::ReadFile(in_dir + "/sounds.bin", own)) return; }
  // SeedColumnFromBin reads the parent's shipped bin and interns the Sound names
  // into sounds_sound_index in parent order -- the parent-shaped index the
  // layer's own bins (skills.stsound, monsounds.Taunt) reference.
  SeedColumnFromBin(d, "sounds", "Sound", EXCEL_TYPE_TXTIDX16T, 47,
                   "sounds_sound_index", parent_dir + "/sounds.bin");
}

// Engine boundary: the two sound tables are PINNED to the parent folder --
// sub_140925410 (d2r.exe 3.1.1) / sub_1401FC290 (D2R_93854_RAC_China.exe 3.3)
// force version 3 when the table name is "sounds"/"soundenviron", and
// sub_140422860 (3.1.1) / SoundDataTablesInit (3.3) pass a hardcoded 3u for
// them.  The engine therefore resolves -- and inverts -- every sound
// reference against the PARENT list, in every pass including the base one.
//
// A base layer that ships its own sounds.bin first builds sounds_sound_index
// in its own (base) order so that its sounds/soundenviron tables generate and
// round-trip against their own bins (both files are offline artifacts the
// engine never loads).  Once those two tables are done, reset the handle and
// rebuild it from the parent's SHIPPED sounds.bin: every remaining consumer
// (missiles, overlay, states, armor, weapons, misc, skills.stsound, monstats,
// monsounds, uniqueitems, setitems) is loaded from the base folder but stores
// parent-shaped indices, so only the parent-shaped index space can both invert
// its cells correctly and round-trip them byte-exactly.
void ReseedSoundsIndexFromParent(Decomp& d, const std::string& in_dir) {
  std::string parent_dir = ParentExcelDir(in_dir);
  if (parent_dir == in_dir) return;  // parent set: already parent-shaped
  std::vector<uint8_t> probe;
  if (!Decomp::ReadFile(parent_dir + "/sounds.bin", probe) || probe.size() < 4) {
    d.warn("sounds: parent sounds.bin unreadable; keeping the layer-shaped index");
    return;
  }
  DataIndex* ix = d.eng.GetIndex("sounds_sound_index");
  if (ix) ix->str.Reset();
  SeedColumnFromBin(d, "sounds", "Sound", EXCEL_TYPE_TXTIDX16T, 47,
                    "sounds_sound_index", parent_dir + "/sounds.bin");
}

const TableDef* FindFinal(const char* name) {
  for (int i = 0; i < kTableCount; ++i)
    if (kTables[i].kind == 0 && std::strcmp(kTables[i].name, name) == 0)
      return &kTables[i];
  return nullptr;
}

std::string JoinCells(const std::vector<std::string>& cells) {
  std::string line;
  // NOTE: the separator must not depend on line content - consecutive empty
  // cells are legal and each still needs its own tab.
  for (size_t i = 0; i < cells.size(); ++i) {
    if (i) line += '\t';
    line += cells[i];
  }
  return line;
}

// Append columns to an existing decompiled txt (carved sub-tables share one
// source file with their parent: leveldefs->levels, objecttokens->objects).
bool MergeFile(const std::string& path, const std::vector<std::string>& header,
               const std::vector<std::vector<std::string>>& rows) {
  std::vector<std::string> lines;
  {
    std::string content;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    content.resize((size_t)(n < 0 ? 0 : n));
    if (n > 0) { size_t rd = fread(&content[0], 1, content.size(), f); (void)rd; }
    fclose(f);
    std::string cur;
    for (char c : content) {
      if (c == '\n') { lines.push_back(cur); cur.clear(); }
      else if (c != '\r') cur.push_back(c);
    }
    if (!cur.empty()) lines.push_back(cur);
  }
  if (lines.size() != rows.size() + 1) return false;
  std::string extra = JoinCells(header);
  lines[0] += '\t';
  lines[0] += extra;
  for (size_t r = 0; r < rows.size(); ++r) {
    lines[r + 1] += '\t';
    lines[r + 1] += JoinCells(rows[r]);
  }
  std::string out;
  for (const auto& l : lines) { out += l; out += '\n'; }
  return Decomp::WriteFile(path, out);
}

std::string BuildFileContent(const std::vector<std::string>& header,
                             const std::vector<std::vector<std::string>>& rows) {
  std::string out = JoinCells(header);
  out += '\n';
  for (const auto& row : rows) {
    out += JoinCells(row);
    out += '\n';
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- pipeline

int RunDecompile(Engine& eng, const std::string& in_dir,
                 const std::string& out_dir, const std::string& only) {
  Decomp d{eng, in_dir, out_dir};

  // Localisation reverse map (id -> key).  The engine already loaded the game's
  // own json localisation (or the tsv fallback) for this input dir via
  // LoadStrings(in_dir); reuse its reverse map so compile and decompile agree
  // exactly on every Key<->id pairing.
  {
    const auto* rev = eng.reverse_translation();
    if (rev) {
      for (const auto& kv : *rev)
        if (!d.translation.count(kv.first)) d.translation[kv.first] = kv.second;
    }
  }
  fs::create_directories(out_dir);

  // Index seeds, mirroring main.cpp's compile pipeline exactly.  Each
  // (type, len) pair describes the game's EARLY registration for the column --
  // the one that fills the shared handle -- and therefore only drives the
  // prepass interning semantics; how the text is recovered from the image is
  // decided inside SeedColumnFromBin from the LATE registration's descriptor
  // (see the comment there).  monstats.Id is the clearest case: the early
  // registration (sub_140928A60, d2r.exe 3.1.1 @ 0x14092F0A9) declares it
  // type 17 index-only into the shared handle at dataTable+0x26D8 -- the very
  // index skills.summon (type 22 STRRREF16, offset 0x110=272) looks up -- while
  // the shipped monstats.bin follows the late registration's type 19
  // (TXTIDX16T) layout, which is where the names are read from.
  SeedColumnFromBin(d, "skills", "skill", 19, 31, "skills_skill_index");
  SeedColumnFromBin(d, "monseq", "sequence", 17, 0, "monseq_sequence_index");
  SeedColumnFromBin(d, "monstats2", "Id", 17, 0, "monstats2_id_index");
  SeedColumnFromBin(d, "monprop", "Id", 18, 0, "monprop_id_index");
  SeedColumnFromBin(d, "monsounds", "Id", 17, 0, "monsounds_id_index");
  SeedColumnFromBin(d, "superuniques", "Superunique", 17, 0, "superuniques_superunique_index");
  SeedColumnFromBin(d, "monumod", "uniquemod", 17, 0, "monumod_uniquemod_index");
  SeedColumnFromBin(d, "skilldesc", "skilldesc", 17, 0, "skilldesc_skilldesc_index");
  SeedColumnFromBin(d, "skilldesc", "skilldesc", 17, 0, "skills_skilldesc_index");
  SeedColumnFromBin(d, "monstats", "Id", 17, 0, "skills_summon_index");
  SeedColumnFromBin(d, "uniqueitems", "index", 16, 32, "uniqueitems_name_index");
  SeedColumnFromBin(d, "setitems", "index", 16, 32, "setitems_name_index");
  // Base-layer bins (skills.stsound, monsounds.Taunt) reference the PARENT-
  // shaped sounds_sound_index; without it those STRREF inversions miss.
  SeedSoundsIndex(d, in_dir);
  // Does this layer ship its own sounds.bin?  If so its sounds/soundenviron
  // tables build sounds_sound_index in their own (base) order while they
  // generate, and the parent-shaped switch for the remaining consumers
  // happens in the loop below, once those two tables are fully round-tripped.
  bool own_sounds_bin = false;
  { std::vector<uint8_t> b; if (Decomp::ReadFile(in_dir + "/sounds.bin", b)) own_sounds_bin = true; }
  // Slot of the soundenviron table (kind 0): sounds sits immediately before
  // it, and every sound consumer sits after it.
  int soundenviron_slot = -1;
  for (int i = 0; i < kTableCount; ++i)
    if (kTables[i].kind == 0 && std::strcmp(kTables[i].name, "soundenviron") == 0) {
      soundenviron_slot = i;
      break;
    }
  bool sounds_index_switched = false;
  {
    // sub_140928A60 pushes five range codes before registering skills.
    DataIndex* ix = eng.GetIndex("skills_range_code_index");
    static const char* kRangeCodes[5] = {"none", "h2h", "rng", "both", "loc"};
    for (const char* rc : kRangeCodes) ix->code.Add(PackCodeU32(rc));
  }

  int ok = 0, mismatch = 0, skipped = 0;
  for (int i = 0; i < kTableCount; ++i) {
    const TableDef& def = kTables[i];
    // Engine boundary: the layer's own sounds/soundenviron tables have now
    // generated (and round-tripped) against their self-shaped index, so the
    // remaining consumers must resolve against the PARENT-shaped index the
    // engine actually uses.  Switch the shared handle exactly once.  Layers
    // that ship no sounds.bin were pre-seeded parent-shaped above and never
    // built a self-shaped index, so they are left alone.
    if (soundenviron_slot >= 0 && i > soundenviron_slot &&
        !sounds_index_switched && own_sounds_bin) {
      sounds_index_switched = true;
      ReseedSoundsIndexFromParent(d, in_dir);
    }
    if (!only.empty() && only != def.name) continue;

    if (def.kind == 2) {
      // Opaque code tables have no txt; carry the image through (the compile
      // command round-trips it the same way).
      std::vector<uint8_t> raw;
      if (d.ReadBin(def.name, &raw))
        Decomp::WriteFile(out_dir + "/" + def.name + ".bin", raw);
      continue;
    }
    if (def.kind == 3) continue;  // calc pages regenerate at compile time
    if (def.kind == 5) {
      std::vector<uint8_t> it, tc;
      if (d.ReadBin("itemtypes", &it) && d.ReadBin("treasureclassex", &tc))
        eng.BuildTreasureClassIndexFromBins(it, tc);
      else
        d.warn("tcindex: missing itemtypes/treasureclassex bins");
      continue;
    }

    // kind 0 (real table) and kind 4 (duplicate registration replay).
    const TableDef* gen = (def.kind == 0) ? &def : FindFinal(def.name);
    if (!gen) {
      d.warn(std::string(def.name) + ": no kind-0 descriptor for replay");
      continue;
    }
    std::vector<uint8_t> bin;
    if (!d.ReadBin(def.name, &bin)) {
      // No shipped .bin: this is a txt-only provider table (it owns a code or
      // string index that other tables reference, but the game never serialised
      // it).  Its .txt is the only ground truth, so pass it through verbatim
      // and compile it so the indexes it owns advance exactly like the forward
      // pass.  Without this, elemtypes_code_index/colors_code_index/bodylocs_code_index/monai_ai_index/events_event_index/skillcalc_code_index/misscalc_code_index stay
      // empty and every lookup that targets them fails to invert.
      std::string src = in_dir + "/" +
                        (def.txt_name ? def.txt_name : def.name) + ".txt";
      std::vector<uint8_t> raw;
      if (!d.ReadFile(src, raw)) { ++skipped; continue; }
      std::string ptxt(raw.begin(), raw.end());
      Decomp::WriteFile(out_dir + "/" +
                        (def.txt_name ? def.txt_name : def.name) + ".txt", ptxt);
      std::vector<char> tvec(ptxt.begin(), ptxt.end());
      std::vector<uint8_t> produced;
      std::string err;
      if (!eng.Compile(def, std::move(tvec), produced, &err))
        d.warn(std::string(def.name) + ": txt passthrough compile failed: " + err);
      ++ok;
      continue;
    }
    if (bin.size() < 4) { ++skipped; continue; }
    uint32_t rows = rd32(bin.data());

    std::vector<std::string> header;
    std::vector<std::vector<std::string>> cols;
    d.quiet = (def.kind != 0);
    bool gen_ok = GenerateTable(d, *gen, bin, &header, &cols);
    // A duplicate registration may cover columns the final descriptor dropped
    // (monmode's "code" field feeds the monmode_code_index code index).  The game's one
    // txt carried them for both registrations, so the replay needs them too -
    // and the same file must be written, so capture them for the kind-0 pass.
    if (gen_ok && def.kind == 4 && def.descs) {
      for (int fi = 0; def.descs[fi].name; ++fi) {
        const ExcelFieldDesc& f = def.descs[fi];
        bool found = false;
        for (const auto& h : header)
          if (IEquals(h, f.name)) { found = true; break; }
        if (found) continue;
        std::vector<std::string> cells;
        // This column exists ONLY in the early registration, so it is not part
        // of the shipped record layout: whatever sits at its offset in the
        // table's own .bin belongs to the kind-0 field sharing that slot.
        // Two ground-truth sources, in order:
        //  1. the source .txt column -- what the forward pass consumed;
        //  2. the early registration's own .bin, when it names one
        //     (sub_140933080 arg 5: actinfo -> actcode.bin).
        const char* tn = def.txt_name ? def.txt_name : def.name;
        const std::vector<std::string>* sc = SourceColumnPtr(d, tn, f.name);
        bool got = (sc && sc->size() == rows);
        if (got) cells = *sc;
        if (!got && AliasBinColumn(d, def, f, rows, &cells)) got = true;
        // A 4-char code column with neither source: emit a deterministic
        // injective placeholder rather than the bytes at the colliding offset.
        if (!got && (f.type == EXCEL_TYPE_CODE || f.type == EXCEL_TYPE_CODE_IDX32 ||
                     f.type == EXCEL_TYPE_CODE_IDX8 || f.type == EXCEL_TYPE_CODE_IDX16))
          got = PlaceholderCodeColumn(rows, &cells);
        if (!got && !GenerateField(d, def, f, bin.data() + 4, rows,
                                   def.record_size, &cells))
          continue;
        d.extra_header[def.name].push_back(f.name);
        d.extra_rows[def.name].push_back(cells);
        header.push_back(f.name);
        cols.push_back(std::move(cells));
      }
    }
    d.quiet = false;
    if (!gen_ok) { ++skipped; continue; }

    // A kind-0 table whose kind-4 twin ran earlier must publish the twin's
    // columns in the same txt (the game loads one file for both).
    if (def.kind == 0) {
      auto it = d.extra_header.find(def.name);
      if (it != d.extra_header.end()) {
        for (size_t k = 0; k < it->second.size(); ++k) {
          header.push_back(it->second[k]);
          cols.push_back(d.extra_rows[def.name][k]);
        }
      }
    }

    std::vector<std::vector<std::string>> rowlist(rows);
    for (uint32_t r = 0; r < rows; ++r)
      for (auto& col : cols)
        rowlist[r].push_back(std::move(col[r]));
    for (const auto& row : rowlist)
      if (!row.empty() && row[0] == "Expansion")
        d.warn(std::string(def.name) +
               ": first cell 'Expansion' would be dropped on recompile");

    std::string txt = BuildFileContent(header, rowlist);
    std::vector<char> tvec(txt.begin(), txt.end());
    std::string err;

    if (def.kind == 4) {
      // Replay only for its index side effects; the file itself is written
      // by the kind-0 twin (same source txt, exactly like the game).
      std::vector<uint8_t> ignored;
      if (!eng.Compile(def, std::move(tvec), ignored, &err))
        d.warn(std::string(def.name) + " (variant) replay failed: " + err);
      continue;
    }

    // kind 0: write (or merge into) the output txt, then recompile it through
    // the engine so the index state advances exactly like the forward pass.
    const char* txt_name = gen->txt_name ? gen->txt_name : gen->name;
    std::string path = out_dir + "/" + txt_name + ".txt";
    if (gen->txt_name) {
      if (!MergeFile(path, header, rowlist)) {
        d.warn(std::string(txt_name) + ".txt: merge failed (missing base or "
               "row count mismatch); writing standalone");
        Decomp::WriteFile(path, txt);
      }
    } else {
      Decomp::WriteFile(path, txt);
    }

    std::vector<uint8_t> produced;
    if (!eng.Compile(def, std::move(tvec), produced, &err)) {
      d.warn(std::string(def.name) + ": recompile failed: " + err);
      ++mismatch;
      continue;
    }
    // Opt-in dump (D2DT_PRODDIR=<dir>): write the recompiled image so it can be
    // diffed against the shipped .bin when a round-trip mismatch appears.
    if (const char* pd = std::getenv("D2DT_PRODDIR")) {
      fs::create_directories(pd);
      Decomp::WriteFile(std::string(pd) + "/" + def.name + ".bin", produced);
    }
    if (produced != bin) {
      size_t n = std::min(produced.size(), bin.size());
      size_t i = 0;
      while (i < n && produced[i] == bin[i]) ++i;
      int rec = def.record_size;
      std::string detail;
      if (i < n && i >= 4 && rec > 0) {
        size_t off = i - 4;
        detail = " row " + std::to_string(off / rec) + " rec off " +
                 std::to_string(off % rec) + " (produced " +
                 std::to_string(produced[i]) + ", shipped " +
                 std::to_string(bin[i]) + ")";
      } else {
        detail = " (size " + std::to_string(produced.size()) + " vs " +
                 std::to_string(bin.size()) + ")";
      }
      d.warn(std::string(def.name) + ": round-trip diff at byte " +
             std::to_string(i) + detail);
      ++mismatch;
    } else {
      ++ok;
    }
  }
  std::printf("decompiled=%d mismatched=%d skipped=%d warns=%ld\n",
              ok, mismatch, skipped, d.warns);
  return mismatch;
}

}  // namespace d2dt
