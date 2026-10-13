// BBE calc-expression compiler - ported from D2R.exe.
// See calc_compiler.h for the address map of the original functions.
#include "calc_compiler.h"
#include "excel_engine.h"
#include "excel_types.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace d2dt {

// ---------------------------------------------------------------- operators

#pragma pack(push, 1)
// 20-byte operator stack entry (sub_141A032F0 / sub_141A021D0).
struct CalcOp {
  uint8_t op;         // +0
  uint8_t pad[3];
  int32_t operand;    // +4   function index / value
  int32_t opEx;       // +8
  int32_t nOperands;  // +12
  uint8_t flag;       // +16  set once emitted
  uint8_t pad2[3];
};
#pragma pack(pop)

// byte_14601AA70 - indexed by the op code on the stack, compared against the
// incoming op code by sub_141A03180.
static const uint8_t kPrec[24] = {
#ifdef D2DT_BUILD_93854
    // byte_142413AA8 (93854).  One more entry than d2r.exe; '%' (0x14) shares
    // the multiplicative rank, so POW/NEG/TERNARY sit one slot higher.
    24, 1, 2, 3, 9, 9, 9, 9, 9, 9,
    15, 15, 15, 15, 15, 15,
    17, 17, 19, 19, 19, 21, 22, 23,
#else
    // byte_14601AA70 (d2r.exe).
    23, 1, 2, 3, 9, 9, 9, 9, 9, 9,
    15, 15, 15, 15, 15, 15,
    17, 17, 19, 19, 20, 21, 22, 0,
#endif
};

// sub_141A03180: pop the stack top when its precedence is >= the new op.
static bool ShouldPop(uint8_t topOp, uint8_t newOp) {
  if (topOp >= BBE_COLON) return false;   // BBE_COLON == BBE_LAST, never pushed
  if (topOp == BBE_FUNC || newOp == BBE_FUNC) return false;
  if (newOp == BBE_PAREN) return false;  // '(' never triggers a pop
  return kPrec[topOp] >= (unsigned)newOp;
}

// sub_141A03230: how many operands this op consumes.
static int OpSize(const CalcOp* e) {
  switch (e->op) {
    case BBE_FUNC: return e->nOperands;
    case BBE_LT: case BBE_GT: case BBE_LE: case BBE_GE:
    case BBE_EQ: case BBE_NE: case BBE_ADD: case BBE_SUB:
    case BBE_MUL: case BBE_DIV: case BBE_POW: return 2;
    case BBE_NEG: return 1;
    case BBE_TERNARY: return 3;
#ifdef D2DT_BUILD_93854
    case BBE_MOD: return 2;   // '%' is a binary operator
#endif
    default: return 0;
  }
}

// sub_141A057E0
static void ClearOp(CalcOp* e) { std::memset(e, 0, sizeof(CalcOp)); }

// sub_141A021D0: topmost FUNC entry that has not been emitted yet.
static CalcOp* TopFunc(CalcOp* ops, int nOps) {
  for (int i = nOps - 1; i >= 0; --i)
    if (ops[i].op == BBE_FUNC && ops[i].flag == 0) return &ops[i];
  return nullptr;
}

// ---------------------------------------------------------------- lexer tokens

// The token kinds the lexer (sub_141A02250) reports through `*out_type`, and
// the values the driver (sub_141A012D0) switches on.  This is a *different*
// numbering from the emitted BBE opcodes above: the driver reads tokens, not
// opcodes.  The punctuation ids follow the order of the lexer's dispatch table;
// the operand kinds (NUM/VAL/FUNC) sit above the punctuation, and '%' is the
// 3.3-only token appended last.
enum TokKind {
  TOK_LPAREN = 0,   // '('
  TOK_RPAREN = 1,   // ')'
  TOK_COMMA = 2,    // ','
  TOK_ADD = 3,      // '+'
  TOK_SUB = 4,      // '-'
  TOK_MUL = 5,      // '*'
  TOK_DIV = 6,      // '/'
  TOK_QUEST = 7,    // '?'
  TOK_COLON = 8,    // ':'
  TOK_POW = 9,      // '^'
  TOK_LE = 10,      // '<='
  TOK_LT = 11,      // '<'
  TOK_GT = 12,      // '>'
  TOK_GE = 13,      // '>='
  TOK_EQ = 14,      // '=='
  TOK_NE = 15,      // '!='
  TOK_NUM = 16,     // numeric literal
  TOK_VAL = 17,     // value/identifier operand
  TOK_FUNC = 18,    // function name
  TOK_MOD = 19,     // '%'  (3.3 / build 93854 only)
};

// ---------------------------------------------------------------- cond states

// enum CondState and the per-state operand value enums (CondTypeKind,
// CondMonsterFlag, CondItemQuality, CondDifficulty) live in calc_compiler.h so
// that decompiler.cpp can share the same names.

// ---------------------------------------------------------------- compiler

namespace {

class Compiler {
 public:
  Compiler(Engine& eng, uint8_t version, CalcFamilyKind fam)
      : eng_(eng), version_(version), fam_(fam) {}

  // Returns the compiled length, or 0 on failure.
  int Run(const char* text, uint8_t* out, int max_len);

 private:
  // Lexer (sub_141A02250).  Returns the next position, or null at end of input.
  const char* Lex(const char* p, uint8_t* out_type, int* out_value);

  // Emit (sub_141A032F0).  Returns the new output cursor or null.
  uint8_t* Emit(uint8_t* cur, uint8_t* base, int max_len, uint8_t newOp,
                int operand);

  // Number / value operand emitters.
  uint8_t* EmitNumber(uint8_t* cur, uint8_t* base, int max_len, int value);
  uint8_t* EmitValue(uint8_t* cur, uint8_t* base, int max_len, int value);

  // sub_140AE8D90: resolve an identifier in the context of the enclosing
  // function call.  Returns the value, or -1.
  int Resolve(const char* text, uint8_t top_op, int32_t top_operand,
              int32_t top_noperands, int cond_slot, int* out_kind);

  Engine& eng_;
  uint8_t version_;
  CalcFamilyKind fam_;
  CalcOp ops_[64];
  int n_ops_ = 0;
  int n_operands_ = 0;
  int cond_state_[64];
};

static inline bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
static inline bool IsIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline bool IsIdentChar(char c) { return IsIdentStart(c) || IsDigit(c); }

uint8_t* Compiler::EmitNumber(uint8_t* cur, uint8_t* base, int max_len, int v) {
  // sub_141A037F0
  if (v < -128 || v > 127) {
    if (v < -32768 || v > 0x7FFF) {
      if (cur - base + 5 >= max_len) return nullptr;
      *cur = BBE_NUM32;
      std::memcpy(cur + 1, &v, 4);
      cur += 5;
    } else {
      if (cur - base + 3 >= max_len) return nullptr;
      *cur = BBE_NUM16;
      int16_t s = (int16_t)v;
      std::memcpy(cur + 1, &s, 2);
      cur += 3;
    }
  } else {
    if (cur - base + 2 >= max_len) return nullptr;
    *cur = BBE_NUM8;
    cur[1] = (uint8_t)(int8_t)v;
    cur += 2;
  }
  ++n_operands_;
  return cur;
}

uint8_t* Compiler::EmitValue(uint8_t* cur, uint8_t* base, int max_len, int v) {
  // sub_141A03A70
  if (v < -128 || v > 127) {
    if (v < -32768 || v > 0x7FFF) {
      if (cur - base + 5 >= max_len) return nullptr;
      *cur = BBE_VAL32;
      std::memcpy(cur + 1, &v, 4);
      cur += 5;
    } else {
      if (cur - base + 3 >= max_len) return nullptr;
      *cur = BBE_VAL16;
      int16_t s = (int16_t)v;
      std::memcpy(cur + 1, &s, 2);
      cur += 3;
    }
  } else {
    if (cur - base + 2 >= max_len) return nullptr;
    *cur = BBE_VAL8;
    cur[1] = (uint8_t)(int8_t)v;
    cur += 2;
  }
  ++n_operands_;
  return cur;
}

uint8_t* Compiler::Emit(uint8_t* cur, uint8_t* base, int max_len, uint8_t newOp,
                        int operand) {
  // sub_141A032F0
  while (n_ops_ > 0) {
    CalcOp* top = &ops_[n_ops_ - 1];
    if (!ShouldPop(top->op, newOp)) break;
    int size = OpSize(top);
    if (n_operands_ < size) return nullptr;
    if (cur - base >= max_len) return nullptr;
    n_operands_ -= size;
    ++n_operands_;
    *cur++ = top->op;
    if (top->op == BBE_FUNC) {
      // 69270 (sub_14166EC80): a FUNC emits ONLY op + operand, with NO size
      // byte.  The 3.1.1/3.3 compiler emits a third byte (the operand count);
      // 69270's interpreter recomputes it from the function index at run time
      // (sub_14166EB90 case 1 calls the arity callback).  Ground truth: this
      // emit and the ')' handler below both write exactly two bytes for
      // BBE_FUNC -- verified in D2R.exe 69270 (IDA 2ada4c852339).  Emitting the
      // size byte here shifts every following byte, which is exactly the
      // skillscode/misscode/skilldesccode page mismatch this fixes.
      *cur++ = (uint8_t)top->operand;
      top->flag = 1;
      break;
    }
    ClearOp(top);
    --n_ops_;
  }
  if (newOp != BBE_COLON && newOp != BBE_SEP && newOp != BBE_END) {
    if (n_ops_ >= 64) return nullptr;
    CalcOp* e = &ops_[n_ops_++];
    ClearOp(e);
    e->op = newOp;
    e->operand = operand;
  }
  if (newOp == BBE_END) {
    if (cur == base) return nullptr;
    if (cur - base >= max_len) return nullptr;
    *cur++ = 0;
  }
  if (newOp == BBE_SEP) {
    CalcOp* f = TopFunc(ops_, n_ops_);
    if (f) ++f->nOperands;
  }
  return cur;
}

const char* Compiler::Lex(const char* p, uint8_t* out_type, int* out_value) {
  // sub_141A02250
  *out_value = 0;
  while (*p && (IsSpace(*p) || *p == '"')) ++p;
  if (IsDigit(*p)) {
    *out_type = TOK_NUM;
    int v = *p - '0';
    for (++p; IsDigit(*p); ++p) v = 10 * v + *p - '0';
    *out_value = v;
    return p;
  }
  if (*p == '\'') {
    // quoted string: handed to the resolver
    char buf[256];
    const char* q = p + 1;
    int n = 0;
    buf[n++] = *q ? *q++ : 0;
    while (*q && *q != '\'') { if (n < 255) buf[n++] = *q; ++q; }
    if (*q) ++q;
    buf[n] = 0;
    CalcOp* f = TopFunc(ops_, n_ops_);
    CalcOp* last = (n_ops_ > 0) ? &ops_[n_ops_ - 1] : nullptr;
    int cond_slot = f ? (int)(f - ops_) : -1;
    int kind = 0;
    int v = Resolve(buf, last ? last->op : 0, last ? last->operand : 0,
                    f ? f->nOperands : 0, cond_slot, &kind);
    if (v < 0) { *out_type = TOK_NUM; *out_value = 0; }
    else { *out_type = kind ? TOK_NUM : TOK_VAL; *out_value = v; }
    return q;
  }
  if (*p == '.') {
    // '.field' : the identifier after the dot is resolved in the context of the
    // enclosing function, whose operand count is bumped first (sub_141A02250).
    char buf[256];
    ++p;
    int n = 0;
    while (IsIdentChar(*p)) { if (n < 255) buf[n++] = *p; ++p; }
    buf[n] = 0;
    CalcOp* f = TopFunc(ops_, n_ops_);
    if (f) ++f->nOperands;
    // sub_141A02250 passes the LAST stack entry's op/operand (the dot path bumps
    // the enclosing FUNC's operand count first) plus that FUNC's operand count;
    // the resolver only proceeds when the op is BBE_FUNC.
    CalcOp* last = (n_ops_ > 0) ? &ops_[n_ops_ - 1] : nullptr;
    int cond_slot = f ? (int)(f - ops_) : -1;
    int kind = 0;
    int v = Resolve(buf, last ? last->op : 0, last ? last->operand : 0,
                    f ? f->nOperands : 0, cond_slot, &kind);
    // sub_141A02250's dot-field path aborts tokenization when its resolver
    // returns a negative value; unlike the bare-identifier path, it does not
    // substitute NUM(0). The driver then flushes the already parsed prefix.
    if (v < 0) return nullptr;
    *out_type = TOK_NUM;
    *out_value = v;
    return p;
  }
  if (IsIdentStart(*p)) {
    char buf[256];
    int n = 0;
    while (IsIdentChar(*p)) { if (n < 255) buf[n++] = *p; ++p; }
    buf[n] = 0;
    while (*p && IsSpace(*p)) ++p;
    if (*p == '(') {
      int idx = CalcFunctionIndex(buf);
      if (idx >= 0) { *out_type = TOK_FUNC; *out_value = idx; return p + 1; }
    }
    CalcOp* f = TopFunc(ops_, n_ops_);
    CalcOp* last = (n_ops_ > 0) ? &ops_[n_ops_ - 1] : nullptr;
    int cond_slot = f ? (int)(f - ops_) : -1;
    int kind = 0;
    int v = Resolve(buf, last ? last->op : 0, last ? last->operand : 0,
                    f ? f->nOperands : 0, cond_slot, &kind);
    if (v < 0) { *out_type = TOK_NUM; *out_value = 0; }
    else { *out_type = kind ? TOK_NUM : TOK_VAL; *out_value = v; }
    return p;
  }
  switch (*p) {
    case 0: return nullptr;
    case '(': *out_type = TOK_LPAREN; return p + 1;
    case ')': *out_type = TOK_RPAREN; return p + 1;
    case '*': *out_type = TOK_MUL; return p + 1;
    case '+': *out_type = TOK_ADD; return p + 1;
    case ',': *out_type = TOK_COMMA; return p + 1;
    case '-': *out_type = TOK_SUB; return p + 1;
    case '/': *out_type = TOK_DIV; return p + 1;
    case ':': *out_type = TOK_COLON; return p + 1;
    case '<': if (p[1] == '=') { *out_type = TOK_LE; return p + 2; } *out_type = TOK_LT; return p + 1;
    case '>': if (p[1] == '=') { *out_type = TOK_GE; return p + 2; } *out_type = TOK_GT; return p + 1;
    case '?': *out_type = TOK_QUEST; return p + 1;
    case '^': *out_type = TOK_POW; return p + 1;
#ifdef D2DT_BUILD_93854
    // 3.3 adds the remainder operator (BBE_MOD).  It has its own lexer token
    // so the driver can emit it at the multiplicative precedence.
    case '%': *out_type = TOK_MOD; return p + 1;
#endif
    case '=': if (p[1] == '=') { *out_type = TOK_EQ; return p + 2; } return nullptr;
    case '!': if (p[1] == '=') { *out_type = TOK_NE; return p + 2; } return nullptr;
    default: return nullptr;
  }
}

// sub_140070B30 -> sub_141A00470: evaluate a compiled expression.  Used to
// constant-fold an expression that contains no value/function operand (the
// driver replaces such an expression with a number literal).
static int EvalConst(const uint8_t* p, int len) {
  int st[64];
  int sp = 0;
  int i = 0;
  auto push = [&](int v) { if (sp < 64) st[sp++] = v; };
  auto pop = [&]() -> int { return sp > 0 ? st[--sp] : 0; };
  while (i < len) {
    uint8_t op = p[i++];
    switch (op) {
      case BBE_FUNC:  // function call - needs the eval table, cannot fold
      case BBE_VAL8: case BBE_VAL16: case BBE_VAL32:  // value operands - need the callback
        return 0;
      case BBE_NUM8: push((int8_t)p[i]); i += 1; break;
      case BBE_NUM16: { int16_t v; std::memcpy(&v, p + i, 2); i += 2; push(v); break; }
      case BBE_NUM32: { int32_t v; std::memcpy(&v, p + i, 4); i += 4; push(v); break; }
      case BBE_LT: { int b = pop(), a = pop(); push(a < b); break; }
      case BBE_GT: { int b = pop(), a = pop(); push(a > b); break; }
      case BBE_LE: { int b = pop(), a = pop(); push(a <= b); break; }
      case BBE_GE: { int b = pop(), a = pop(); push(a >= b); break; }
      case BBE_EQ: { int b = pop(), a = pop(); push(a == b); break; }
      case BBE_NE: { int b = pop(), a = pop(); push(a != b); break; }
      case BBE_ADD: { int a = pop(), b = pop(); push(a + b); break; }
      case BBE_SUB: { int a = pop(), b = pop(); push(b - a); break; }
      case BBE_MUL: { int a = pop(), b = pop(); push(a * b); break; }
      case BBE_DIV: { int a = pop(), b = pop(); push(a != 0 ? b / a : 0); break; }
      // POW/NEG/TERNARY are named rather than numeric: 93854 renumbers them.
      case BBE_POW: { int e = pop(), b = pop(); int r = b;
                   if (e <= 0) r = 1; else for (int k = 1; k < e; ++k) r *= b;
                   push(r); break; }
      case BBE_NEG: { int a = pop(); push(-a); break; }
      case BBE_TERNARY: { int c = pop(), b = pop(), a = pop(); push(a != 0 ? b : c); break; }
#ifdef D2DT_BUILD_93854
      case BBE_MOD: { int a = pop(), b = pop(); push(a != 0 ? b % a : 0); break; }
#endif
      default: return sp > 0 ? st[sp - 1] : 0;
    }
  }
  return sp > 0 ? st[sp - 1] : 0;
}

}  // namespace

// sub_140AE8B10: function name -> index.
int CalcFunctionIndex(const char* name) {
  if (std::strcmp(name, "min") == 0) return FN_MIN;
  if (std::strcmp(name, "max") == 0) return FN_MAX;
  if (std::strcmp(name, "rand") == 0) return FN_RAND;
  if (std::strcmp(name, "skill") == 0) return FN_SKILL;
  if (std::strcmp(name, "miss") == 0) return FN_MISS;
  if (std::strcmp(name, "stat") == 0) return FN_STAT;
  if (std::strcmp(name, "sklvl") == 0) return FN_SKLVL;
  if (std::strcmp(name, "sksrc") == 0) return FN_SKSRC;
  if (std::strcmp(name, "cond") == 0) return FN_COND;
  return -1;
}

int CalcFunctionArity(int fnIndex) {
  // off_145CEE210 expectedOperands: entries 0-5 and 7 are 2, entry 6
  // (sklvl) is 3, and entry 8 (cond) is dynamic (-1).
  if (fnIndex >= FN_MIN && fnIndex <= FN_STAT) return 2;
  if (fnIndex == FN_SKLVL) return 3;
  if (fnIndex == FN_SKSRC) return 2;
  return -1;
}

int Compiler::Resolve(const char* text, uint8_t top_op, int32_t top_operand,
                      int32_t top_noperands, int cond_slot, int* out_kind) {
  *out_kind = 0;
  const int fn = top_operand;
  const int n = top_noperands;

  // sub_1400F3B84 - string index lookup.
  auto lookup = [&](const char* handle) -> int {
    DataIndex* ix = eng_.GetIndex(handle);
    if (!ix) return -1;
    uint32_t v = ix->str.Lookup(text, false);
    return v == 0xFFFFFFFFu ? -1 : (int)v;
  };
  // sub_140AE7AA0 + sub_14010D278 - code index lookup.
  auto lookupCode = [&](const char* handle) -> int {
    DataIndex* ix = eng_.GetIndex(handle);
    if (!ix) return -1;
    uint32_t c = PackCodeU32(text);
    uint32_t v = ix->code.Lookup(c, true);
    return v == 0xFFFFFFFFu ? -1 : (int)v;
  };
  auto lookupFamilyCodeIndex = [&]() -> int {
    switch (fam_) {
      case CALC_FAM_SKILLS:
      case CALC_FAM_SKILLDESC: return lookupCode("skillcalc_code_index");
      case CALC_FAM_MISSILES: return lookupCode("misscalc_code_index");
#ifdef D2DT_BUILD_93854
      // 93854 adds the moncalc code provider (moncalc.txt: hpct diff lvl elte
      // pcl1..pcl5) and the monpet calc family resolves its bare identifiers
      // through it -- e.g. calc4 "(100-hpct)/((elte==1+elte==4)?20:...)" and
      // bindchancecalc "(pcl1*pcl2*(hpct<85))?(pcl3+pcl4):0".  In the earlier
      // build no such identifiers occur, so the family had no fallback.
      case CALC_FAM_MONPET: return lookupCode("moncalc_code_index");
#endif
      default: return -1;
    }
  };
  if (top_op != BBE_FUNC) {
    // The per-page wrappers call sub_140AE8D90 first, then fall back to the
    // family calc-code index when it declines the identifier (sub_140AE9A30 /
    // sub_140AE9C30). For a non-FUNC top op that fallback is the only path.
    *out_kind = 0;
    return lookupFamilyCodeIndex();
  }
  switch (fn) {
    case FN_SKILL:  // skill
      if (n != 0) { *out_kind = 0; return lookupCode("skillcalc_code_index"); }
      *out_kind = 1;
      return lookup("skills_skill_index_p0");
    case FN_MISS:  // miss
      if (n != 0) { *out_kind = 0; return lookupCode("misscalc_code_index"); }
      *out_kind = 1;
      return lookup("missiles_missile_index");
    case FN_STAT:  // stat
      // sub_14078B9C0 case 5 (sub_14078B9C0 receives a3==1 whenever the
      // operand stack top is the enclosing FUNC, so this path serves EVERY
      // quoted/bare identifier inside stat(...), first operand and flag
      // alike): try the itemstatcost name index -> (NUM, idx); on a miss
      // compare against "base" -> (VAL,1), "mod" -> (VAL,2), and ANY other
      // identifier -- including "accr" and calc codes like "ln12" ->
      // (VAL,0).  The original's strcmp("accr") result is discarded; the
      // shipped mod page proves it with ~180 VAL8(0) flag operands and 7
      // VAL8(0) stat-index operands (stat('ln12',0): itemstatcost miss,
      // keyword miss -> VAL(0)).
      {
        int v = lookup("itemstatcost_stat_index");
        if (v >= 0) { *out_kind = 1; return v; }
        *out_kind = 0;
        if (std::strcmp(text, "base") == 0) return 1;
        if (std::strcmp(text, "mod") == 0) return 2;
        return 0;
      }
    case FN_SKLVL:  // sklvl
      if (n != 0) {
        if (n == 1) {
          char* end = nullptr;
          long v = std::strtol(text, &end, 10);
          if (end != text) { *out_kind = 1; return (int)v; }
        }
        *out_kind = 0;
        return lookupCode("skillcalc_code_index");
      }
      *out_kind = 1;
      return lookup("skills_skill_index_p0");
    case FN_SKSRC:  // sksrc
      if (n != 0) { *out_kind = 0; return lookupCode("skillcalc_code_index"); }
      *out_kind = 1;
      return lookup("skills_skill_index_p0");
    case FN_COND: {  // cond: sub_140AE8D90 -> sub_140154AC4 / sub_14011F743
      if (n == 0) {
        int state = -1;
        if (std::strcmp(text, "IsType") == 0) state = COND_ISTYPE;
        else if (std::strcmp(text, "IsClass") == 0) state = COND_ISCLASS;
        else if (std::strcmp(text, "Desecrated") == 0) state = COND_DESECRATED;
        else if (std::strcmp(text, "MonsterTestElite") == 0) state = COND_MONSTERTESTELITE;
        else if (std::strcmp(text, "ItemIsType") == 0) state = COND_ITEMISTYPE;
        else if (std::strcmp(text, "ItemIsModType") == 0) state = COND_ITEMISMODTYPE;
        else if (std::strcmp(text, "Difficulty") == 0) state = COND_DIFFICULTY;
        else if (std::strcmp(text, "IsDesecratedZonesEnabled") == 0) state = COND_ISDESECRATEDZONESENABLED;
        if (state >= 0 && cond_slot >= 0 && cond_slot < 64)
          cond_state_[cond_slot] = state;
        *out_kind = 1;
        return state;
      }
      if (cond_slot < 0 || cond_slot >= 64) return -1;
      *out_kind = 1;
      // sub_140A26EA0: dispatch on the state saved by the first operand.  Each
      // handler sets *kind = 1 and returns the resolved value, or -1 (with an
      // assert in the original) on an unknown word.
      switch (cond_state_[cond_slot]) {
        case COND_ISTYPE:  // sub_140A272F0
          if (std::strcmp(text, "player") == 0) return CT_PLAYER;
          if (std::strcmp(text, "monster") == 0) return CT_MONSTER;
          if (std::strcmp(text, "object") == 0) return CT_OBJECT;
          if (std::strcmp(text, "missile") == 0) return CT_MISSILE;
          if (std::strcmp(text, "item") == 0) return CT_ITEM;
          if (std::strcmp(text, "roomtile") == 0) return CT_ROOMTILE;
          return -1;
        case COND_ISCLASS: {  // sub_140A27400
          // hPlayerClassCodeIdx (code, len<=4) -> hMonsterStatsIdx (string)
          // -> hMissileStatsIdx (string) -> hItemIdx (code, len<=4).
          const bool short_code = std::strlen(text) <= 4;
          int v = short_code ? lookupCode("playerclass_code_index") : -1;
          if (v >= 0) return v;
          v = lookup("monstats_id_index");
          if (v >= 0) return v;
          v = lookup("missiles_missile_index");
          if (v >= 0) return v;
          if (short_code) {
            v = lookupCode("item_code_index");
            if (v >= 0) return v;
          }
          return -1;
        }
        case COND_MONSTERTESTELITE:  // sub_140A27650
          if (std::strcmp(text, "unique") == 0) return CM_UNIQUE;
          if (std::strcmp(text, "champion") == 0) return CM_CHAMPION;
          if (std::strcmp(text, "super") == 0) return CM_SUPER;
          if (std::strcmp(text, "minion") == 0) return CM_MINION;
          if (std::strcmp(text, "herald") == 0) return CM_HERALD;
          if (std::strcmp(text, "heraldminion") == 0) return CM_HERALDMINION;
          return -1;
        case COND_ITEMISTYPE:  // sub_140A27760 (hItemTypesIdx, len<=4)
          if (std::strlen(text) <= 4) {
            int v = lookupCode("itemtypes_code_index");
            if (v >= 0) return v;
          }
          return -1;
        case COND_ITEMISMODTYPE:  // sub_140A27850
          if (std::strcmp(text, "unique") == 0) return CQ_UNIQUE;
          if (std::strcmp(text, "set") == 0) return CQ_SET;
          if (std::strcmp(text, "rare") == 0) return CQ_RARE;
          if (std::strcmp(text, "magic") == 0) return CQ_MAGIC;
          if (std::strcmp(text, "crafted") == 0) return CQ_CRAFTED;
          if (std::strcmp(text, "lowquality") == 0) return CQ_LOWQUALITY;
          if (std::strcmp(text, "normal") == 0) return CQ_NORMAL;
          if (std::strcmp(text, "hiquality") == 0) return CQ_HIQUALITY;
          if (std::strcmp(text, "tempered") == 0) return CQ_TEMPERED;
          return -1;
        case COND_DIFFICULTY:  // sub_140A279D0
          if (std::strcmp(text, "normal") == 0) return CD_NORMAL;
          if (std::strcmp(text, "nightmare") == 0) return CD_NIGHTMARE;
          if (std::strcmp(text, "hell") == 0) return CD_HELL;
          return -1;
        default:  // COND_DESECRATED / COND_ISDESECRATEDZONESENABLED take no second
                  // operand (sub_140A26CE0 max = 1); -1 otherwise.
          return -1;
      }
    }
    default:
      // sub_140AE8D90 returns 0 for the ordinary math functions (min/max/
      // rand) and other unhandled function contexts. The page-specific wrapper
      // then resolves the identifier through the skillcalc/misscalc code index.
      *out_kind = 0;
      return lookupFamilyCodeIndex();
  }
}

int Compiler::Run(const char* text, uint8_t* out, int max_len) {
  // sub_141A012D0
  n_ops_ = 0;
  n_operands_ = 0;
  for (int& state : cond_state_) state = -1;
  uint8_t* cur = out;
  uint8_t* base = out;
  const char* p = text;
  int has_value = 0;
  bool last_was_value = false;

  for (;;) {
    uint8_t type = 0;
    int value = 0;
    const char* np = Lex(p, &type, &value);
    if (std::getenv("D2DT_TRACE") && std::strlen(text) > 350)
      std::fprintf(stderr, "TRACE np=%d t=%d v=%d p=%d c='%c' ops=%d operands=%d\n",
                   np ? 1 : 0, (int)type, value, (int)(p - text),
                   *p ? *p : '?', n_ops_, n_operands_);
    if (np != nullptr) {
      p = np;
      switch (type) {
        case TOK_LPAREN:  // '('
          cur = Emit(cur, base, max_len, BBE_PAREN, 0);
          break;
        case TOK_RPAREN:  // ')'
          break;
        case TOK_COMMA: cur = Emit(cur, base, max_len, BBE_SEP, 0); break;
        case TOK_ADD: cur = Emit(cur, base, max_len, BBE_ADD, 0); last_was_value = false; break;
        case TOK_SUB:
          cur = Emit(cur, base, max_len, last_was_value ? BBE_SUB : BBE_NEG, 0);
          last_was_value = false;
          break;
        case TOK_MUL: cur = Emit(cur, base, max_len, BBE_MUL, 0); last_was_value = false; break;
        case TOK_DIV: cur = Emit(cur, base, max_len, BBE_DIV, 0); last_was_value = false; break;
        case TOK_QUEST: cur = Emit(cur, base, max_len, BBE_TERNARY, 0); break;
        case TOK_COLON: cur = Emit(cur, base, max_len, BBE_COLON, 0); break;
        case TOK_POW: cur = Emit(cur, base, max_len, BBE_POW, 0); last_was_value = false; break;
#ifdef D2DT_BUILD_93854
        case TOK_MOD: cur = Emit(cur, base, max_len, BBE_MOD, 0); last_was_value = false; break;
#endif
        case TOK_LE: cur = Emit(cur, base, max_len, BBE_LE, 0); last_was_value = false; break;
        case TOK_LT: cur = Emit(cur, base, max_len, BBE_LT, 0); last_was_value = false; break;
        case TOK_GT: cur = Emit(cur, base, max_len, BBE_GT, 0); last_was_value = false; break;
        case TOK_GE: cur = Emit(cur, base, max_len, BBE_GE, 0); last_was_value = false; break;
        case TOK_EQ: cur = Emit(cur, base, max_len, BBE_EQ, 0); last_was_value = false; break;
        case TOK_NE: cur = Emit(cur, base, max_len, BBE_NE, 0); last_was_value = false; break;
        case TOK_NUM:
          cur = EmitNumber(cur, base, max_len, value);
          last_was_value = true;
          break;
        case TOK_VAL:
          cur = EmitValue(cur, base, max_len, value);
          last_was_value = true;
          has_value = 1;
          break;
        case TOK_FUNC:
          cur = Emit(cur, base, max_len, BBE_FUNC, value);
          last_was_value = true;
          has_value = 1;
          break;
        default:
          return 0;
      }
      if (cur == nullptr) return 0;
      if (type == TOK_RPAREN) {
        // ')' (sub_141A012D0, type==1 branch). Pop operators until we hit the
        // matching '(' (PAREN). A FUNC sitting on top is emitted here (its
        // argument list is complete). After the loop, clear the PAREN (or the
        // just-emitted FUNC) from the top; anything else is an unbalanced ')'.
        while (n_ops_ > 0) {
          CalcOp* top = &ops_[n_ops_ - 1];
          if (top->op == BBE_PAREN) break;
          if (top->op == BBE_FUNC) {
            ++top->nOperands;
            int want = CalcFunctionArity(top->operand);
            if (top->op == BBE_FUNC && top->operand == FN_COND) {
              const int slot = (int)(top - ops_);
              if (slot >= 0 && slot < 64) {
                const int state = cond_state_[slot];
                // sub_140A26CE0: states 2/7 take one operand, 0-6 take two.
                if (state == COND_DESECRATED || state == COND_ISDESECRATEDZONESENABLED)
                  want = 1;
                else if (state >= COND_ISTYPE && state <= COND_DIFFICULTY)
                  want = 2;
              }
            }
            if (want >= 0 && top->nOperands != want) return 0;
            int size = OpSize(top);
            if (n_operands_ < size) return 0;
            if (cur - base >= max_len) return 0;
            n_operands_ -= size;
            ++n_operands_;
            *cur++ = top->op;
            // 69270: a FUNC is op + operand only (no size byte); see Emit above.
            *cur++ = (uint8_t)top->operand;
            top->flag = 1;
            break;
          }
          int size = OpSize(top);
          if (n_operands_ < size) return 0;
          if (cur - base >= max_len) return 0;
          n_operands_ -= size;
          ++n_operands_;
          *cur++ = top->op;
          ClearOp(top);
          --n_ops_;
        }
        // post-loop: clear the PAREN we stopped at, or the FUNC just emitted.
        if (n_ops_ > 0) {
          CalcOp* top = &ops_[n_ops_ - 1];
          if (top->op == BBE_PAREN || top->op == BBE_FUNC) {
            ClearOp(top);
            --n_ops_;
          } else {
            return 0;  // unbalanced ')' - a bare operator is on top
          }
        } else {
          return 0;    // unbalanced ')' - nothing to close
        }
      }
      continue;
    }
    // input exhausted: flush
    cur = Emit(cur, base, max_len, BBE_END, 0);
    if (cur == nullptr) return 0;
    if (!has_value) {
      // No value/function operand: sub_140070B30 evaluates the expression and
      // the result replaces it (written from the start of the buffer).
      int folded = EvalConst(base, (int)(cur - base));
      cur = EmitNumber(base, base, max_len, folded);
      if (cur == nullptr) return 0;
      cur = Emit(cur, base, max_len, BBE_END, 0);
      if (cur == nullptr) return 0;
    }
    return (int)(cur - base);
  }
}

int CompileCalcExpression(Engine& eng, uint8_t version, CalcFamilyKind family,
                          const char* text, uint8_t* out, int max_len) {
  if (!text || !*text) return 0;
  Compiler c(eng, version, family);
  return c.Run(text, out, max_len);
}

}  // namespace d2dt
