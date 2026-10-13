// Port of the BBE calc-expression compiler.
//
//   sub_1400AF457 -> sub_141A012D0   driver (shunting-yard)
//   sub_141A02250                    lexer
//   sub_141A032F0                    operator emit
//   sub_141A037F0 / sub_141A03A70    number / value operand emit
//   sub_141A03230                    operator operand count
//   sub_141A03180 + byte_14601AA70   precedence
//   sub_141A021D0 / sub_141A057E0    stack helpers
//   sub_140AE8D90                    identifier resolver (context dependent)
//
// The result is a byte string that the caller appends to a per-family code page
// (sub_140148481) and stores the previous page size as the field value.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace d2dt {

class Engine;

// BBE opcodes (the emitted bytes).
enum BbeOp {
  BBE_END = 0,
  BBE_FUNC = 1,
  BBE_PAREN = 2,
  BBE_SEP = 3,        // ','
  BBE_NUM8 = 7,
  BBE_NUM16 = 8,
  BBE_NUM32 = 9,
  BBE_VAL8 = 4,
  BBE_VAL16 = 5,
  BBE_VAL32 = 6,
  BBE_LT = 0x0A,
  BBE_GT = 0x0B,
  BBE_LE = 0x0C,
  BBE_GE = 0x0D,
  BBE_EQ = 0x0E,
  BBE_NE = 0x0F,
  BBE_ADD = 0x10,
  BBE_SUB = 0x11,
  BBE_MUL = 0x12,
  BBE_DIV = 0x13,
#ifdef D2DT_BUILD_93854
  // 3.3 (93854) adds the remainder operator '%' and inserts it at 0x14 with
  // the multiplicative precedence (19), renumbering POW/NEG/TERNARY/COLON up by
  // one.  Ground truth: the opcode precedence table at 0x142413AA8 in
  // D2R_93854_RAC_China.exe is 24 entries with BBE_LAST = 0x18
  // (24,1,2,3,9*6,15*6,17,17,19,19,19,21,22,23), where d2r.exe has 23 entries
  // with BBE_LAST = 0x17 (...,19,19,20,21,22) at 0x14601AA70.  The new '%' slot
  // shows up in the pages as a byte we never emitted, and every NEG/TERN byte
  // in the shipped pages is exactly one higher than ours.
  BBE_MOD = 0x14,
  BBE_POW = 0x15,
  BBE_NEG = 0x16,
  BBE_TERNARY = 0x17,
  BBE_COLON = 0x18,   // also BBE_LAST: the first opcode the stack never holds
#else
  BBE_POW = 0x14,
  BBE_NEG = 0x15,
  BBE_TERNARY = 0x16,
  BBE_COLON = 0x17,   // also BBE_LAST
#endif
};

// Identifier-resolution context, taken from sub_140AE8D90's switch on the
// enclosing function index (sub_140AE8B10 order).
enum CalcFamilyKind {
  CALC_FAM_SKILLS = 0,     // sub_140AE7C60  (page: skillscode)
  CALC_FAM_SKILLDESC = 1,  // sub_140AE7CF0  (page: skilldesccode)
  CALC_FAM_MISSILES = 2,   // sub_140AE7D80  (page: misscode)
  CALC_FAM_ITEMS = 3,      // sub_140AE7E10  (page: itemscode)
  CALC_FAM_MONPET = 4,     // sub_140AE7EA0
  CALC_FAM_ITEMS2 = 5,     // sub_140AE7F30
};

// Function indices.  sub_140AE8B10 maps the name to one of these, and the
// resolver (sub_140AE8D90, `switch ((int)v72)`) switches on it.  Only 3..8 are
// handled there; 0/1/2 (min/max/rand) fall through to the `default: return 0`.
enum CalcFn {
  FN_MIN = 0,
  FN_MAX = 1,
  FN_RAND = 2,
  FN_SKILL = 3,
  FN_MISS = 4,
  FN_STAT = 5,
  FN_SKLVL = 6,
  FN_SKSRC = 7,
  FN_COND = 8,
};

// cond(...) predicate states.  The first operand of cond(...) is a predicate
// name; sub_140A26D80 (== sub_140154AC4) maps it to one of these, saved in the
// enclosing call's slot, and the second operand is dispatched on that state by
// sub_140A26EA0 (== sub_14011F743).  States 2 and 7 take NO second operand
// (sub_140A26CE0 max-operand count is 1 for them), so they are absent from the
// dispatch switch and fall to its `default` (assert + 0xFFFFFFFF).
enum CondState {
  COND_ISTYPE = 0,          // sub_140A272F0
  COND_ISCLASS = 1,         // sub_140A27400
  COND_DESECRATED = 2,      // no second operand
  COND_MONSTERTESTELITE = 3,// sub_140A27650
  COND_ITEMISTYPE = 4,      // sub_140A27760
  COND_ITEMISMODTYPE = 5,   // sub_140A27850
  COND_DIFFICULTY = 6,      // sub_140A279D0
  COND_ISDESECRATEDZONESENABLED = 7,  // no second operand
};

// The second operand of cond(...), a per-state name -> number mapping.  Ground
// truth: the per-state handlers named on each enum below.
enum CondTypeKind {   // COND_ISTYPE / sub_140A272F0
  CT_PLAYER = 0, CT_MONSTER = 1, CT_OBJECT = 2,
  CT_MISSILE = 3, CT_ITEM = 4, CT_ROOMTILE = 5,
};
enum CondMonsterFlag {  // COND_MONSTERTESTELITE / sub_140A27650 (bit mask)
  CM_SUPER = 2, CM_CHAMPION = 4, CM_UNIQUE = 8,
  CM_MINION = 16, CM_HERALD = 512, CM_HERALDMINION = 1024,
};
enum CondItemQuality {  // COND_ITEMISMODTYPE / sub_140A27850
  CQ_LOWQUALITY = 1, CQ_NORMAL = 2, CQ_HIQUALITY = 3, CQ_MAGIC = 4,
  CQ_SET = 5, CQ_RARE = 6, CQ_UNIQUE = 7, CQ_CRAFTED = 8, CQ_TEMPERED = 9,
};
enum CondDifficulty {   // COND_DIFFICULTY / sub_140A279D0
  CD_NORMAL = 0, CD_NIGHTMARE = 1, CD_HELL = 2,
};

// sub_140AE8B10: function name -> index (min,max,rand,skill,miss,stat,sklvl,
// sksrc,cond), or -1.
int CalcFunctionIndex(const char* name);

// sub_140AE8C70: expected operand count for a function (2 for all but cond,
// which is dynamic).
int CalcFunctionArity(int fnIndex);

// Compile `text` with the BBE compiler.  On success returns the number of
// bytes written to `out` (> 0); on failure returns 0 and the caller stores -1.
int CompileCalcExpression(Engine& eng, uint8_t version, CalcFamilyKind family,
                          const char* text, uint8_t* out, int max_len);

}  // namespace d2dt
