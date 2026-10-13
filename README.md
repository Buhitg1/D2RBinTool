# d2datatables - standalone D2R excel datatable compiler

A C++17 port of the Diablo II: Resurrected excel datatable engine
(`Excel.cpp` / `DataTbls.cpp`), plus a standalone command line compiler that
turns the `.txt` source tables into the game's `.bin` images.

Everything here is derived from the decompilation of `D2R.exe`
(IDA database hash `2b648d1dca70`).  The port keeps the original control flow
and the original field-type numbering; where the game stores a raw pointer we
keep the same 32-byte descriptor layout.

## Layout

```
src/excel_types.h    field type enum + ExcelFieldDesc (32 bytes, game layout)
src/excel_engine.h   engine API (index tables, tokeniser, mapper, parser)
src/excel_engine.cpp ported implementation
src/tables.h         generated: packed memory-layout struct per datatable
src/tables.cpp       generated: FieldDesc array + registry per datatable
src/main.cpp         CLI driver
build.bat            MSVC build script (explicit toolchain paths)
```

The `tables.*` files are produced by `../tools/gen_tables.py` from
`../schemas_parsed_local.json`, which in turn is produced by
`../tools/parse_schemas.py` from the Hex-Rays pseudocode of every loader.

## Build

```
cd d2datatables
build.bat
```

`build.bat` sets `INCLUDE`/`LIB` explicitly so that `vcvars64.bat` (which calls
`reg.exe`) is not required.  Adjust `MSVC` / `SDK` / `SDKVER` if your toolchain
differs.

## Build variants: 3.1.1 (default) and 3.3 / build 93854

One source tree builds **both** shipped game builds.  The 3.3 engine
(`D2R_93854_RAC_China.exe`, build 93854, IDA hash `98abc2a0a58d`) differs from
the 3.1.1 engine (`D2R.exe`, build 91923, IDA hash `2b648d1dca70`) in four table
schemas, one new txt-only provider table and one new calc operator.  All of it
is gated behind `D2DT_BUILD_93854`, so the default build is byte-identical to
before.

```
cd d2datatables
build.bat           rem 3.1.1  -> build\        (no macro)
build_93854.bat     rem 3.3    -> build_93854\  (/DD2DT_BUILD_93854)
```

What `D2DT_BUILD_93854` selects (from `../schemas_93854.json`, derived by
per-table loader disassembly on the assist mesh):

| table | 3.1.1 | 3.3 / 93854 |
|---|---|---|
| `charstats` | 82 fields, rec 208 | 84 fields, rec 212 — `+TwoHandedOffHandRestrictItemType` (t15 @208), `+TwoHandedDamageAsOneHanded` (t4 @210) |
| `levels` | 143 fields, rec 396 | 144 fields, rec 404 — `+CompletionTotalRoomsOverride` (t2 @16) |
| `monpet` | 19 fields, rec 68 | 34 fields, rec 120 — `+calc1..5`, `+BoundStat1..5`, `+BoundCalc1..5` |
| `superuniques` | 22 fields, rec 60 | 22 fields, rec 52 — tail repacked; `MonSound` 28→20, `Mod1/2/3` 16/20/24→16/17/18 |
| `moncalc` | — | new txt-only code provider (1 field `code` t10, rec 4) |
| `objtype` | present | dropped |
| BBE `%` | — | new opcode `BBE_MOD = 0x14`, rest of the enum shifted +1 |

The four schema changes are exactly the changelog's `charstats` /
`Levels` / `MonPet` column additions; the new opcode is the changelog's
*"Added remainder operator `%` support"*, which lands at `0x14` in the
precedence table (see `../RE_NOTES.md` for the table addresses).  `moncalc` is
the provider the new `MonPet#calc#` / `BoundCalc#` columns resolve against.

The default branch of the generated `src/tables.{h,cpp}` is byte-identical to
the pre-93854 output; `tools/pp_branch.py` selects one branch out of the
generated file so the two can be diffed.

### 93854 verification

See `../VERIFY-93854.md`.  `data_93847/global/excel` (+ `base/`) compiled with
the 93854 engine and compared byte-for-byte against the shipped `.bin`:

- **parent**: 88 shipped `.bin`, all 88 byte-exact.
- **base**: 86 shipped `.bin`, all 86 byte-exact.

The base total is 3 lower than parent's **by construction**, not by extra
failures: the 3.3 base set ships no `.bin` for `sounds` / `soundenviron`
(they are `.txt`-only there, so there is nothing to compare), and `objtype`
ships no `.bin` in either set (dropped in 3.3, which is why parent is 88 and
not 89).  Each verdict counts only the `.bin` files that set actually ships.
- `decompile` round-trips both sets exactly (`decompiled=91 mismatched=0
  skipped=4 warns=0`).

`data_93847/local/lng/strings/*.json` is a strict superset of the 3.1.1 strings
(all 12118 shared keys carry the same id; the only new key is
`StrSkillChainMaxCount = 28105`).  Each build loads its own json directly - no
generated intermediate table is involved.

## Building

The CMake build is the primary path.  It needs nothing beyond a C++17 compiler
(MSVC 19.3x+, GCC 9+, Clang 10+) — the json parser, the table descriptors and
the calc tables are all part of the source tree:

    cmake -S d2datatables -B <build-dir>
    cmake --build <build-dir> --config Release

It produces **both** engine variants, written to the directories the verify
scripts (`tools/verify.py`, `tools/verify_93854.py`) and the `VERIFY*.md`
reports read, no matter which `<build-dir>` you configure:

| target                | binary                                     | engine |
|-----------------------|--------------------------------------------|--------|
| `d2datatables`        | `d2datatables/build/d2datatables.exe`      | 3.1.1 (game build 91923) |
| `d2datables_93854`    | `d2datatables/build_93854/d2datatables.exe` | 3.3 (game build 93854, `-DD2DT_BUILD_93854`) |

With a multi-config generator (Visual Studio, Ninja Multi-Config) pass
`--config Release`; with a single-config one (Ninja, Unix Makefiles) configure
with `-DCMAKE_BUILD_TYPE=Release` instead.  `d2datatables/CMakeLists.txt`
routes both binaries into the two directories above for every configuration —
only the executables land there, CMake's intermediates stay under `<build-dir>`.

`build.bat` and `build_93854.bat` remain as a no-CMake fallback: they invoke
`cl.exe` directly with the toolchain paths pinned explicitly (which is how the
current `VERIFY*.md` numbers were produced).

### Windows notes (this machine: VS 18 Enterprise preview, MSVC 14.51.36231, SDK 10.0.26100.0)

Both notes apply only when driving the build from a plain shell rather than a
*Developer PowerShell / x64 Native Tools* prompt:

- The bundled `Visual Studio 18 2026` generator cannot locate the compiler in
  this install (`No CMAKE_CXX_COMPILER could be found`).  Configure with the
  NMake generator and the toolchain on `PATH` instead — that is what
  `build.bat` sets up by hand:

      $MSVC   = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
      $SDK    = "C:\Program Files (x86)\Windows Kits\10"
      $SDKVER = "10.0.26100.0"
      $env:INCLUDE = "$MSVC\include;$SDK\Include\$SDKVER\ucrt;$SDK\Include\$SDKVER\um;$SDK\Include\$SDKVER\shared;$SDK\Include\$SDKVER\winrt"
      $env:LIB     = "$MSVC\lib\x64;$SDK\Lib\$SDKVER\ucrt\x64;$SDK\Lib\$SDKVER\um\x64"
      $env:PATH    = "$MSVC\bin\Hostx64\x64;$SDK\bin\$SDKVER\x64;$env:PATH"
      cmake -S d2datatables -B <build-dir> -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
      cmake --build <build-dir>

- The SDK's `rc.exe` (`$SDK\bin\$SDKVER\x64`) must be on `PATH` as well: with
  the NMake generator CMake links MSVC executables through a separate resource
  step for the manifest, and without `rc` the link fails with
  `RC Pass 1 ... no such file or directory`.

The bundled CMake (4.3.1 here) sits at
`C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.

## Usage

```
d2datatables list
d2datatables compile <in_dir> <out_dir> [--only <table>]
d2datatables compile1 <table> <in.txt> <out.bin>
d2datatables decompile <in_dir> <out_dir> [--only <table>]
d2datatables dump <table> <file.bin>
```

### Examples

Compile both shipped sets of the vanilla game (each layer is compiled on its
own, because 35 of the 90 `.txt` differ between base and parent):

    d2datatables compile data/global/excel/base out_bin
    d2datatables compile data/global/excel      out_bin_parent

Decompile a layer back to `.txt`, proving the round trip (every generated file
is recompiled and byte-diffed against the shipped `.bin`):

    d2datatables decompile data/global/excel/base out_dec_base
    d2datatables decompile data/global/excel      out_dec_parent

Compile a mod (same directory layout as the game's data folder — this is how
`../VERIFY-Reimagined.md` was produced):

    d2datatables compile <mod>/data/global/excel/base out_mod_base
    d2datatables compile <mod>/data/global/excel      out_mod_parent

Single-table conveniences:

    d2datatables compile1 armor data/global/excel/armor.txt armor.bin
    d2datatables dump     sounds data/global/excel/sounds.bin

`--only <table>` processes one table (debug convenience): cross-table index
state is only complete in a full run, so a single-table bin can still miss
references that earlier registrations would have resolved.

### `compile` needs the complete set of source `.txt`

`compile` walks the tables in the game's registration order
(`DataTablesInitAll`) and shares **one** set of global index tables across the
whole run — string indexes, code indexes, the (parent-shaped) sound index space
and the calc code pages all build up incrementally as the tables register. A
table whose `.txt` is missing is simply skipped, and everything that resolves
through it afterwards resolves wrong (empty index → missed lookups), so **every
`.txt` of the layer must be present, not just the ones you changed**:

- the 9 txt-only providers (`bodylocs`, `colors`, `elemtypes`, `events`,
  `misscalc`, `monai`, `monplace`, `skillcalc`, `storepage`) own code/string
  indexes that other tables look up;
- the early-seeded callback dependencies (`skills`, `monseq`, `monstats2`,
  `monprop`, `monsounds`, `superuniques`, `monumod`, `skilldesc`, `monstats`,
  `uniqueitems`, `setitems`) must be present or their seeds stay empty;
- sound references resolve through the parent sound list — see
  [The sound index space](#the-sound-index-space-the-engine-pins-soundssoundenviron-to-the-parent)
  below.

`local/lng/strings/*.json` is required too (type 24/25 fields) — see
[Localisation json](#localisation-json--required-by-both-commands).

For a **mod** that means compiling against a *complete* directory, unmodded
tables included. The game itself reads the union of the mod MPQ data and the
base game data, so a compile has to see the same union: either the mod ships
its full data set (as Reimagined does — verified byte-exact), or you build a
merged directory with the mod's files overlaid on the full game data. A
"changed files only" folder is not a valid compile input, because the index
state the game builds depends on the tables you left out.

### `decompile` needs the `.bin` plus a minimum set of reference `.txt`

`decompile` is the inverse: it rebuilds every `.txt` from the shipped `.bin`
images and recompiles each one to prove a byte-exact round trip.  It needs the
89 `.bin` plus only **9** of the 90 shipped `.txt`; the other 81 are derived
from the binaries.  The 9 are the txt-only providers listed above — `bodylocs`,
`colors`, `elemtypes`, `events`, `misscalc`, `monai`, `monplace`, `skillcalc`,
`storepage` — and every one of them is individually necessary: removing any
single one produces at least one round-trip mismatch.  What each one is needed
for, and why the other 81 are genuinely recoverable, is documented in
[`DECOMPILE_TXT_DEPS.md`](DECOMPILE_TXT_DEPS.md).  The localisation json is a
second, independent input — see
[Localisation json](#localisation-json--required-by-both-commands).

### Localisation json — required by both commands

Datatable fields of type 24/25 (`translate_string`) do not store text: they
store the **numeric id** of a localisation `Key` (`sub_140924FB0` →
`sub_140F44A10(str, 5382)`, a lookup that returns the caller's default `5382`
on a miss — which is why `books.name` is `0x1506` for every row, and why
`uniqueappellation` stores `2825` for "the Hammer").  Both commands therefore
need the game's Key ↔ id map, and they read it straight from the game's own
localisation files:

```
<data_root>/
  data/global/excel/...      <- the <in_dir> you pass on the command line
  local/lng/strings/*.json   <- 21 files, e.g. item-nameaffixes.json
```

`local/lng/strings/*.json` is the format the engine itself loads
(`d2json`-shaped `[{"Key": "...", "id": N}, ...]` arrays; first occurrence of a
Key wins).  The tool discovers the directory by walking **up** from `<in_dir>`,
so `data/global/excel/base` resolves to `<data_root>/local/lng/strings` — pass
the excel dir, not the data root.  It reads the 21 top-level `*.json` files;
the `metadata/` subtree beside them is not part of the Key ↔ id map (every type
24/25 cell in all verified corpora reproduces byte-exactly without it).  Every
verified corpus carries it: all three retail sets and the Reimagined mod.

`compile` uses it to turn `Name`-style columns into the stored ids;
`decompile` uses the reverse map (id → Key) to invert them again.  If no json
is discoverable — an excel-only extraction, say — the tool prints
`warning: no localisation strings found` and every type 24/25 cell silently
falls back to the default `5382`, so the resulting bins/txt are wrong in those
columns.  Point the tool at a data dir that includes `local/`.

## The binary format

```
uint32   row_count
row_count * record_size   raw struct image (native little-endian, padding included)
```

Confirmed against `sub_140932DC0` (reader) and `sub_140932A80` (writer); see
`../RE_NOTES.md`.

Eight shipped files are serialised **code indexes** rather than tables, and
`sub_140924FF0` writes them straight from an in-memory index object when building
in txt mode (an empty index writes a single `0x00` byte).  Six are the calc pages
(`itemscode`, `moncode`, `misscode`, `tccode`, `skillscode`, `skilldesccode`) and
are **generated** (`kind=3`) from the tables that append to them.  `compcode` is
an ordinary table whose record is one 4-char code, so its bin carries the text.
`actcode` is the odd one out: it is not opaque data but the serialised output of
actinfo's *early* registration (`sub_1409257D0` passes `"actcode"` as that
registration's own bin name to `sub_140933080`), i.e. actinfo's `act` column.  It
is currently carried as a `kind=2` round-tripped entry, which is byte-exact and
also serves as the decompiler's ground truth for the `act` column.

## Field types

`type` is the raw `nType` stored in `ExcelFieldDesc`, mapped in
`sub_1419D9950` and consumed by the parser `sub_1419D6EA0`:

| type | meaning | stored as |
|---|---|---|
| 1,7,16 | text | `nFieldLength` chars (+ string index register for 16) |
| 2,8 | int | int32 |
| 3 | int | int16 |
| 4,5,6 | int | int8 (5 = only when a digit was consumed) |
| 9 | 4-char code | raw, space padded |
| 10,12,14 | 4-char code -> index (intern) | u32 / u8 / u16 |
| 11,13,15 | 4-char code -> index (lookup) | u32 / u8 / u16 |
| 17,18 | text -> string index (intern) | u16 / u32 |
| 19,20 | string index (intern) + raw text | u16+text / u32+text |
| 21,22,23 | text -> string index (lookup) | u32 / u16 / u8 |
| 24,25 | text -> callback | u16 / u32 |
| 26,27,28 | text -> callback(version,text,row,offset,len,r,c) | callback defined |
| 29,30 | boolean / inverse boolean | bit `nFieldLength` of the byte at `offset` |

## Status

See `../VERIFY.md`.  Both shipped sets compile byte-for-byte against the shipped
`.bin` files:

- **base**: 89 byte-exact, 0 pinned, 0 reference-only, 0 schema-gap, 0 missing.
- **parent**: 89 byte-exact, 0 / 0 / 0.

`decompile` round-trips both sets exactly (`decompiled=91 mismatched=0
skipped=4 warns=0`).  The 4 skipped tables are `runessrv`, `cubeserver`,
`customnpcs`, `customgossiptbl` — declared by `D2R.exe` but shipped with neither
a `.bin` nor a `.txt`.

The 3.3 / build 93854 engine is verified separately against
`data_93847/global/excel`: see [Build variants](#build-variants-311-default-and-33--build-93854)
above and `../VERIFY-93854.md`.

### The sound index space: the engine pins `sounds`/`soundenviron` to the parent

The engine resolves a table's folder from a `GameVersion`
(`0: invalid, 1: base, 2: base, 3: parent`, i.e. `data\global\excel` + suffix
`{invalid/, base/, base/, ""}`), but it **pins the two sound tables to the
parent**:

- `sub_140925410` (d2r.exe 3.1.1) / `sub_1401FC290` (D2R_93854 3.3) force
  `version = 3` when the table name is `sounds` or `soundenviron` (checked
  against the static arrays `off_145CC5080` / `off_1423E4240`).
- `sub_140422860` (3.1.1) and `SoundDataTablesInit` → `sub_140288B20` /
  `sub_140289510` (3.3) pass a **hardcoded `3u`** for both.
- `DataTablesInitAll` registers every table once per pass over versions
  `{2 (base), 3 (parent)}`, so in the base pass both sound tables are still
  read from the parent folder.

The live `sounds_sound_index` space is therefore **always the parent list**,
in every pass including the base one.  That is why retail `base/monsounds.bin`
(and blizzless `base/skills.bin`) store *parent* positions, and why
`base/sounds.bin` / `base/soundenviron.bin` in 3.1.1 — files the engine never
loads — are stale offline artifacts built against the base list.

The port implements exactly that boundary:

- `main.cpp` compiles each layer's own `sounds` and `soundenviron` tables
  self-indexed first (those two `.bin` files reproduce byte-for-byte as
  artifacts of the layer that carries them), then resets `sounds_sound_index`
  and reseeds it from the parent's `sounds.txt`
  (`ReseedSoundsIndexParentShaped`) before the first consumer
  (`missiles`, `overlay`, `states`, `armor`, `weapons`, `misc`, `skills`,
  `monstats`, `monsounds`, `uniqueitems`, `setitems` are all registered after
  `soundenviron`).
- `decompiler.cpp` mirrors it: `SeedSoundsIndex` pre-seeds parent-shaped for
  layers that ship no `sounds.bin` of their own, and once a layer's own
  `sounds`/`soundenviron` have generated and round-tripped,
  `ReseedSoundsIndexFromParent` switches the handle to the parent-shaped list
  for the remaining tables.

Consequence: there are **no pinned shipped-data inconsistencies left** in any
verified data set.  The historical `base/monsounds` pin (retail 3.1.1 and 3.3)
and the blizzless `base/skills` pin disappeared because the compiler now
resolves base-layer sound references against the parent list exactly like the
game, so those stale expansion copies reproduce byte-exactly.  The
`verify.py` / `verify_93854.py` pin machinery is kept (with `KNOWN = {}`) for
the next shipped-data inconsistency; a pin only absorbs a difference that
still matches its recorded signature.

The lists themselves diverge at row 6830, where the expansion list carries 6
entries the classic one lacks (`item_worldstone_shard_hd`,
`item_mephistos_brain_hd`, `item_baals_eye_hd`, `item_diablos_horn_hd`,
`desecrated_zone_unique_chance_gained_hd`,
`desecrated_zone_unique_spawned_hd`); every base-layer sound name also exists
in the parent list, so no reference is lost by the switch.

### How the calc expressions are handled

`skills`, `skilldesc`, `missiles`, `weapons`/`armor`/`misc`, `uniqueitems`,
`treasureclassex` and `monpet` store an *offset into a code page* for every type
26/28 field.  `sub_140148481` appends the compiled expression to that page and
returns the size before the append, so the offsets follow from the byte length of
each expression.  `calc_compiler.cpp` is a real BBE expression compiler, so the
bytes come from compiling the source text rather than from a replay table.

The six pages are generated by the compile and serialised under their
ground-truth names, which are also the shipped file names:

| page            | callback             | shipped file     |
| --------------- | -------------------- | ---------------- |
| `skillscode`    | `cb:calc_skills`     | `skillscode.bin`    |
| `skilldesccode` | `cb:calc_skilldesc`  | `skilldesccode.bin` |
| `misscode`      | `cb:calc_missile`    | `misscode.bin`      |
| `itemscode`     | `cb:calc_item`       | `itemscode.bin`     |
| `tccode`        | `cb:calc_condition`  | `tccode.bin`        |
| `moncode`       | `cb:calc_monpet`     | `moncode.bin`       |

All six reproduce byte-for-byte in both sets (including `moncode.bin` at 20 159
bytes and `tccode.bin` at 619).  A page is registered by the loader that produces
it (`sub_140924FF0` writes `<name>.bin` at registration), so its slot in
`kTables` follows its last contributing table — otherwise the file is written
while the page is still empty and serialises as a single `0x00` byte.

Because the pages ship on disk, the decompiler disassembles `treasureclassex`,
`uniqueitems` and `monpet` calc columns straight from `tccode.bin` / `moncode.bin`
instead of needing their `.txt`.

### Code-index columns that are not in the table's own `.bin`

Three tables — `actinfo`, `plrmode`, `monmode` — are registered twice: once by
the `sub_1409257D0` pre-pass, with a single type-10 code field whose only job is
to fill a code index, and once by their real loader, which writes the shipped
`.bin` with a different field list.  The early registration's column is therefore
absent from the shipped record layout; only its index survives.

`sub_140933080` lets an early registration name its own file (`arg5`).  Only
actinfo does — `"actcode"` — so its `act` column ships as `actcode.bin` and is
read straight back.  `plrmode`/`monmode` name nothing, so no shipped file holds
their `code` text; the decompiler emits a deterministic injective placeholder
(`#00`, `#01`, …) for those, which keeps the code index a bijection with the rows
and is all the round trip needs.  See `DECOMPILE_TXT_DEPS.md` for the evidence.
