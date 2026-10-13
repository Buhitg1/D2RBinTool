// D2R excel datatable engine - implementation.
// Control flow mirrors the original D2R.exe functions; see the header for the
// address map.  Where the original relies on fixed-size stack buffers we use
// the same bounds so parsing behaves identically.
#include "excel_engine.h"
#include "tables.h"
#include "idx_defaults.h"
#include "calc_compiler.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstdlib>
#include <filesystem>
#include <set>

namespace d2dt {

static ParseCall LookupCallback(const char* name);

// ---------------------------------------------------------------- helpers

void PackCode(const char* src, char* dst) {  // sub_1419DAFD0
  int n = 0;
  for (; n < 4 && src[n]; ++n) dst[n] = src[n];
  for (; n < 4; ++n) dst[n] = ' ';
}

static inline uint32_t rd_u32(const void* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd_u16(const void* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void wr_u32(void* p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr_u16(void* p, uint16_t v) { memcpy(p, &v, 2); }
static inline void wr_u8(void* p, uint8_t v) { memcpy(p, &v, 1); }

// A 4-char code packed little-endian as the game compares it (u32).
static inline uint32_t code_of(const char* c) {
  return (uint8_t)c[0] | ((uint32_t)(uint8_t)c[1] << 8) |
         ((uint32_t)(uint8_t)c[2] << 16) | ((uint32_t)(uint8_t)c[3] << 24);
}

// sub_140105A4B + sub_140A8C950 - pack a <=4 char code into the u32 the code
// indexes use.
// sub_140105A4B + sub_140A8C950 - pack a <=4 char code into the u32 the code
// indexes use.
uint32_t PackCodeU32(const char* src) {
  char packed[4];
  PackCode(src, packed);
  return code_of(packed);
}

// ---------------------------------------------------------------- StringIndex

// The string index compares keys case-insensitively (charstats stores
// "fire bolt" and it resolves to the row holding "Fire Bolt").
static std::string NormKey(const char* s) {
  std::string k = s ? s : "";
  std::transform(k.begin(), k.end(), k.begin(), ::tolower);
  return k;
}

uint32_t StringIndex::Intern(const char* s, bool add) {  // sub_14164AA50
  // The insert path is an insert-or-find (sub_14164C320): when the key is
  // already present the freshly built node is DISCARDED and the existing
  // node -- carrying its ORIGINAL index -- is returned.  So the key->index
  // map always keeps the FIRST insertion, and a duplicate never rewrites it.
  // The add=false path still consumes a fresh id (the caller stores the name
  // at that id in the index->name table), it just is not what the name
  // resolves to.  Verified against shipped cubemain.bin: uniqueitems has
  // "Crainte Vomir" at rows 119/621/622 and every cube output referencing it
  // stores index 119 (0x77+1), i.e. first-wins.
  if (!s) s = "";
  std::string k = NormKey(s);
  auto it = map_.find(k);
  if (it != map_.end()) {
    if (add) return it->second;
    std::fprintf(stderr, "Duplicate string index '%s'\n", s);
    // sub_14164AA50 a3==0 duplicate path: the warning prints, then the
    // function returns a FRESH id from the counter -- but the map node keeps
    // its ORIGINAL value.  Only the caller stores the name at the fresh id
    // (index->name), so a lookup still resolves to the FIRST insertion.
    // Verified against shipped cubemain.bin: uniqueitems has "Crainte Vomir"
    // at rows 119/621/622 and every cube output referencing it stores 119
    // (0x77+1).  Rewriting map_[k] here resolved outputs to row 622 instead.
    uint32_t idx = next_id_++;
    if (by_index_.size() <= idx) by_index_.resize(idx + 1);
    by_index_[idx] = s;
    return idx;
  }
  uint32_t idx = next_id_++;
  map_[k] = idx;
  if (by_index_.size() <= idx) by_index_.resize(idx + 1);
  by_index_[idx] = s;
  return idx;
}

void StringIndex::Seed(const std::string& key, uint32_t id) {
  if (key.empty()) return;
  map_.emplace(key, id);
  if (id >= next_id_) next_id_ = id + 1;
  if (by_index_.size() <= id) by_index_.resize(id + 1);
  by_index_[id] = key;
}

uint32_t StringIndex::Lookup(const char* s, bool strict) const {  // sub_1419D9380
  if (!s || !*s) return empty_index_;
  std::string k = NormKey(s);
  auto it = map_.find(k);
  if (it == map_.end()) {
    if (strict && s[0] != '*' && std::strcmp(s, "-1") != 0)
      std::fprintf(stderr, "Failed to find string '%s' in index\n", s);
    return 0xFFFFFFFFu;
  }
  return it->second;
}

// ---------------------------------------------------------------- CodeIndex

uint32_t CodeIndex::Add(uint32_t code) {  // sub_1419D9080
  const uint32_t orig = code;
  auto it = entries_.lower_bound(code);
  while (it != entries_.end() && it->first == code) {
    if (code == orig && code > 0x202020A0u)
      std::fprintf(stderr, "Duplicate code index\n");
    ++code;
    it = entries_.lower_bound(code);
  }
  uint32_t idx = count_++;
  entries_[code] = idx;
  by_index_.push_back(code);
  return idx;
}

uint32_t CodeIndex::Lookup(uint32_t code, bool strict) const {  // sub_1419D8BC0
  auto it = entries_.find(code);
  if (it != entries_.end()) return it->second;
  if (strict && code > 0x202020A0u)
    std::fprintf(stderr, "Failed to find code '%s' in index\n", (const char*)&code);
  return 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- callbacks

// translate_string -> sub_140924FB0 -> sub_140F44A10(str, 5382):
// translation-table lookup.  The table is the localisation key space
// (Key -> numeric id); a miss returns the caller supplied default 5382.
// This is why books "name" is 0x1506 for every row, and why uniqueappellation
// stores 2825 for "the Hammer".
static const std::unordered_map<std::string, uint32_t>* g_translation = nullptr;

static uint16_t TranslationCallback16(const char* text) {
  if (text && *text && g_translation) {
    auto it = g_translation->find(text);
    if (it != g_translation->end()) return (uint16_t)it->second;
  }
  return 5382;
}

static Engine* g_engine = nullptr;

// parse_monster_ref: monpet "monster".  Writes { u16 kind, u16 index } where kind
// is 2 for a hireling-class name (index 4240) or 1 for a monster name (3952).
static void ParseMonsterRef(uint8_t, const char* text, char* row, int offset,
                            int, int, int) {
  uint16_t* p = (uint16_t*)(row + offset);
  p[0] = 0;
  p[1] = 0xFFFF;
  if (!text || !*text || !g_engine) return;
  DataIndex* a = g_engine->GetIndex("superuniques_superunique_index");
  uint32_t v = a ? a->str.Lookup(text, false) : 0xFFFFFFFFu;
  if (v != 0xFFFFFFFFu) { p[0] = 2; p[1] = (uint16_t)v; return; }
  DataIndex* b = g_engine->GetIndex("monstats_id_index");
  v = b ? b->str.Lookup(text, false) : 0xFFFFFFFFu;
  if (v != 0xFFFFFFFFu) { p[0] = 1; p[1] = (uint16_t)v; }
}

// parse_place_ref -> sub_140A9EE20: monpreset "Place".  Writes a kind byte at
// row+1 and a u16 index at row+2, trying 4240, 3952 then 4048.
static void ParsePlaceRef(uint8_t, const char* text, char* row, int,
                          int, int, int) {
  row[1] = 0;
  *(uint16_t*)(row + 2) = 0;
  if (!text || !*text || !g_engine) return;
  const char* hs[3] = {"superuniques_superunique_index", "monstats_id_index", "monplace_index"};
  for (int k = 0; k < 3; ++k) {
    DataIndex* ix = g_engine->GetIndex(hs[k]);
    uint32_t v = ix ? ix->str.Lookup(text, k == 2) : 0xFFFFFFFFu;
    if (v != 0xFFFFFFFFu) {
      row[1] = (char)(k == 0 ? 2 : (k == 1 ? 1 : 0));
      *(uint16_t*)(row + 2) = (uint16_t)v;
      return;
    }
  }
}

// parse_variations -> sub_140A9AA40: monstats2 variation lists.  The text is a
// comma separated list of <=4 char codes; each is resolved through the code
// index at instance+504 and stored as a byte.
static void ParseVariations(uint8_t, const char* text, char* row, int offset,
                            int, int, int) {
  if (!row) return;
  unsigned idx = (unsigned)offset;
  if (idx >= 0x10) return;
  row[idx + 21] = 0;
  for (int i = 0; i < 12; ++i) row[12 * idx + 38 + i] = (char)0xFF;
  if (!text || !g_engine) return;
  // sub_140A95770: skip one leading quote and terminate the working copy at
  // the next quote before sub_140A957F0 splits the comma-separated tokens.
  std::string unquoted(text);
  if (!unquoted.empty() && unquoted[0] == '"') unquoted.erase(0, 1);
  size_t quote = unquoted.find('"');
  if (quote != std::string::npos) unquoted.resize(quote);
  DataIndex* ix = g_engine->GetIndex("compcode_code_index");
  int n = 0;
  const char* p = unquoted.c_str();
  while (*p && n < 12) {
    char tok[8];
    int k = 0;
    while (*p && *p != ',' && k < 7) tok[k++] = *p++;
    tok[k] = 0;
    if (*p == ',') ++p;
    if (!tok[0]) break;
    if (std::strlen(tok) > 4) break;
    uint32_t v = ix ? ix->code.Lookup(PackCodeU32(tok), true) : 0xFFFFFFFFu;
    row[12 * idx + 38 + n++] = (char)(uint8_t)v;
    if (!*p) break;
  }
  row[idx + 21] = (char)n;
  if (idx == 15) {
    int total = 0;
    for (int j = 0; j < 16; ++j)
      if ((uint8_t)row[j + 21] > 1) total += (uint8_t)row[j + 21] - 1;
    if (total > 254) total = 0xFE;
    row[37] = (char)total;
  }
}

// parse_monstats_sound -> sub_1408A77E0 (D2R.exe 69270; the descriptor points at
// the thunk sub_140017C7E): monstats skill-mode callback.  Stores a code byte at
// row + idx + 436 and a fallback sequence index at row + 2*idx + 444; it skips
// the row entirely when the paired SkillN index at row + 2*idx + 420 is negative.
// NOTE: these three record offsets are HARDCODED in the game and DIFFER between
// builds -- 3.1.1 uses 468 / 476 / 452 because its monstats record places SkillN
// at 452, whereas 69270 places SkillN at 420 (so mode=436, seq=444).  Using the
// 3.1.1 constants here writes past the 476-byte record and corrupts the next
// row's Id index, which is exactly the shipped-bytes failure this fixes.
static void ParseMonStatsSound(uint8_t, const char* text, char* row, int offset,
                               int, int, int) {
  if (!row) return;
  unsigned idx = (unsigned)offset;
  if (idx >= 8) return;
  if (!g_engine) return;
  row[idx + 436] = 0;
  *(uint16_t*)(row + 2 * idx + 444) = 0xFFFF;
  if (*(int16_t*)(row + 2 * idx + 420) < 0) return;
  if (!text) return;
  DataIndex* code = g_engine->GetIndex("monmode_code_index");
  uint8_t v = 0xFF;
  if (std::strlen(text) <= 3) {
    uint32_t c = PackCodeU32(text);
    uint32_t r = code ? code->code.Lookup(c, true) : 0xFFFFFFFFu;
    v = (uint8_t)r;
  }
  row[idx + 436] = (char)v;
  if ((int8_t)v < 0 || v == 14) {
    row[idx + 436] = 14;
    DataIndex* str = g_engine->GetIndex("monseq_sequence_index");
    uint32_t r = str ? str->str.Lookup(text, true) : 0xFFFFFFFFu;
    *(uint16_t*)(row + 2 * idx + 444) = (uint16_t)r;
  }
}


// ---------------------------------------------------------------- horadric cube
//
// sub_140B54070 registers cubemain; the three field callbacks below were
// decompiled from D2R.exe in full:
//   parse_cube_input -> sub_140B56E40 -> sub_140B55C80  (input slots)
//   parse_cube_output -> sub_140B56ED0 -> sub_140B56450  (output slots)
//   parse_cube_param -> sub_140B56F60                   (param)
// The descriptor nFieldOffset of "input N"/"output N" is the slot index; the
// real slot lives at row + 8*idx + 20 (inputs, 8 bytes each) or
// row + 89*idx + 76 (outputs).  Indexes used:
//   itemtypes_code_index itemtypes code index (dataTable[616])
//   item_code_index  shared item code index (byte 544; filled by the weapons/armor/
//           misc type-10 "code" fields)
//   uniqueitems_name_index uniqueitems name index (dataTable[632], rows at +633)
//   setitems_name_index setitems name index (dataTable[628], rows at +629)
//   itemstatcost_stat_index stat-name string index (byte 4720, param callback)

// sub_14009C05A / sub_1401E7414: CRT string compare thunks (0 == equal).
static bool CubeIEq(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    int ca = (unsigned char)*a++, cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return false;
  }
  return *a == *b;
}

// sub_140B55AC0: skip one leading quote and terminate at the next quote.
static char* CubeUnquote(char* s) {
  char* p = s;
  if (*p == '"') p = s + 1;
  for (char* i = p; *i; ++i)
    if (*i == '"') { *i = 0; break; }
  return p;
}

// sub_140B55B40: return the next delimiter-delimited token, advancing *cur
// past it (cur becomes null when the string is exhausted).
static char* CubeNext(char** cur, const char* delims) {
  char* tok = *cur;
  *cur += strcspn(*cur, delims);
  if (**cur) *(*cur)++ = 0;
  else *cur = nullptr;
  return tok;
}

// sub_140B55BE0: same, returning atol(token).
static int CubeNextInt(char** cur, const char* delims) {
  if (!*cur) return 0;
  return (int)strtol(CubeNext(cur, delims), nullptr, 10);
}

// DataTablesGetItemStatsByCode (0x14095E090): resolve a 4-char item code
// through the shared item code index at byte 544; out receives the index
// value on a hit.
static bool CubeItemStatsByCode(uint32_t code, uint16_t* out) {
  if (!g_engine) return false;
  DataIndex* ix = g_engine->GetIndex("item_code_index");
  uint32_t v = ix ? ix->code.Lookup(code, false) : 0xFFFFFFFFu;
  if (v == 0xFFFFFFFFu) return false;
  *out = (uint16_t)v;
  return true;
}

// sub_1400F9917 / sub_1401146F9: row `idx` of a compiled table image.
static const uint8_t* CubeRow(const char* table, uint32_t idx) {
  if (!g_engine) return nullptr;
  int stride = 0;
  for (int i = 0; i < kTableCount; ++i)
    if (std::strcmp(kTables[i].name, table) == 0) { stride = kTables[i].record_size; break; }
  if (stride <= 0) return nullptr;
  const std::vector<uint8_t>* bin = g_engine->GetTableRows(table);
  if (!bin || bin->size() < 4) return nullptr;
  uint32_t rows = rd_u32(bin->data());
  if (idx >= rows) return nullptr;
  if (bin->size() < (size_t)4 + (size_t)rows * stride) return nullptr;
  return bin->data() + 4 + (size_t)idx * stride;
}

// sub_140B56E40 -> sub_140B55C80: horadric cube input slot parser.
static void ParseCubeInput(uint8_t, const char* text, char* row, int offset,
                           int, int r, int c) {
  if (!row || !text || !*text) return;
  char* slot = row + 8 * (int64_t)offset + 20;
  uint16_t* w = (uint16_t*)slot;
  int found = 0;
  uint16_t stat = 0;

  char buf[256];
  std::memset(buf, 0, sizeof(buf));
  std::strncpy(buf, text, sizeof(buf) - 1);
  char* cur = CubeUnquote(buf);
  if (!*cur) return;
  char* tok = CubeNext(&cur, ",");

  if (std::strlen(tok) <= 4 && CubeIEq(tok, "any")) {
    w[0] |= 1u;
    w[1] = 0xFFFFu;
    found = 1;
  }
  if (!found && std::strlen(tok) <= 4) {
    if (g_engine) {
      DataIndex* ix = g_engine->GetIndex("itemtypes_code_index");
      uint32_t v = ix ? ix->code.Lookup(PackCodeU32(tok), false) : 0xFFFFFFFFu;
      if (v != 0xFFFFFFFFu) {
        w[0] |= 2u;
        w[1] = (uint16_t)v;
        found = 1;
      }
    }
  }
  if (!found && std::strlen(tok) <= 4) {
    if (CubeItemStatsByCode(PackCodeU32(tok), &stat)) {
      w[0] |= 1u;
      w[1] = stat;
      found = 1;
    }
  }
  if (!found && g_engine) {
    DataIndex* ix = g_engine->GetIndex("uniqueitems_name_index");
    uint32_t v = ix ? ix->str.Lookup(tok, false) : 0xFFFFFFFFu;
    if (v != 0xFFFFFFFFu) {
      const uint8_t* e = CubeRow("uniqueitems", v);
      uint16_t st = 0;
      if (e) CubeItemStatsByCode(rd_u32(e + 40), &st);
      w[0] |= 0x41u;
      slot[6] = 7;
      *(uint16_t*)(slot + 4) = (uint16_t)(v + 1);
      w[1] = st;
      found = 1;
    }
  }
  if (!found && g_engine) {
    DataIndex* ix = g_engine->GetIndex("setitems_name_index");
    uint32_t v = ix ? ix->str.Lookup(tok, false) : 0xFFFFFFFFu;
    if (v != 0xFFFFFFFFu) {
      const uint8_t* e = CubeRow("setitems", v);
      uint16_t st = 0;
      if (e) CubeItemStatsByCode(rd_u32(e + 40), &st);
      w[0] |= 0x41u;
      slot[6] = 5;
      *(uint16_t*)(slot + 4) = (uint16_t)(v + 1);
      w[1] = st;
      found = 1;
    }
  }
  if (!found) {
    std::fprintf(stderr,
                 "Couldn't parse horardric cube input!  (Line:%d  Item:%d)\n",
                 r, offset);
    return;
  }

  // modifier chain: qty + quality flags (nested else-if in the original).
  while (cur) {
    char* m = CubeNext(&cur, "=,");
    if (!m) {
      std::fprintf(stderr,
          "Couldn't parse horardric cube input modifier!  (Line:%d  Item:%d)\n",
          r, offset);
      return;
    }
    if (CubeIEq(m, "qty")) {
      slot[7] = (char)CubeNextInt(&cur, ",");
    } else if (CubeIEq(m, "low")) slot[6] = 1;
    else if (CubeIEq(m, "nor")) slot[6] = 2;
    else if (CubeIEq(m, "hiq")) slot[6] = 3;
    else if (CubeIEq(m, "mag")) slot[6] = 4;
    else if (CubeIEq(m, "set")) slot[6] = 5;
    else if (CubeIEq(m, "rar")) slot[6] = 6;
    else if (CubeIEq(m, "uni")) slot[6] = 7;
    else if (CubeIEq(m, "crf")) slot[6] = 8;
    else if (CubeIEq(m, "tmp")) slot[6] = 9;
    else if (CubeIEq(m, "nos")) w[0] |= 4u;
    else if (CubeIEq(m, "sock")) w[0] |= 8u;
    else if (CubeIEq(m, "noe")) w[0] |= 0x20u;
    else if (CubeIEq(m, "eth")) w[0] |= 0x10u;
    else if (CubeIEq(m, "upg")) w[0] |= 0x80u;
    else if (CubeIEq(m, "bas")) w[0] |= 0x100u;
    else if (CubeIEq(m, "exc")) w[0] |= 0x200u;
    else if (CubeIEq(m, "eli")) w[0] |= 0x400u;
    else if (CubeIEq(m, "nru")) w[0] |= 0x800u;
    else {
      std::fprintf(stderr,
          "Couldn't parse horardric cube input modifier!  (Line:%d  Item:%d)\n",
          r, offset);
      return;
    }
  }
}

// sub_140947860 -> sub_140946EB0: horadric cube output slot parser (D2R.exe
// 69270).  The output slot is 84 bytes at row + 84*idx + 76 and is COMPACTED
// relative to 3.1.1 (89 bytes at +89*idx+76 with the flag word at +4): 69270
// puts the flag word at +0, so every member shifts down by 4, and the
// uniqueitems/setitems "extra" byte is read at +52/+48 instead of +56/+52.
// The game stores no line number in the slot (3.1.1 used the 4 bytes the flag
// word now occupies).
//   +0  u16 flags      +2  u16 stat      +4  u16 (unique/set index + 1)
//   +6  u8  quality    +7  u8  qty/lvl   +8  u8  kind
//   +11 u8  extra      +12 u16 pre[..]   +18 u16 suf[..]
static void ParseCubeOutput(uint8_t, const char* text, char* row, int offset,
                            int, int r, int c) {
  if (!row || !text || !*text) return;
  char* slot = row + 84 * (int64_t)offset + 76;
  uint16_t* w4 = (uint16_t*)slot;   // flag word at +0
  int found = 0;

  char buf[256];
  std::memset(buf, 0, sizeof(buf));
  std::strncpy(buf, text, sizeof(buf) - 1);
  char* cur = CubeUnquote(buf);
  char* tok = CubeNext(&cur, ",");

  if (CubeIEq(tok, "Cow Portal")) { slot[8] = 1; found = 1; }
  else if (CubeIEq(tok, "Pandemonium Portal")) { slot[8] = 2; found = 1; }
  else if (CubeIEq(tok, "Pandemonium Finale Portal")) { slot[8] = 3; found = 1; }
  else if (CubeIEq(tok, "Red Portal")) { slot[8] = 4; found = 1; }
  else if (CubeIEq(tok, "usetype")) { slot[8] = (char)-1; found = 1; }
  else if (CubeIEq(tok, "useitem")) { slot[8] = (char)-2; found = 1; }

  uint16_t stat = 0;
  if (!found && std::strlen(tok) <= 4) {
    if (CubeItemStatsByCode(PackCodeU32(tok), &stat)) {
      slot[8] = (char)-4;
      *(uint16_t*)(slot + 2) = stat;
      found = 1;
    }
  }
  if (!found && std::strlen(tok) <= 4 && g_engine) {
    DataIndex* ix = g_engine->GetIndex("itemtypes_code_index");
    uint32_t v = ix ? ix->code.Lookup(PackCodeU32(tok), false) : 0xFFFFFFFFu;
    if (v != 0xFFFFFFFFu) {
      slot[8] = (char)-3;
      *(uint16_t*)(slot + 2) = (uint16_t)v;
      found = 1;
    }
  }
  if (!found && g_engine) {
    DataIndex* ix = g_engine->GetIndex("uniqueitems_name_index");
    uint32_t v = ix ? ix->str.Lookup(tok, false) : 0xFFFFFFFFu;
    if (v != 0xFFFFFFFFu) {
      const uint8_t* e = CubeRow("uniqueitems", v);
      uint16_t st = 0;
      if (e) CubeItemStatsByCode(rd_u32(e + 40), &st);
      slot[8] = (char)-4;
      *w4 |= 8u;
      slot[6] = 7;
      *(uint16_t*)(slot + 4) = (uint16_t)(v + 1);
      *(uint16_t*)(slot + 2) = st;
      slot[11] = e ? (char)e[52] : 0;
      found = 1;
    }
  }
  if (!found && g_engine) {
    // NOTE: the original does not set its found flag here (no shipped recipe
    // exercises a set-name output); the modifier loop below is skipped.
    DataIndex* ix = g_engine->GetIndex("setitems_name_index");
    uint32_t v = ix ? ix->str.Lookup(tok, false) : 0xFFFFFFFFu;
    if (v != 0xFFFFFFFFu) {
      const uint8_t* e = CubeRow("setitems", v);
      uint16_t st = 0;
      if (e) CubeItemStatsByCode(rd_u32(e + 40), &st);
      slot[8] = (char)-4;
      *w4 |= 8u;
      slot[6] = 5;
      *(uint16_t*)(slot + 4) = (uint16_t)(v + 1);
      *(uint16_t*)(slot + 2) = st;
      slot[11] = e ? (char)e[48] : 0;
    }
  }
  if (!found) {
    std::fprintf(stderr,
                 "Couldn't parse horardric cube output!  (Line:%d  Item:%d)\n",
                 r, offset);
    return;
  }

  int pre = 0, suf = 0;
  while (cur) {
    char* m = CubeNext(&cur, "=,");
    if (!m) {
      std::fprintf(stderr,
          "Couldn't parse horardric cube output modifier!  (Line:%d  Item:%d)\n",
          r, offset);
      return;
    }
    if (CubeIEq(m, "qty")) {
      slot[7] = (char)CubeNextInt(&cur, ",");
    } else if (CubeIEq(m, "pre")) {
      *(uint16_t*)(slot + 12 + 2 * pre) = (uint16_t)CubeNextInt(&cur, ",");
      ++pre;
    } else if (CubeIEq(m, "suf")) {
      *(uint16_t*)(slot + 18 + 2 * suf) = (uint16_t)CubeNextInt(&cur, ",");
      ++suf;
    } else if (CubeIEq(m, "low")) slot[6] = 1;
    else if (CubeIEq(m, "nor")) slot[6] = 2;
    else if (CubeIEq(m, "hiq")) slot[6] = 3;
    else if (CubeIEq(m, "mag")) slot[6] = 4;
    else if (CubeIEq(m, "set")) slot[6] = 5;
    else if (CubeIEq(m, "rar")) slot[6] = 6;
    else if (CubeIEq(m, "uni")) slot[6] = 7;
    else if (CubeIEq(m, "crf")) slot[6] = 8;
    else if (CubeIEq(m, "tmp")) slot[6] = 9;
    else if (CubeIEq(m, "eth")) *w4 |= 4u;
    else if (CubeIEq(m, "sock")) {
      *w4 |= 2u;
      slot[7] = (char)CubeNextInt(&cur, ",");
    } else if (CubeIEq(m, "mod")) *w4 |= 1u;
    else if (CubeIEq(m, "uns")) *w4 |= 0x10u;
    else if (CubeIEq(m, "rem")) *w4 |= 0x20u;
    else if (CubeIEq(m, "reg")) { slot[8] = (char)-1; *w4 |= 0x40u; }
    else if (CubeIEq(m, "exc")) *w4 |= 0x80u;
    else if (CubeIEq(m, "eli")) *w4 |= 0x100u;
    else if (CubeIEq(m, "rep")) *w4 |= 0x200u;
    else if (CubeIEq(m, "rch")) *w4 |= 0x400u;
    else if (CubeIEq(m, "lvl")) {
      slot[7] = (char)CubeNextInt(&cur, ",");
    } else {
      std::fprintf(stderr,
          "Couldn't parse horardric cube output modifier '%s'!  (Line:%d  Item:%d)\n",
          m, r, offset);
      return;
    }
  }
}

// parse_cube_param -> sub_140B56F60: numeric-or-stat-name parameter.
static void ParseCubeParam(uint8_t, const char* text, char* row, int offset,
                           int, int r, int) {
  if (!row || !text || !*text) return;
  int32_t* p = (int32_t*)(row + offset);
  if (text[0] == '-' || (text[0] >= '0' && text[0] <= '9')) {
    *p = (int32_t)strtol(text, nullptr, 10);
    return;
  }
  DataIndex* ix = g_engine ? g_engine->GetIndex("itemstatcost_stat_index") : nullptr;
  uint32_t v = ix ? ix->str.Lookup(text, false) : 0xFFFFFFFFu;
  if (v != 0xFFFFFFFFu) {
    *p = (int32_t)v;
  } else {
    *p = 0;
    std::fprintf(stderr, "Failed to parse '%s' line %d\n", text, r);
  }
}

// Dispatch a type 26/27/28 callback.  `text` may be null, which is how the
// original zero-initialises a descriptor that has no column in the file.
static void RunParseCallback(const ExcelFieldDesc& d, const char* text,
                             char* row, int r, int c) {
  if (!d.ptr) return;
  const char* h = (const char*)d.ptr;
  const char* fn = (std::strncmp(h, "cb:", 3) == 0) ? h + 3 : h;
  char* dst = row + d.offset;
  const char* page = nullptr;
  CalcFamilyKind kind = CALC_FAM_SKILLS;
  // sub_140AE7C60 / 7CF0 / 7D80 / 7E10 / 7EA0 / 7F30: the page each loader
  // appends to (instance + offset) and the resolver flavour it uses.  Page
  // names are the ground-truth index names registered via sub_1401142A8 and
  // serialised as "<name>.bin" (tccode = dataTable+0xD90, moncode = +0x12C0).
  if (std::strcmp(fn, "calc_skills") == 0) { page = "skillscode"; kind = CALC_FAM_SKILLS; }
  else if (std::strcmp(fn, "calc_skilldesc") == 0) { page = "skilldesccode"; kind = CALC_FAM_SKILLDESC; }
  else if (std::strcmp(fn, "calc_missile") == 0) { page = "misscode"; kind = CALC_FAM_MISSILES; }
  else if (std::strcmp(fn, "calc_item") == 0) { page = "itemscode"; kind = CALC_FAM_ITEMS; }
  else if (std::strcmp(fn, "calc_monpet") == 0) { page = "moncode"; kind = CALC_FAM_MONPET; }
  else if (std::strcmp(fn, "calc_condition") == 0) { page = "tccode"; kind = CALC_FAM_ITEMS2; }
  if (page) {
    // sub_140AE9840 -> sub_140148481: append the compiled bytes to the page and
    // store the page size from before the append.
    std::vector<uint8_t>& pg = g_engine->Page(page);
    uint8_t buf[1024];
    int n = 0;
    if (text && *text)
      n = CompileCalcExpression(*g_engine, 3, kind, text, buf, (int)sizeof(buf));
    if (n > 0) {
      uint32_t off = (uint32_t)pg.size();
      pg.insert(pg.end(), buf, buf + n);
      wr_u32(dst, off);
    } else {
      wr_u32(dst, 0xFFFFFFFFu);
    }
    return;
  }
  ParseCall f = LookupCallback(fn);
  if (f) f(0, text, row, (int)d.offset, d.len, r, c);
}

// calc_property -> sub_14096EAA0: property-mode index callback.  Writes
// { int16 index, int16 flag }: -1/-1 for an empty text, otherwise the property
// name is looked up in the two property indexes (instance+624 and +632), the
// flag recording which one matched.
static void ParsePropertyIndex(uint8_t, const char* text, char* row, int offset,
                               int, int, int) {
  int16_t* p = (int16_t*)(row + offset);
  p[0] = -1;
  p[1] = -1;
  if (!text || !*text || !g_engine) return;
  DataIndex* a = g_engine->GetIndex("properties_code_index");
  uint32_t v = a ? a->str.Lookup(text, false) : 0xFFFFFFFFu;
  if (v != 0xFFFFFFFFu) { p[0] = (int16_t)v; p[1] = 0; return; }
  DataIndex* b = g_engine->GetIndex("propertygroups_code_index");
  v = b ? b->str.Lookup(text, false) : 0xFFFFFFFFu;
  if (v != 0xFFFFFFFFu) { p[0] = (int16_t)v; p[1] = 1; }
}

// calc_mod_param -> sub_14096E8C0: numeric-or-name parameter callback.
static void ParseModParam(uint8_t, const char* text, char* row, int offset,
                          int, int, int) {
  int32_t* p = (int32_t*)(row + offset);
  if (text && *text) {
    if (*text == '-' || (*text >= '0' && *text <= '9')) {
      *p = (int32_t)strtol(text, nullptr, 10);
    } else {
      int32_t val = 0;
      const char* hs[3] = {"skills_skill_index", "montype_type_index", "states_state_index"};
      for (int i = 0; i < 3 && g_engine; ++i) {
        DataIndex* ix = g_engine->GetIndex(hs[i]);
        uint32_t v = ix ? ix->str.Lookup(text, false) : 0xFFFFFFFFu;
        if (v != 0xFFFFFFFFu) { val = (int32_t)v; break; }
      }
      *p = val;
    }
  } else {
    *p = 0;
  }
}

static void ParseNoop(uint8_t, const char*, char*, int, int, int, int) {}

// ---------------------------------------------------------------- BuildCtx

bool Engine::BuildCtx(std::vector<char>&& file, ExcelCtx& ctx) {  // sub_1419D8500
  if (file.empty()) return false;

  // Normalise line endings: every cell ends with NUL and every line ends with
  // "\0\n", exactly the shape the original parser walks.  The header line is
  // consumed to count columns; only the body is kept.
  std::vector<char> out;
  out.reserve(file.size() + 16);
  size_t i = 0;
  const size_t n = file.size();
  int n_cols = 0, n_rows = 0;
  size_t body_offset = 0;
  bool first_line = true;
  std::vector<char> line;

  while (i < n) {
    line.clear();
    int cells = 0;
    while (true) {
      while (i < n && file[i] != '\t' && file[i] != '\r' && file[i] != '\n')
        line.push_back(file[i++]);
      line.push_back('\0');  // terminate the cell
      ++cells;
      if (i < n && file[i] == '\t') { ++i; continue; }  // next cell
      break;
    }
    if (i < n && file[i] == '\r') ++i;
    if (i < n && file[i] == '\n') ++i;
    line.push_back('\n');

    if (first_line) {
      first_line = false;
      n_cols = cells;
      if (i >= n) return false;  // header only -> no data
      // keep the header in the buffer; the column mapper reads it
      out.insert(out.end(), line.begin(), line.end());
      body_offset = out.size();
      continue;
    }
    // rows whose first cell is exactly "Expansion" are dropped
    if (cells >= 1 && std::strcmp(line.data(), "Expansion") == 0) continue;
    out.insert(out.end(), line.begin(), line.end());
    ++n_rows;
  }

  if (n_rows == 0) return false;
  ctx.buf = std::move(out);
  ctx.header = ctx.buf.data();
  ctx.data = ctx.buf.data() + body_offset;
  ctx.n_rows = n_rows;
  ctx.n_cols = n_cols;
  return true;
}

// ---------------------------------------------------------------- MapColumns

int Engine::MapColumns(uint16_t* out_map, const ExcelCtx& ctx,
                       const ExcelFieldDesc* descs) const {  // sub_1419DB410
  int n_desc = 0;
  while (descs[n_desc].name) ++n_desc;

  // Skip BYTE_OR groups: a type-5 entry claims nFieldLength following entries.
  for (int i = 0; i < n_desc;) {
    if (descs[i].type == EXCEL_TYPE_BYTE_OR) {
      int claim = descs[i].len;
      for (int j = 1; j < claim; ++j) {
        ++i;
        if (i >= n_desc || descs[i].type != EXCEL_TYPE_BYTE_OR) break;
      }
    }
    ++i;
  }

  std::vector<int> used(n_desc, 0);
  std::unordered_map<std::string, int> by_name;
  for (int i = 0; i < n_desc; ++i) {
    std::string k = descs[i].name;
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    by_name[k] = i;  // last insertion wins, as in the original bucket list
  }

  int n = 0;
  const char* col = ctx.header;
  const bool trace = std::getenv("D2DT_TRACE") != nullptr;
  for (int c = 0; c < ctx.n_cols; ++c) {
    std::string k = col;
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    auto it = by_name.find(k);
    if (trace) std::fprintf(stderr, "  col[%d]='%s' -> %s\n", c, col,
                            it != by_name.end() ? "match" : "MISS");
    if (it != by_name.end()) {
      used[it->second] = 1;
      out_map[n++] = (uint16_t)it->second;
    } else {
      out_map[n++] = 0x7FFF;
    }
    col += std::strlen(col) + 1;
  }
  for (int i = 0; i < n_desc; ++i) {
    if (!used[i]) {
      if (descs[i].type != EXCEL_TYPE_TEXT7 && descs[i].type != EXCEL_TYPE_INT8B)
        std::fprintf(stderr, "Excel attribute '%s' not found in file\n", descs[i].name);
      out_map[n++] = (uint16_t)i;
    }
  }
  return n;
}

// ---------------------------------------------------------------- ParseRows

void Engine::ParseRows(const ExcelCtx& ctx, const ExcelFieldDesc* descs, char* out,
                       int stride, int32_t n_rows, const uint16_t* map,
                       int map_count) {  // sub_1419D6EA0
  const int32_t n_cols = ctx.n_cols;
  const char* p = ctx.data;

  // Decide whether the cross-reference pre-pass is needed.
  bool need_prepass = false;
  for (int c = 0; c < n_cols; ++c) {
    if (map[c] == 0x7FFF) continue;
    int t = descs[map[c]].type;
    if (t == EXCEL_TYPE_CODE_IDX32 || t == EXCEL_TYPE_CODE_IDX8 ||
        t == EXCEL_TYPE_CODE_IDX16 || t == EXCEL_TYPE_TEXT16 ||
        t == EXCEL_TYPE_TXTIDX16 || t == EXCEL_TYPE_TXTIDX32 ||
        t == EXCEL_TYPE_TXTIDX16T || t == EXCEL_TYPE_TXTIDX32T)
      need_prepass = true;
  }

  char rowbuf[512];
  char codestr[8];

  if (need_prepass) {
    const char* q = ctx.data;
    for (int r = 0; r < n_rows; ++r) {
      for (int c = 0; c < n_cols; ++c) {
        if (map[c] == 0x7FFF) { q += std::strlen(q) + 1; continue; }
        const ExcelFieldDesc& d = descs[map[c]];
        char* dst = out + (int64_t)r * stride + d.offset;
        switch (d.type) {
          case EXCEL_TYPE_CODE_IDX32: case EXCEL_TYPE_CODE_IDX8:
          case EXCEL_TYPE_CODE_IDX16: {
            std::memset(codestr, 0, 4);
            int k = 0;
            for (; q[k] && k < 4; ++k) codestr[k] = q[k];
            q += std::strlen(q) + 1;
            char packed[4];
            PackCode(codestr, packed);
            DataIndex* ix = (DataIndex*)d.ptr;
            uint32_t code = code_of(packed);
            uint32_t idx = ix ? ix->code.Add(code) : 0;
            if (d.type == EXCEL_TYPE_CODE_IDX32) wr_u32(dst, code);
            else if (d.type == EXCEL_TYPE_CODE_IDX8) { if (idx > 255) {} wr_u8(dst, (uint8_t)code); }
            else { if (idx > 0xFFFF) {} wr_u16(dst, (uint16_t)code); }
            break;
          }
          case EXCEL_TYPE_TEXT16: {
            char* b = dst;
            char* end = dst + d.len - 1;
            while (*q && b < end) *b++ = *q++;
            *b = 0;
            q += std::strlen(q) + 1;
            DataIndex* ix = (DataIndex*)d.ptr;
            if (ix) ix->str.Intern(dst, false);
            break;
          }
          case EXCEL_TYPE_TXTIDX16: case EXCEL_TYPE_TXTIDX32:
          case EXCEL_TYPE_TXTIDX16T: case EXCEL_TYPE_TXTIDX32T: {
            std::memset(rowbuf, 0, sizeof(rowbuf));
            char* b = rowbuf;
            char* end = rowbuf + 0x30;
            while (*q && b < end) *b++ = *q++;
            *b = 0;
            q += std::strlen(q) + 1;
            DataIndex* ix = (DataIndex*)d.ptr;
            uint32_t idx = ix ? ix->str.Intern(rowbuf, true) : 0;
            if (d.type == EXCEL_TYPE_TXTIDX16 || d.type == EXCEL_TYPE_TXTIDX16T)
              wr_u16(dst, (uint16_t)idx);
            else wr_u32(dst, idx);
            break;
          }
          default:
            q += std::strlen(q) + 1;
            break;
        }
      }
      // row terminator
      if (*q == '\n') ++q; else if (*q == '\0') ++q;
    }
  }

  // Main pass.
  p = ctx.data;
  for (int r = 0; r < n_rows; ++r) {
    char* row = out + (int64_t)r * stride;
    int c = 0;
    for (; c < n_cols; ++c) {
      if (map[c] == 0x7FFF) { p += std::strlen(p) + 1; continue; }
      const ExcelFieldDesc& d = descs[map[c]];
      const char* tok = p;
      char* dst = row + d.offset;
      char* bound = dst + d.len;
      if (d.type == EXCEL_TYPE_TXTIDX16T) { dst += 2; bound += 2; }
      else if (d.type == EXCEL_TYPE_TXTIDX32T) { dst += 4; bound += 4; }

      switch (d.type) {
        case EXCEL_TYPE_TEXT: case EXCEL_TYPE_TEXT7:
        case EXCEL_TYPE_TXTIDX16T: case EXCEL_TYPE_TXTIDX32T: {
          while (*p && dst < bound) *dst++ = *p++;
          p += std::strlen(p) + 1;
          *dst = 0;
          break;
        }
        case EXCEL_TYPE_INT32: case EXCEL_TYPE_INT16: case EXCEL_TYPE_INT8:
        case EXCEL_TYPE_BYTE_OR: case EXCEL_TYPE_INT8B: case EXCEL_TYPE_INT32B:
        case EXCEL_TYPE_BOOL: case EXCEL_TYPE_INVBOOL: {
          int neg = (*tok == '-');
          int v = 0;
          if (neg) ++p;
          while (*p) v = 10 * v + (*p++ - '0');
          if (neg) v = -v;
          ++p;
          if (d.type == EXCEL_TYPE_INT32 || d.type == EXCEL_TYPE_INT32B) wr_u32(dst, (uint32_t)v);
          else if (d.type == EXCEL_TYPE_INT16) wr_u16(dst, (uint16_t)v);
          else if (d.type == EXCEL_TYPE_INT8 || d.type == EXCEL_TYPE_INT8B) wr_u8(dst, (uint8_t)v);
          else if (d.type == EXCEL_TYPE_BYTE_OR) { if (tok + 1 != p) wr_u8(dst, (uint8_t)v); }
          else if (d.type == EXCEL_TYPE_BOOL) {  // value != 0 -> set bit len
            if (v != 0) dst[(d.len) / 8] |= (uint8_t)(1u << ((d.len) & 7));
            else dst[(d.len) / 8] &= (uint8_t)~(1u << ((d.len) & 7));
          } else {  // EXCEL_TYPE_INVBOOL: value == 0 -> set bit
            if (v != 0) dst[(d.len) / 8] &= (uint8_t)~(1u << ((d.len) & 7));
            else dst[(d.len) / 8] |= (uint8_t)(1u << ((d.len) & 7));
          }
          break;
        }
        case EXCEL_TYPE_CODE: {
          std::memset(codestr, 0, 4);
          int k = 0;
          for (; p[k] && k < 4; ++k) codestr[k] = p[k];
          p += std::strlen(p) + 1;
          PackCode(codestr, dst);
          break;
        }
        case EXCEL_TYPE_CODE_IDX32: case EXCEL_TYPE_CODE_IDX8:
        case EXCEL_TYPE_CODE_IDX16: case EXCEL_TYPE_TEXT16:
        case EXCEL_TYPE_TXTIDX16: case EXCEL_TYPE_TXTIDX32:
          p += std::strlen(p) + 1;
          break;
        case EXCEL_TYPE_CODE_REF32: case EXCEL_TYPE_CODE_REF8:
        case EXCEL_TYPE_CODE_REF16: {
          std::memset(codestr, 0, 4);
          int k = 0;
          for (; p[k] && k < 4; ++k) codestr[k] = p[k];
          p += std::strlen(p) + 1;
          char packed[4];
          PackCode(codestr, packed);
          DataIndex* ix = (DataIndex*)d.ptr;
          uint32_t idx = ix ? ix->code.Lookup(code_of(packed), true) : 0xFFFFFFFFu;
          if (d.type == EXCEL_TYPE_CODE_REF32) wr_u32(dst, idx);
          else if (d.type == EXCEL_TYPE_CODE_REF8) { if (idx >= 255) {} wr_u8(dst, (uint8_t)idx); }
          else { if (idx >= 0xFFFF) {} wr_u16(dst, (uint16_t)idx); }
          break;
        }
        case EXCEL_TYPE_STRREF32: case EXCEL_TYPE_STRREF16:
        case EXCEL_TYPE_STRREF8: {
          char b[48];
          char* q2 = b;
          char* e2 = b + 47;
          while (*p && q2 < e2) *q2++ = *p++;
          *q2 = 0;
          p += std::strlen(p) + 1;
          DataIndex* ix = (DataIndex*)d.ptr;
          uint32_t idx = ix ? ix->str.Lookup(b, true) : 0xFFFFFFFFu;
          if (d.type == EXCEL_TYPE_STRREF32) wr_u32(dst, idx);
          else if (d.type == EXCEL_TYPE_STRREF16) { if (idx > 65534) {} wr_u16(dst, (uint16_t)idx); }
          else { if (idx > 254) {} wr_u8(dst, (uint8_t)idx); }
          break;
        }
        case EXCEL_TYPE_STRCALL16: case EXCEL_TYPE_STRCALL32: {
          char b[256];
          char* q2 = b;
          while (*p && q2 < b + 255) *q2++ = *p++;
          p += std::strlen(p) + 1;
          *q2 = 0;
          if (d.ptr) {
            if (d.type == EXCEL_TYPE_STRCALL16) wr_u16(dst, TranslationCallback16(b));
            else wr_u32(dst, TranslationCallback16(b));
          }
          break;
        }
        case EXCEL_TYPE_PARSE26: case EXCEL_TYPE_PARSE27: case EXCEL_TYPE_PARSE28: {
          char b[256];
          char* q2 = b;
          while (*p && q2 < b + 255) *q2++ = *p++;
          p += std::strlen(p) + 1;
          *q2 = 0;
          RunParseCallback(d, b, row, r, c);
          break;
        }
        default:
          std::fprintf(stderr, "Invalid case: ptData->nType=%d\n", d.type);
          p += std::strlen(p) + 1;
          break;
      }
    }
    // Fields beyond the txt columns (appended unused descs) are zero-initialised.
    for (; c < map_count; ++c) {
      if (map[c] == 0x7FFF) continue;
      const ExcelFieldDesc& d = descs[map[c]];
      char* dst = row + d.offset;
      switch (d.type) {
        case EXCEL_TYPE_TEXT: case EXCEL_TYPE_INT8: case EXCEL_TYPE_BYTE_OR:
        case EXCEL_TYPE_INT8B: case EXCEL_TYPE_TEXT7: case EXCEL_TYPE_CODE_IDX8:
        case EXCEL_TYPE_TEXT16: wr_u8(dst, 0); break;
        case EXCEL_TYPE_INT32: case EXCEL_TYPE_INT32B: case EXCEL_TYPE_CODE:
        case EXCEL_TYPE_CODE_IDX32: case EXCEL_TYPE_TXTIDX32:
        case EXCEL_TYPE_STRCALL32: wr_u32(dst, 0); break;
        case EXCEL_TYPE_INT16: case EXCEL_TYPE_CODE_IDX16: case EXCEL_TYPE_TXTIDX16:
        case EXCEL_TYPE_STRCALL16: wr_u16(dst, 0); break;
        case EXCEL_TYPE_CODE_REF32: case EXCEL_TYPE_STRREF32: wr_u32(dst, 0xFFFFFFFFu); break;
        case EXCEL_TYPE_CODE_REF8: case EXCEL_TYPE_STRREF8: wr_u8(dst, 0xFF); break;
        case EXCEL_TYPE_CODE_REF16: case EXCEL_TYPE_STRREF16: wr_u16(dst, 0xFFFF); break;
        case EXCEL_TYPE_TXTIDX16T: wr_u16(dst, 0); dst[2] = 0; break;
        case EXCEL_TYPE_TXTIDX32T: wr_u32(dst, 0); dst[4] = 0; break;
        case EXCEL_TYPE_PARSE26: case EXCEL_TYPE_PARSE27: case EXCEL_TYPE_PARSE28:
          RunParseCallback(d, nullptr, row, r, c); break;
        case EXCEL_TYPE_BOOL: case EXCEL_TYPE_INVBOOL: break;
        default: break;
      }
    }
    if (*p == '\n') ++p; else if (*p == '\0') ++p;
  }
}

// ---------------------------------------------------------------- Engine

DataIndex* Engine::GetIndex(const std::string& handle) {
  auto it = indexes_.find(handle);
  if (it != indexes_.end()) return it->second;
  // Index objects are created empty and filled while the tables load; only the
  // localisation translation map is pre-seeded, and that is used by the type
  // 24/25 callbacks rather than by the index tables themselves.
  DataIndex* ix = new DataIndex();
  // An empty key returns *(hIndex+60); the value is per index and was
  // recovered from the shipped data (tools/gen_idxdefaults.py).
  ix->str.set_empty_index(0xFFFFFFFFu);
  for (int i = 0; i < kIndexDefaultCount; ++i)
    if (handle == kIndexDefaults[i].handle) { ix->str.set_empty_index(kIndexDefaults[i].value); break; }
  indexes_[handle] = ix;
  return ix;
}

const std::vector<uint8_t>* Engine::GetPage(const std::string& name) const {
  auto it = pages_.find(name);
  return it == pages_.end() ? nullptr : &it->second;
}

static ParseCall LookupCallback(const char* name) {
  if (std::strcmp(name, "calc_property") == 0) return &ParsePropertyIndex;
  if (std::strcmp(name, "calc_mod_param") == 0) return &ParseModParam;
  if (std::strcmp(name, "parse_monster_ref") == 0) return &ParseMonsterRef;
  if (std::strcmp(name, "parse_place_ref") == 0) return &ParsePlaceRef;
  if (std::strcmp(name, "parse_variations") == 0) return &ParseVariations;
  if (std::strcmp(name, "parse_monstats_sound") == 0) return &ParseMonStatsSound;
  if (std::strcmp(name, "parse_cube_input") == 0) return &ParseCubeInput;
  if (std::strcmp(name, "parse_cube_output") == 0) return &ParseCubeOutput;
  if (std::strcmp(name, "parse_cube_param") == 0) return &ParseCubeParam;
  return nullptr;
}


// ---------------------------------------------------------- treasure class

// Read a tab separated file into a header + row list.
static bool ReadTsv(const std::string& path, std::vector<std::string>& header,
                    std::vector<std::vector<std::string>>& rows) {
  std::vector<char> raw;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  raw.resize((size_t)n);
  if (n) { size_t rd = fread(raw.data(), 1, (size_t)n, f); (void)rd; }
  fclose(f);
  std::string text(raw.begin(), raw.end());
  std::vector<std::string> lines;
  std::string cur;
  for (char c : text) {
    if (c == '\n') { lines.push_back(cur); cur.clear(); }
    else if (c != '\r') cur.push_back(c);
  }
  if (!cur.empty()) lines.push_back(cur);
  if (lines.empty()) return false;
  auto split = [](const std::string& l) {
    std::vector<std::string> out;
    std::string c2;
    for (char ch : l) {
      if (ch == '\t') { out.push_back(c2); c2.clear(); }
      else c2.push_back(ch);
    }
    out.push_back(c2);
    return out;
  };
  header = split(lines[0]);
  for (size_t i = 0; i < header.size(); ++i)
    std::transform(header[i].begin(), header[i].end(), header[i].begin(), ::tolower);
  for (size_t i = 1; i < lines.size(); ++i) {
    if (lines[i].empty()) continue;
    rows.push_back(split(lines[i]));
  }
  return true;
}

static int ColIndex(const std::vector<std::string>& header, const char* name) {
  for (size_t i = 0; i < header.size(); ++i)
    if (header[i] == name) return (int)i;
  return -1;
}

bool Engine::BuildTreasureClassIndex(const std::string& itemtypes_txt,
                                     const std::string& tcex_txt) {
  DataIndex* ix = GetIndex("treasureclass_name_index");
  ix->str.Intern("TC", true);

  std::vector<std::string> hdr;
  std::vector<std::vector<std::string>> rows;
  if (ReadTsv(itemtypes_txt, hdr, rows)) {
    int ci = ColIndex(hdr, "code");
    int ti = ColIndex(hdr, "treasureclass");
    if (ci >= 0 && ti >= 0) {
      for (const auto& r : rows) {
        if ((int)r.size() <= ti || (int)r.size() <= ci) continue;
        const std::string& flag = r[ti];
        if (flag.empty() || flag == "0") continue;
        std::string code = r[ci];
        while (!code.empty() && code.back() == ' ') code.pop_back();
        for (int j = 3; j <= 96; j += 3) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%s%d", code.c_str(), j);
          ix->str.Intern(buf, true);
        }
      }
    }
  }

  std::vector<std::string> h2;
  std::vector<std::vector<std::string>> r2;
  if (ReadTsv(tcex_txt, h2, r2)) {
    for (const auto& r : r2) {
      if (r.empty()) continue;
      std::string nm = r[0];
      while (!nm.empty() && nm.back() == ' ') nm.pop_back();
      if (nm.empty()) continue;
      ix->str.Intern(nm.c_str(), true);
    }
  }
  return true;
}

bool Engine::BuildTreasureClassIndexFromBins(
    const std::vector<uint8_t>& itemtypes_bin,
    const std::vector<uint8_t>& tcex_bin) {
  DataIndex* ix = GetIndex("treasureclass_name_index");
  ix->str.Intern("TC", true);

  auto find_def = [](const char* name) -> const TableDef* {
    for (int i = 0; i < kTableCount; ++i)
      if (std::strcmp(kTables[i].name, name) == 0) return &kTables[i];
    return nullptr;
  };
  auto find_field = [](const TableDef* def, const char* field) -> const ExcelFieldDesc* {
    if (!def || !def->descs) return nullptr;
    for (int i = 0; def->descs[i].name; ++i)
      if (std::strcmp(def->descs[i].name, field) == 0) return &def->descs[i];
    return nullptr;
  };

  const TableDef* it_def = find_def("itemtypes");
  const ExcelFieldDesc* code_d = find_field(it_def, "code");
  const ExcelFieldDesc* flag_d = find_field(it_def, "treasureclass");
  if (it_def && code_d && flag_d && itemtypes_bin.size() >= 4) {
    uint32_t rows = rd_u32(itemtypes_bin.data());
    int rec = it_def->record_size;
    if (itemtypes_bin.size() >= (size_t)4 + (size_t)rows * rec) {
      for (uint32_t r = 0; r < rows; ++r) {
        const uint8_t* row = itemtypes_bin.data() + 4 + (size_t)r * rec;
        uint8_t flag = row[flag_d->offset];
        if (!flag) continue;
        // type 10: the packed 4-char code, space padded (sub_1419DAFD0).
        char code[5];
        for (int k = 0; k < 4; ++k) code[k] = (char)row[code_d->offset + k];
        code[4] = 0;
        char* e = code + 4;
        while (e > code && e[-1] == ' ') *--e = 0;
        for (int j = 3; j <= 96; j += 3) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%s%d", code, j);
          ix->str.Intern(buf, true);
        }
      }
    }
  }

  const TableDef* tc_def = find_def("treasureclassex");
  const ExcelFieldDesc* name_d = find_field(tc_def, "treasure class");
  if (tc_def && name_d && tcex_bin.size() >= 4) {
    uint32_t rows = rd_u32(tcex_bin.data());
    int rec = tc_def->record_size;
    if (tcex_bin.size() >= (size_t)4 + (size_t)rows * rec) {
      for (uint32_t r = 0; r < rows; ++r) {
        const uint8_t* row = tcex_bin.data() + 4 + (size_t)r * rec;
        std::string nm((const char*)row + name_d->offset);
        if (nm.empty()) continue;
        ix->str.Intern(nm.c_str(), true);
      }
    }
  }
  return true;
}

// Read an entire file into a std::string (returns false on any error).
static bool ReadWholeFile(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return false; }
  out.resize((size_t)n);
  if (n && fread(out.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return false; }
  fclose(f);
  return true;
}

// Discover <root>/local/lng/strings relative to the excel input directory.
// `in_dir` is typically .../global/excel or .../global/excel/base; the
// localisation lives at <root>/local/lng/strings.
bool Engine::FindStringsDir(const std::string& in_dir, std::string& out_dir) {
  std::error_code ec;
  std::filesystem::path p = std::filesystem::absolute(in_dir, ec);
  for (int i = 0; i < 8; ++i) {
    std::filesystem::path cand = p / "local" / "lng" / "strings";
    if (std::filesystem::is_directory(cand, ec)) {
      out_dir = cand.string();
      return true;
    }
    if (p == p.parent_path()) break;
    p = p.parent_path();
  }
  return false;
}

bool Engine::LoadStringsFromJsonDir(const std::string& dir) {
  std::error_code ec;
  std::vector<std::string> files;
  for (auto it = std::filesystem::directory_iterator(dir, ec);
       it != std::filesystem::directory_iterator(); ++it) {
    if (it->is_regular_file(ec) && it->path().extension() == ".json")
      files.push_back(it->path().string());
  }
  std::sort(files.begin(), files.end());  // deterministic, name-sorted order (the json loader is order independent,
  // but keep it stable for reproducible diagnostics)

  d2json::Parser parser;
  size_t loaded = 0, skipped = 0;
  for (const auto& f : files) {
    std::string text;
    if (!ReadWholeFile(f, text)) { ++skipped; continue; }
    d2json::Value root;
    std::string err;
    if (!parser.parse(text, root, &err)) {
      std::fprintf(stderr, "json: skip %s: %s\n", f.c_str(), err.c_str());
      ++skipped;
      continue;
    }
    if (root.t != d2json::Value::ARR) { ++skipped; continue; }
    for (const auto& e : root.a) {
      if (e.t != d2json::Value::OBJ) continue;
      const d2json::Value* k = e.find("Key");
      const d2json::Value* id = e.find("id");
      if (!k || k->t != d2json::Value::STR) continue;
      if (!id || !id->isInt) continue;
      const std::string& key = k->s;
      uint32_t idv = (uint32_t)id->i;
      // First Key -> id wins (first occurrence in sorted file order).
      if (json_strings_.find(key) == json_strings_.end()) {
        json_strings_[key] = idv;
        if (idv > json_max_id_) json_max_id_ = idv;
      }
      // Reverse: first id -> Key wins.
      if (reverse_strings_.find(idv) == reverse_strings_.end())
        reverse_strings_[idv] = key;
    }
    ++loaded;
  }
  if (loaded == 0 && skipped)
    std::fprintf(stderr, "json: loaded 0 files from %s (%zu skipped)\n",
                 dir.c_str(), skipped);
  g_translation = &json_strings_;
  return !json_strings_.empty();
}

bool Engine::LoadStrings(const std::string& in_dir) {
  std::string sdir;
  if (FindStringsDir(in_dir, sdir)) return LoadStringsFromJsonDir(sdir);
  return false;
}

std::vector<ExcelFieldDesc> Engine::Resolve(const TableDef& def) {
  std::vector<ExcelFieldDesc> out;
  if (!def.descs) return out;
  for (int i = 0; def.descs[i].name; ++i) {
    ExcelFieldDesc d = def.descs[i];
    if (d.ptr) {
      const char* h = (const char*)d.ptr;
      if (std::strncmp(h, "cb:", 3) == 0) {
        // callbacks are dispatched by name at parse time (calc families need
        // the expression text, not a plain function pointer)
      } else {
        d.ptr = GetIndex(h);
      }
    }
    out.push_back(d);
  }
  // keep the sentinel terminator so the ported scanners can walk the array
  ExcelFieldDesc end{};
  end.name = nullptr;
  out.push_back(end);
  return out;
}

bool Engine::CompileCtx(const TableDef& def, ExcelCtx& ctx,
                        std::vector<uint8_t>& out_bin, std::string* err) {
  std::vector<ExcelFieldDesc> descs = Resolve(def);
  if (descs.empty()) { if (err) *err = "table has no descriptors"; return false; }
  g_engine = this;
  g_engine = this;
  int stride = def.record_size;

  uint16_t map[512];
  int map_count = MapColumns(map, ctx, descs.data());
  if (map_count > 512) { if (err) *err = "too many columns"; return false; }

  std::vector<uint8_t> rows((size_t)stride * ctx.n_rows, 0);
  ParseRows(ctx, descs.data(), (char*)rows.data(), stride, ctx.n_rows, map, map_count);

  out_bin.clear();
  out_bin.resize(4 + rows.size());
  wr_u32(out_bin.data(), (uint32_t)ctx.n_rows);
  std::memcpy(out_bin.data() + 4, rows.data(), rows.size());
  table_bins_[def.name] = out_bin;
  return true;
}

bool Engine::Compile(const TableDef& def, std::vector<char> txt,
                     std::vector<uint8_t>& out_bin, std::string* err) {
  ExcelCtx ctx;
  if (!BuildCtx(std::move(txt), ctx)) { if (err) *err = "empty table"; return false; }
  return CompileCtx(def, ctx, out_bin, err);
}

}  // namespace d2dt
