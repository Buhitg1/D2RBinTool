// d2datatables - standalone D2R excel datatable compiler / dumper.
//
//   d2datatables list
//   d2datatables compile <in_dir> <out_dir> [--only <table>]
//   d2datatables compile1 <table> <in.txt> <out.bin>
//   d2datatables dump <table> <file.bin>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

#include "excel_engine.h"
#include "tables.h"
#include "decompiler.h"

namespace fs = std::filesystem;
using namespace d2dt;

static bool ReadFile(const std::string& p, std::vector<char>& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize((size_t)n);
  if (n) { size_t rd = fread(out.data(), 1, (size_t)n, f); (void)rd; }
  fclose(f);
  return true;
}

static bool WriteFile(const std::string& p, const void* d, size_t n) {
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) return false;
  size_t wr = fwrite(d, 1, n, f);
  fclose(f);
  return wr == n;
}

// Replay one index-bearing field into its exact global handle. This is used for
// callback dependencies that the game has ready before a table's main parse.
static bool SeedIndexFromColumn(Engine& eng, const std::string& in_dir,
                                const char* table, const char* column,
                                int type, int len, const char* handle,
                                int record_size, std::string* err) {
  std::vector<char> txt;
  if (!ReadFile(in_dir + "/" + table + ".txt", txt)) return true;
  const ExcelFieldDesc descs[] = {
    { column, type, len, 0, (void*)handle },
    { nullptr, 0, 0, 0, nullptr },
  };
  const TableDef prepass = { table, table, descs, record_size, 0 };
  std::vector<uint8_t> ignored;
  return eng.Compile(prepass, std::move(txt), ignored, err);
}

// data\global\excel\base -> data\global\excel (returned unchanged when in_dir
// is not a base layer).  The engine resolves a table's folder from a
// GameVersion (0: invalid, 1: base, 2: base, 3: parent) but PINNED the two
// sound tables to the parent: sub_140925410 (d2r.exe 3.1.1) / sub_1401FC290
// (D2R_93854_RAC_China.exe 3.3) force version 3 when the table name is
// "sounds"/"soundenviron", and sub_140422860 (3.1.1) / SoundDataTablesInit
// (3.3) pass a hardcoded 3u for them.  The live sound index space is
// therefore ALWAYS the parent list, in every pass including the base one.
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

// Install the PARENT-shaped sounds_sound_index for a base layer's sound
// consumers.  The layer's own sounds/soundenviron tables compile self-indexed
// first (both are pinned to the parent at load time, so their base-layer bins
// are offline artifacts of the base data set); every table that stores a sound
// reference -- missiles, overlay, states, armor, weapons, misc, skills.stsound,
// monstats, monsounds, uniqueitems, setitems -- is loaded from the base folder
// and resolves against the global (parent-shaped) index, exactly like the
// version-3 read the game performs for the sound tables.
static void ReseedSoundsIndexParentShaped(Engine& eng,
                                          const std::string& in_dir) {
  std::string parent_dir = ParentExcelDir(in_dir);
  if (parent_dir == in_dir) return;  // parent set: the list already is parent-shaped
  std::string src = parent_dir + "/sounds.txt";
  std::vector<char> txt;
  if (!fs::exists(src) || !ReadFile(src, txt) || txt.empty()) return;  // no parent list
  // Tokenise here so the reset only happens once the parent txt is known to
  // parse (Compile's first failure mode is exactly BuildCtx).
  ExcelCtx probe;
  if (!Engine::BuildCtx(std::vector<char>(txt), probe) || probe.n_rows <= 0) {
    std::fprintf(stderr, "warning: parent sounds.txt (%s) has no data rows; "
                         "keeping the layer-shaped sound index\n", src.c_str());
    return;
  }
  // Nothing later reads sounds rows back through the engine (only the
  // horadric cube callbacks do, and only for uniqueitems/setitems), so the
  // prepass compile clobbering table_bins_["sounds"] is inert.
  DataIndex* ix = eng.GetIndex("sounds_sound_index");
  if (ix) ix->str.Reset();
  std::string err;
  if (!SeedIndexFromColumn(eng, parent_dir, "sounds", "Sound",
                           EXCEL_TYPE_TXTIDX16T, 47, "sounds_sound_index",
                           312, &err))
    std::fprintf(stderr, "warning: parent sounds.txt reseed failed: %s\n",
                 err.c_str());
}

// The localisation string table (Key -> id) drives the type 24/25 translation
// callback.  Discover the game's own json localisation relative to the excel
// input dir; fall back to the flattened tsv if none is present.
static bool LoadStrings(Engine& eng, const std::string& in_dir) {
  return eng.LoadStrings(in_dir);
}

static const TableDef* Find(const std::string& name) {
  for (int i = 0; i < kTableCount; ++i)
    if (name == kTables[i].name) return &kTables[i];
  return nullptr;
}

// String-list tables (uniqueappellation / uniqueprefix / uniquesuffix) are a
// single "Name" column compiled into a list of uint16 string indices.
static bool CompileStringList(Engine& eng, const TableDef& def,
                              const std::vector<char>& txt,
                              std::vector<uint8_t>& out, std::string* err) {
  ExcelCtx ctx;
  if (!Engine::BuildCtx(std::vector<char>(txt), ctx)) { if (err) *err = "empty"; return false; }
  DataIndex* ix = eng.GetIndex("json");
  std::vector<uint16_t> idx;
  const char* p = ctx.data;
  for (int r = 0; r < ctx.n_rows; ++r) {
    idx.push_back((uint16_t)ix->str.Intern(p, true));
    p += std::strlen(p) + 1;
    if (*p == '\n') ++p; else if (*p == '\0') ++p;
  }
  out.assign(4 + idx.size() * 2, 0);
  uint32_t n = (uint32_t)idx.size();
  memcpy(out.data(), &n, 4);
  memcpy(out.data() + 4, idx.data(), idx.size() * 2);
  return true;
}

static int Usage() {
  std::printf(
      "d2datatables - D2R excel datatable compiler (ground truth: D2R.exe)\n"
      "  d2datatables list\n"
      "  d2datatables compile <in_dir> <out_dir> [--only <table>]\n"
      "  d2datatables compile1 <table> <in.txt> <out.bin>\n"
      "  d2datatables decompile <in_dir> <out_dir> [--only <table>]\n"
      "  d2datatables dump <table> <file.bin>\n");
  return 1;
}

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  std::string cmd = argv[1];

  if (cmd == "list") {
    for (int i = 0; i < kTableCount; ++i)
      std::printf("%-24s kind=%d recsize=%d\n", kTables[i].name, kTables[i].kind,
                  kTables[i].record_size);
    std::printf("total %d tables\n", kTableCount);
    return 0;
  }

  if (cmd == "compile1") {
    if (argc < 5) return Usage();
    const TableDef* def = Find(argv[2]);
    if (!def) { std::fprintf(stderr, "unknown table '%s'\n", argv[2]); return 2; }
    std::vector<char> txt;
    if (!ReadFile(argv[3], txt)) { std::fprintf(stderr, "cannot read %s\n", argv[3]); return 2; }
    Engine eng(3);
    LoadStrings(eng, fs::path(argv[3]).parent_path().string());
    std::vector<uint8_t> bin;
    std::string err;
    bool ok = (def->kind == 1) ? CompileStringList(eng, *def, txt, bin, &err)
                               : eng.Compile(*def, txt, bin, &err);
    if (!ok) { std::fprintf(stderr, "%s: %s\n", def->name, err.c_str()); return 3; }
    if (!WriteFile(argv[4], bin.data(), bin.size())) return 3;
    std::printf("%s -> %s (%zu bytes)\n", def->name, argv[4], bin.size());
    return 0;
  }

  if (cmd == "compile") {
    if (argc < 4) return Usage();
    std::string in_dir = argv[2], out_dir = argv[3];
    std::string only;
    if (argc >= 6 && std::string(argv[4]) == "--only") only = argv[5];
    fs::create_directories(out_dir);
    Engine eng(3);
    if (!LoadStrings(eng, in_dir)) std::fprintf(stderr, "warning: no localisation strings found\n");
    // Seed indexes consumed before their provider tables' normal kTables
    // position. Each field type/handle mirrors its registered descriptor, so
    // the temporary compile inserts names in exact source row order.
    std::string seed_err;
    if (!SeedIndexFromColumn(eng, in_dir, "skills", "skill", 19, 31,
                             "skills_skill_index", 33, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "monseq", "sequence", 17, 0,
                             "monseq_sequence_index", 2, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "monstats2", "Id", 17, 0,
                             "monstats2_id_index", 2, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "monprop", "Id", 18, 0,
                             "monprop_id_index", 4, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "monsounds", "Id", 17, 0,
                             "monsounds_id_index", 2, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "superuniques", "Superunique", 17, 0,
                             "superuniques_superunique_index", 2, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "monumod", "uniquemod", 17, 0,
                             "monumod_uniquemod_index", 2, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "skilldesc", "skilldesc", 17, 0,
                             "skilldesc_skilldesc_index", 2, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "skilldesc", "skilldesc", 17, 0,
                             "skills_skilldesc_index", 2, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "monstats", "Id", 17, 0,
                             "skills_summon_index", 2, &seed_err) ||
        // sub_1409638A0 / sub_140964C80 fill the uniqueitems/setitems name
        // pools AFTER the parse with sub_140043B44 = addFlag0 (sub_1419DB190
        // a3=0): every row appends, duplicates included, so value == row
        // number.  Type 16 is our addFlag0 path; type 17 (addFlag1) would
        // collapse the 8 duplicate uniqueitems names and shift later values.
        !SeedIndexFromColumn(eng, in_dir, "uniqueitems", "index", 16, 32,
                             "uniqueitems_name_index", 32, &seed_err) ||
        !SeedIndexFromColumn(eng, in_dir, "setitems", "index", 16, 32,
                             "setitems_name_index", 32, &seed_err)) {
      std::fprintf(stderr, "failed to seed early callback indexes: %s\n", seed_err.c_str());
      return 4;
    }
    // sub_140928A60 pushes five range codes into the skills range code index
    // (instance+9912) before registering skills: none/h2h/rng/both/loc with
    // insertion-order values 0..4.
    {
      DataIndex* ix = eng.GetIndex("skills_range_code_index");
      static const char* kRangeCodes[5] = {"none", "h2h", "rng", "both", "loc"};
      for (const char* rc : kRangeCodes) ix->code.Add(PackCodeU32(rc));
    }
    // Slot of the soundenviron table (kind 0): sounds sits immediately before
    // it, and every table that stores a sound reference sits after it.
    int soundenviron_slot = -1;
    for (int i = 0; i < kTableCount; ++i)
      if (kTables[i].kind == 0 && std::strcmp(kTables[i].name, "soundenviron") == 0) {
        soundenviron_slot = i;
        break;
      }
    bool sounds_reseeded = false;
    int ok = 0, skip = 0, fail = 0;
    for (int i = 0; i < kTableCount; ++i) {
      const TableDef& def = kTables[i];
      // Engine boundary: once the layer's own sounds/soundenviron tables have
      // compiled, every remaining table must resolve sound references against
      // the PARENT list -- the engine pins both tables to version 3, so the
      // global sound index space is the parent list in every pass.  Switch
      // the shared handle exactly once, before the first consumer.
      if (soundenviron_slot >= 0 && i > soundenviron_slot && !sounds_reseeded) {
        sounds_reseeded = true;
        ReseedSoundsIndexParentShaped(eng, in_dir);
      }
      if (!only.empty() && only != def.name) continue;
      // kind 4: replayed only for its index side effects
      if (def.kind == 4) {
        // sub_140933080 builds the .txt path from the source name, which for
        // carved sub-tables (objecttokens, leveldefs) differs from the table
        // name; see TableDef::txt_name.
        std::string src = in_dir + "/" + (def.txt_name ? def.txt_name : def.name) + ".txt";
        std::vector<char> txt;
        std::string err;
        if (fs::exists(src) && ReadFile(src, txt)) eng.Compile(def, txt, *(new std::vector<uint8_t>()), &err);
        continue;
      }
      // Treasure class index build (sub_140A97040 side effect).
      if (def.kind == 5) {
        eng.BuildTreasureClassIndex(in_dir + "/itemtypes.txt",
                                    in_dir + "/treasureclassex.txt");
        ++ok;
        continue;
      }
      // Generated calc-expression code page.
      if (def.kind == 3) {
        const std::vector<uint8_t>* pg = eng.GetPage(def.name);
        std::vector<uint8_t> buf;
        if (pg && !pg->empty()) buf = *pg;
        else buf.push_back(0);   // an empty page serialises as one 0x00 byte
        if (!WriteFile(out_dir + "/" + def.name + ".bin", buf.data(), buf.size())) { ++fail; continue; }
        ++ok;
        continue;
      }
      // Opaque code tables have no txt source; round-trip the shipped image.
      if (def.kind == 2) {
        std::string bsrc = in_dir + "/" + def.name + ".bin";
        if (!fs::exists(bsrc)) { ++skip; continue; }
        std::vector<char> raw;
        if (!ReadFile(bsrc, raw)) { ++skip; continue; }
        if (!WriteFile(out_dir + "/" + def.name + ".bin", raw.data(), raw.size())) { ++fail; continue; }
        ++ok;
        continue;
      }
      std::string src = in_dir + "/" + (def.txt_name ? def.txt_name : def.name) + ".txt";
      if (!fs::exists(src)) { ++skip; continue; }
      std::vector<char> txt;
      if (!ReadFile(src, txt)) { ++skip; continue; }
      std::vector<uint8_t> bin;
      std::string err;
      bool r = (def.kind == 1) ? CompileStringList(eng, def, txt, bin, &err)
                               : eng.Compile(def, txt, bin, &err);
      if (!r) { std::printf("FAIL %-24s %s\n", def.name, err.c_str()); ++fail; continue; }
      std::string dst = out_dir + "/" + def.name + ".bin";
      if (!WriteFile(dst, bin.data(), bin.size())) { ++fail; continue; }
      ++ok;
    }
    std::printf("compiled=%d skipped=%d failed=%d\n", ok, skip, fail);
    return fail ? 4 : 0;
  }

  if (cmd == "decompile") {
    if (argc < 4) return Usage();
    std::string in_dir = argv[2], out_dir = argv[3];
    std::string only;
    if (argc >= 6 && std::string(argv[4]) == "--only") only = argv[5];
    Engine eng(3);
    if (!LoadStrings(eng, in_dir)) std::fprintf(stderr, "warning: no localisation strings found\n");
    int bad = RunDecompile(eng, in_dir, out_dir, only);
    return bad ? 5 : 0;
  }

  if (cmd == "dump") {
    if (argc < 4) return Usage();
    const TableDef* def = Find(argv[2]);
    if (!def) { std::fprintf(stderr, "unknown table '%s'\n", argv[2]); return 2; }
    std::vector<char> raw;
    if (!ReadFile(argv[3], raw)) { std::fprintf(stderr, "cannot read %s\n", argv[3]); return 2; }
    if (raw.size() < 4) return 2;
    uint32_t rows = 0; memcpy(&rows, raw.data(), 4);
    int rs = def->record_size;
    std::printf("table=%s rows=%u record_size=%d body=%zu\n", def->name, rows, rs, raw.size() - 4);
    if (rows && rs && (raw.size() - 4) == (size_t)rows * rs) std::printf("layout OK\n");
    else std::printf("layout MISMATCH\n");
    return 0;
  }

  return Usage();
}
