// D2R excel datatable engine - port of Excel.cpp / DataTbls.cpp.
//
// Ported from the D2R.exe decompilation; control flow mirrors the original:
//   sub_1419D8500  build tokenised txt context
//   sub_1419DB410  map txt columns onto FieldDesc entries
//   sub_1419D6EA0  parse rows into the struct image
//   sub_1419DB190  string index intern / lookup
//   sub_1419D9080  code index add
//   sub_1419D8BC0  code index lookup
//   sub_1419D9380  string index lookup
//   sub_1419DAFD0  pack a <=4 char code, space padded
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include "excel_types.h"
#include "json.h"

namespace d2dt {

// ------------------------------------------------------------------ indexes

// String index table (Excel.cpp "string index").  It is seeded from the
// localisation json (Key -> numeric id) and appends a fresh sequential id for
// any key that is not present.
class StringIndex {
 public:
  uint32_t Intern(const char* s, bool add);           // sub_1419DB190
  uint32_t Lookup(const char* s, bool strict) const;  // sub_1419D9380
  void Seed(const std::string& key, uint32_t id);     // json load
  // Drop every interned key/id and restart the id sequence.  Used to re-point
  // a shared handle at a different list mid-run: the sounds tables are pinned
  // to the parent folder (sub_140925410 / sub_1401FC290 force version 3;
  // sub_140422860 / SoundDataTablesInit pass a hardcoded 3u), so a base layer
  // must resolve sound names against the PARENT list once its own sounds and
  // soundenviron tables have compiled.  empty_index_ survives: it is
  // per-handle data recovered from the shipped bins (kIndexDefaults), not a
  // function of the interned keys.
  void Reset() {
    map_.clear();
    by_index_.clear();
    next_id_ = 0;
  }
  uint32_t count() const { return next_id_; }
  uint32_t empty_index() const { return empty_index_; }
  void set_empty_index(uint32_t v) { empty_index_ = v; }

  // Decompiler support: index -> the exact text that produced it (insertion
  // order, including the add=false duplicate appends).
  const std::string* TextAt(uint32_t idx) const {
    return idx < by_index_.size() ? &by_index_[idx] : nullptr;
  }

 private:
  std::unordered_map<std::string, uint32_t> map_;
  std::vector<std::string> by_index_;
  uint32_t next_id_ = 0;
  uint32_t empty_index_ = 0;
};

// Code index table (Excel.cpp "code index"): sorted {code, index} pairs.
class CodeIndex {
 public:
  uint32_t Add(uint32_t code);                          // sub_1419D9080
  uint32_t Lookup(uint32_t code, bool strict) const;    // sub_1419D8BC0
  uint32_t count() const { return count_; }
  uint32_t size() const { return (uint32_t)entries_.size(); }

  // Decompiler support: index -> the packed code key that was registered
  // under it (post duplicate-bump), so type 11/13/15 cells can be inverted.
  bool CodeAt(uint32_t idx, uint32_t* code) const {
    if (idx >= by_index_.size()) return false;
    *code = by_index_[idx];
    return true;
  }

 private:
  std::map<uint32_t, uint32_t> entries_;  // code -> index
  std::vector<uint32_t> by_index_;        // index -> code
  uint32_t count_ = 0;
};

// One index handle as referenced by ExcelFieldDesc::ptr.  The game stores an
// instance-relative offset; we key by that offset string.
struct DataIndex {
  StringIndex str;
  CodeIndex code;
};

// ------------------------------------------------------------------ context

// Tokenised .txt image.  Layout matches the game's ExcelCtx:
//   data   <- ctx+24 (start of body, cells NUL separated, lines end "\0\n")
//   n_rows <- ctx+32
//   n_cols <- ctx+36
struct ExcelCtx {
  std::vector<char> buf;
  const char* header = nullptr;  // first header cell (sub_14018E599)
  char* data = nullptr;          // ctx+24: first body cell
  int32_t n_rows = 0;            // ctx+32
  int32_t n_cols = 0;            // ctx+36
};

// ------------------------------------------------------------------ engine

class Engine {
 public:
  explicit Engine(uint8_t version) : version_(version) {}

  // Tokenise a .txt image (sub_1419D8500).  Returns false if the file is empty
  // or has no data rows.
  static bool BuildCtx(std::vector<char>&& file, ExcelCtx& ctx);

  // Compile one table: tokenise -> map columns -> parse rows -> bin image.
  // out_bin receives  u32 row_count  followed by  row_count * record_size bytes.
  bool Compile(const struct TableDef& def, std::vector<char> txt,
               std::vector<uint8_t>& out_bin, std::string* err);

  // Same, but from an already tokenised context (useful for the dump path).
  bool CompileCtx(const struct TableDef& def, ExcelCtx& ctx,
                  std::vector<uint8_t>& out_bin, std::string* err);

  DataIndex* GetIndex(const std::string& handle);

  // Compiled images of earlier tables (name -> u32 rowcount + rows).  The
  // horadric cube callbacks read uniqueitems/setitems rows back through
  // sub_1400F9917 / sub_1401146F9 style row access.
  const std::vector<uint8_t>* GetTableRows(const std::string& name) const {
    auto it = table_bins_.find(name);
    return it == table_bins_.end() ? nullptr : &it->second;
  }

  // Calc-expression code pages (skillscode / skilldesccode / misscode /
  // itemscode).  Expressions are appended as rows are parsed and the resulting
  // offset is stored in the field, exactly like sub_140148481.
  const std::vector<uint8_t>* GetPage(const std::string& name) const;

  // sub_140A97040 -> sub_140A95370 + sub_140A960E0: the treasure class index
  // (instance+3440) is not filled by a FieldDesc.  It gets the literal "TC",
  // then "<code><3..96 step 3>" for every item type whose "treasureclass" flag
  // is set, then the treasureclassex names in row order.
  bool BuildTreasureClassIndex(const std::string& itemtypes_txt,
                               const std::string& tcex_txt);

  // Decompiler variant: same pool (sub_140A97040) rebuilt from the compiled
  // itemtypes/treasureclassex images instead of the txt sources.
  bool BuildTreasureClassIndexFromBins(const std::vector<uint8_t>& itemtypes_bin,
                                       const std::vector<uint8_t>& tcex_bin);
  std::vector<uint8_t>& Page(const std::string& name) { return pages_[name]; }

  // Localisation loading.  The game's string table maps a localisation Key to a
  // numeric id (type 24/25 translate_string fields).  The one and only source
  // is the game's own json (<root>/local/lng/strings/*.json, discovered by
  // walking up from `in_dir`) - the format the engine itself reads.  If none is
  // discoverable (an excel-only extraction) nothing is loaded and the type
  // 24/25 callback returns the engine's default 5382 for every cell; point the
  // tool at a data dir that carries local/.  Must be called before compiling.
  bool LoadStrings(const std::string& in_dir);
  bool LoadStringsFromJsonDir(const std::string& dir);  // explicit dir
  bool strings_loaded() const { return json_max_id_ != 0; }
  // Walk up from in_dir (8 levels) for the game's <root>/local/lng/strings
  // directory.  Exposed so the decompiler can carry the localisation with its
  // output -- the generated txt references string Keys, not raw ids.
  static bool FindStringsDir(const std::string& in_dir, std::string& out_dir);

  // Reverse map (id -> Key) for the decompiler's STRCALL inversion.  Built
  // alongside the forward map; first id occurrence wins.
  const std::unordered_map<uint32_t, std::string>* reverse_translation() const {
    return &reverse_strings_;
  }

  uint8_t version() const { return version_; }

 private:
  // sub_1419DB410
  int MapColumns(uint16_t* out_map, const ExcelCtx& ctx,
                 const ExcelFieldDesc* descs) const;
  // sub_1419D6EA0
  void ParseRows(const ExcelCtx& ctx, const ExcelFieldDesc* descs, char* out,
                 int stride, int32_t n_rows, const uint16_t* map, int map_count);

  std::vector<ExcelFieldDesc> Resolve(const TableDef& def);

  uint8_t version_;
  std::unordered_map<std::string, DataIndex*> indexes_;
  std::unordered_map<std::string, std::vector<uint8_t>> table_bins_;
  std::unordered_map<std::string, uint32_t> json_strings_;       // Key -> id
  std::unordered_map<uint32_t, std::string> reverse_strings_;    // id -> Key
  std::unordered_map<std::string, std::vector<uint8_t>> pages_;
  uint32_t json_max_id_ = 0;
};

// sub_1419DAFD0 - copy up to 4 chars into dst, pad the rest with spaces.
void PackCode(const char* src, char* dst);

// sub_140105A4B + sub_140A8C950 - pack a <=4 char code into the u32 the code
// indexes use.
uint32_t PackCodeU32(const char* src);

}  // namespace d2dt
