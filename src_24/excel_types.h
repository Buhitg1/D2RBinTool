// D2R excel datatable engine - field type definitions.
//
// Ground truth: D2R.exe (IDA hash 2b648d1dca70)
//   type -> {doctype, len, memsize} table : sub_1419D9950  (Excel.cpp:0x4A2)
//   txt parser switch                     : sub_1419D6EA0
// The numbering below is the raw nType value stored in ExcelFieldDesc.type.
#pragma once
#include <cstdint>

namespace d2dt {

enum ExcelType : int32_t {
  EXCEL_TYPE_NONE = 0,      // terminator of a FieldDesc array
  EXCEL_TYPE_TEXT = 1,      // text, nFieldLength chars (schema length)
  EXCEL_TYPE_INT32 = 2,
  EXCEL_TYPE_INT16 = 3,
  EXCEL_TYPE_INT8 = 4,
  EXCEL_TYPE_BYTE_OR = 5,   // int8, written only if a digit was consumed
  EXCEL_TYPE_INT8B = 6,     // alias of int8
  EXCEL_TYPE_TEXT7 = 7,     // alias of text
  EXCEL_TYPE_INT32B = 8,    // alias of int32
  EXCEL_TYPE_CODE = 9,      // 4-char code, packed and space padded, stored raw
  EXCEL_TYPE_CODE_IDX32 = 10,  // 4-char code -> u32 index (intern/add)
  EXCEL_TYPE_CODE_REF32 = 11,  // 4-char code -> u32 index (lookup)
  EXCEL_TYPE_CODE_IDX8 = 12,   // as 10, stored u8
  EXCEL_TYPE_CODE_REF8 = 13,   // as 11, stored u8
  EXCEL_TYPE_CODE_IDX16 = 14,  // as 10, stored u16
  EXCEL_TYPE_CODE_REF16 = 15,  // as 11, stored u16
  EXCEL_TYPE_TEXT16 = 16,      // text + string index register (no add)
  EXCEL_TYPE_TXTIDX16 = 17,    // text -> u16 string index (add)
  EXCEL_TYPE_TXTIDX32 = 18,    // text -> u32 string index (add)
  EXCEL_TYPE_TXTIDX16T = 19,   // u16 string index (add) followed by raw text
  EXCEL_TYPE_TXTIDX32T = 20,   // u32 string index (add) followed by raw text
  EXCEL_TYPE_STRREF32 = 21,    // text -> u32 string index (lookup)
  EXCEL_TYPE_STRREF16 = 22,    // text -> u16 string index (lookup)
  EXCEL_TYPE_STRREF8 = 23,     // text -> u8 string index (lookup)
  EXCEL_TYPE_STRCALL16 = 24,   // text -> u16 via callback at desc->ptr
  EXCEL_TYPE_STRCALL32 = 25,   // text -> u32 via callback at desc->ptr
  EXCEL_TYPE_PARSE26 = 26,     // text -> callback(version, text, row, size, len, r, c)
  EXCEL_TYPE_PARSE27 = 27,
  EXCEL_TYPE_PARSE28 = 28,
  EXCEL_TYPE_BOOL = 29,        // value != 0 -> set bit desc->len of the byte
  EXCEL_TYPE_INVBOOL = 30,     // value == 0 -> set bit desc->len of the byte
};

// Opaque handle to a code/string index table (the game stores instance+hIndex).
struct DataIndex;

typedef uint16_t (*StringCall16)(const char* text);
typedef uint32_t (*StringCall32)(const char* text);
typedef void (*ParseCall)(uint8_t version, const char* text, char* row,
                          int fieldSize, int fieldLen, int rowIdx, int colIdx);

// 32-byte field descriptor, identical layout to the game's structure.
struct ExcelFieldDesc {
  const char* name;
  int32_t type;
  int32_t len;
  int64_t offset;
  void* ptr;  // DataIndex* or callback
};

}  // namespace d2dt
