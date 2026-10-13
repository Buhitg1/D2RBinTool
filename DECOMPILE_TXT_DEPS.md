# Decompiler source-`.txt` dependencies

`d2datatables decompile <bin_dir> <out_txt_dir>` rebuilds every `.txt` table from  
the shipped `.bin` images and then recompiles each one to prove the round trip is  
byte-exact.

It does **not** need the original `.txt` sources, with one bounded exception:

|                                                      | count  |
| ---------------------------------------------------- | ------ |
| shipped `.bin` needed                                | 89     |
| source `.txt` **required**                           | **9**  |
| source `.txt` **optional** (derived from the `.bin`) | **81** |
| `.txt` shipped in total                              | 90     |

Verified on both shipped sets, using only the 89 `.bin` plus the 9 `.txt` below:

```
data/global/excel          bins=89 txt=9 -> decompiled=91 mismatched=0 skipped=4 warns=0
data/global/excel/base     bins=89 txt=9 -> decompiled=91 mismatched=0 skipped=4 warns=0
```

Every one of the 9 is individually necessary: removing any single one produces at  
least one round-trip mismatch.

The decompiler is not a copy of the source: **the great majority of the generated  
`.txt` files differ from the shipped source** (column order, normalised values,  
regenerated calc text). Only the 9 txt-only provider tables are byte-identical,  
and only because they are passed through verbatim.

> History: this set was 16, then 13 once the calc pages were located on disk  
> (`tccode.bin` / `moncode.bin`), then 12 once `stat()` learned to fall back to  
> the numeric stat id, then 11 once `actcode.bin` was recognised as actinfo's  
> own early-registration file, and finally 9 once the unrecoverable  
> code-index columns of `plrmode` / `monmode` gained a placeholder fallback.

---

## Required (9)

### txt-only provider tables (9)

These ship **no `.bin` at all**. The code/string index they own exists only as  
text, and other tables' index fields point into it, so it cannot be rebuilt from  
any binary.

| table       | file            | index it owns          | consumers (examples)                                                                                              |
| ----------- | --------------- | ---------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `bodylocs`  | `bodylocs.txt`  | `bodylocs_code_index`  | `itemtypes.bodyloc1/2`, `charstats.item*loc`, `monequip.loc1/2`                                                   |
| `colors`    | `colors.txt`    | `colors_code_index`    | `magicprefix`/`magicsuffix`/`automagic.transformcolor`, `uniqueitems`/`setitems.chrtransform`, `states.itemtrans` |
| `ElemTypes` | `elemtypes.txt` | `elemtypes_code_index` | `montype.element`, `monstats.El1Type..`, `missiles.EType`, `skills.EType`                                         |
| `events`    | `events.txt`    | `events_event_index`   | `itemstatcost.itemevent1..`                                                                                       |
| `misscalc`  | `misscalc.txt`  | `misscalc_code_index`  | calc expressions referencing misscalc codes                                                                       |
| `monai`     | `monai.txt`     | `monai_ai_index`       | `monstats.AI`                                                                                                     |
| `MonPlace`  | `monplace.txt`  | `monplace_index`       | `monpreset.Place`                                                                                                 |
| `skillcalc` | `skillcalc.txt` | `skillcalc_code_index` | calc expressions referencing skillcalc codes                                                                      |
| `StorePage` | `storepage.txt` | `storepage_code_index` | store-page references                                                                                             |

These nine are **legitimately text-only reference files**: neither table set ships  
a `.bin` for any of them, and the game itself reads them from `.txt`. They are  
not a decompiler workaround.

> 3.3 / build 93854 adds a **tenth** txt-only provider, `moncalc` (one `code`  
> field, record 4, no `.bin`), registered right after `misscalc`. When compiling  
> or decompiling `data_93847` with the `D2DT_BUILD_93854` engine, add  
> `moncalc.txt` to the required set. See `../RE_NOTES.md` §"FINDINGS  
> 2026-10-12f".

`skillcalc` in particular is **not** recoverable from the calc pages, so it stays  
required:

- No bin contains the code list. Searching all 89 bins for all 127 code strings  
  yields only coincidental substrings inside unrelated text (`mana` inside a sound  
  filename, `rng`/`ast1` inside other codes) — never a 127-entry list.
- `skillscode.bin` / `skilldesccode.bin` store only the compiled bytecode. A  
  skillcalc reference appears there as a **VAL** operand holding the bare index  
  (`04 01` = `VAL8(1)`), never the 4-char code.
- Only 79 of the 127 codes are referenced by any VAL (values 0..122, with 48  
  indices never used), so even the *positions* of the list cannot be recovered.
- Unlike `stat()`, a VAL has **no numeric fallback**: a VAL must be re-emitted as  
  an identifier, because digits lex to NUM — a different operand kind that encodes  
  to different bytes (`calc_compiler.cpp` `Resolve`: a non-FUNC operand resolves  
  through `lookupFamilyCodeIndex()`, and `PackCodeU32` only matches a real code).

---

## The eight `*code.bin` files

Eight shipped files are serialised *code indexes* rather than tables. Seven of  
them are the calc pages (see below); the eighth is actinfo's. All eight are  
utilised — none is dead weight, and each one is what makes the corresponding  
`.txt` optional:

| file                | what it is                                | `.txt` it makes optional                  |
| ------------------- | ----------------------------------------- | ----------------------------------------- |
| `skillscode.bin`    | skills calc page (`cb:calc_skills`)       | `skills.txt`                              |
| `skilldesccode.bin` | skilldesc calc page (`cb:calc_skilldesc`) | `skilldesc.txt`                           |
| `misscode.bin`      | missiles calc page (`cb:calc_missile`)    | `missiles.txt`                            |
| `itemscode.bin`     | item calc page (`cb:calc_item`)           | `armor` / `misc` / `weapons` calc columns |
| `tccode.bin`        | TC calc page (`cb:calc_condition`)        | `treasureclassex.txt`, `uniqueitems.txt`  |
| `moncode.bin`       | monpet calc page (`cb:calc_monpet`)       | `monpet.txt`                              |
| `compcode.bin`      | compcode's own table (record = 1 code)    | `compcode.txt`                            |
| `actcode.bin`       | **actinfo's early-registration output**   | `actinfo.txt`                             |

The first six are **generated** (`kind 3`) from the tables that append to them.  
`compcode` is an ordinary table whose record is a single 4-char code, so the bin  
carries the text. `actcode` is covered in the next section — it is the one that  
used to be a genuine gap.

---

## Code-index columns that are *not* in the table's own `.bin`

This is the only remaining reason a `.txt` can matter, and it applies to exactly  
three tables: `actinfo`, `plrmode`, `monmode`.

`sub_1409257D0` is a pre-pass that runs before the main table loads. It  
re-registers a set of tables with **one field each** — the code field whose index  
other tables consult:

```c
// sub_1409257D0
sub_14018CDDE(out, inst, "actinfo", {act, type 10, offset 0}, "actcode", "actcode");
sub_14018CDDE(out, inst, "MonMode", {code, type 10, offset 0}, "",        "");
sub_14018CDDE(out, inst, "PlrMode", {code, type 10, offset 0}, "",        "");
```

Later, the real loader for each of those tables runs and writes the shipped  
`.bin` with a **different, larger** field list (`actinfo`: 20 fields,  
`plrmode`/`monmode`: `name` @0 + `token` @32). The early registration's own  
column is therefore **absent from the shipped record layout** — it is not  
"hidden" behind the kind-0 field that happens to sit at the same offset, it is  
simply not written at all. Only its *index* side effect survives.

`sub_140933080` decides what file that early registration uses:

```c
if (txt_mode)  read "<name>.txt";
else {                                       // bin-only
  if (arg5[0]) read "<arg5>.bin";            // <- the early registration's OWN bin
  read "<name>.txt";
}
```

`arg5` is empty for `MonMode` and `PlrMode`, but **`"actcode"` for actinfo** — so  
actinfo's code-index pass reads and writes `actcode.bin`, and that file ships. It  
is the serialised `act` column (5 rows x 4 bytes, one raw type-10 code each):  
`1, 2, 3, 4, 5`, exactly matching `actinfo.txt`.

So the three tables split cleanly:

| table     | column    | early-registration bin | recoverable?                           | `.txt` needed  |
| --------- | --------- | ---------------------- | -------------------------------------- | -------------- |
| `actinfo` | `act` @0  | `actcode.bin` (ships)  | **yes** — read back from `actcode.bin` | no             |
| `plrmode` | `code` @0 | none                   | no — no file anywhere holds it         | no (see below) |
| `monmode` | `code` @0 | none                   | no — no file anywhere holds it         | no (see below) |

### Why `plrmode` / `monmode` still work without the `.txt`

The column exists only to fill a code index, and every consumer stores an  
**index** into it (`monequip.anim`/`seqtrans` → `plrmode_code_index`,  
`monstats.Sk1..8mode` and friends → `monmode_code_index`). Reproducing the exact  
index is all the round trip needs, so the index merely has to stay a bijection  
with the rows.

Fabricating the cell from the bytes at the colliding offset cannot do that: it  
reads the neighbouring field and yields NUL bytes (which corrupt the txt) and  
duplicates — `plrmode` rows 18/19 and `monmode` row 13 share the `GH` token, and  
`monmode` rows 5/6 (`attack1`/`attack2`) share the first four name bytes.

The decompiler therefore emits a deterministic placeholder when neither the  
source column nor an early-registration `.bin` exists:

```
#00  #01  #02  ...            '#' + row number in base 36, padded to 2 digits
```

Three characters, not four: `sub_140A905C0` (the `SkNmode` callback) only treats  
a cell of **<= 3 characters** as a mode code and otherwise silently stores 14, so  
a longer placeholder would break `monstats`. Supplying the real `.txt` replaces  
the placeholder with the true codes (`DT`, `NU`, `GH`, `SQ`, `KB`, ...).


This is the same trade-off already accepted for `itemstatcost` (see below), and  
the same convention `IndexColumnText` uses for an unrecoverable type 17/18 name.

---

## Where the calc pages actually live (they *are* shipped)

Earlier revisions of this document claimed `condcalc` / `monpetcalc` are "never  
serialised". **That was wrong**, and it is why `treasureclassex.txt`,  
`uniqueitems.txt` and `monpet.txt` used to be required. The pages are shipped:

| page      | shipped file  | registered by                                                                                                                                                | index object       |
| --------- | ------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------ |
| `tccode`  | `tccode.bin`  | `sub_140A97040`, called from `sub_140A972F0` (the TreasureClassEx loader)                                                                                    | `dataTable+0xD90`  |
| `moncode` | `moncode.bin` | `sub_140A8ED80` (reached from `DataTablesInitAll` at `0x14092444e`), which loads monpet via `sub_140194E08`→`sub_140A9FEB0` and *then* registers `"moncode"` | `dataTable+0x12C0` |

Evidence chain:

- The strings `"tccode"` (`0x145ce6764`) and `"moncode"` (`0x145ce81f8`) exist in  
  `d2r.exe`; `"condcalc"` and `"monpetcalc"` do **not** exist anywhere in the  
  image — those two names were invented by the port and are now retired.
- `sub_1401142A8` is a thunk to `sub_140924FF0`, which builds `"%s.bin"` from the  
  registered name and then either **writes** the index object (txt/build mode,  
  `GetGlobalInitParams()->txt != 0 && ->direct != 0`, via `bcWriteFile`) or  
  **reads it back** (bin-only mode, `sub_140925290`). An *empty* page is first  
  given a single `0x00` byte (`sub_140148481(page, &zero, 1)`) — which is why  
  `base/tccode.bin` and `base/moncode.bin` are exactly 1 byte.
- The stored type-28 value is the page size *before* the append, and it lines up  
  exactly with the shipped page bytes:
  | table             | field               | stored offsets          | `tccode.bin` expression starts  |
  | ----------------- | ------------------- | ----------------------- | ------------------------------- |
  | `uniqueitems`     | `DropConditionCalc` | 0, 9, 18, 27, 36, 45    | 0, 9, 18, 27, 36, 45 (9 B each) |
  | `treasureclassex` | `ConditionCalc`     | 54, 62, 70, 78, 86, 94… | 54, 62, 70, … (8 B each)        |
  and for `monpet` (`consumecalc1` 15, 109, 203…; `numunderlingcalc` 0, 3, 6, 9,  
  12, 30…) against `moncode.bin`.

Consequences now implemented:

1. The port's page names are the ground-truth ones: `tccode` and `moncode`  
   (`CalcPageOf` / `RunParseCallback` / `InvertCallback`).
2. `tccode` and `moncode` are **generated** (`kind 3`), not raw-copied: the  
   forward compile reproduces `tccode.bin` (619 B) and `moncode.bin` (20 159 B)  
   byte-for-byte from `uniqueitems.txt` + `treasureclassex.txt` and `monpet.txt`.
3. Their `kTables` slot must follow the loader that registers them, otherwise the  
   file is written while the page is still empty (see `tools/gen_tables.py`).
4. `Decomp::Page("tccode" / "moncode")` now finds the shipped page, so  
   `treasureclassex.txt`, `uniqueitems.txt` and `monpet.txt` are **optional**.

---

## Optional (81)

Every other shipped `.txt`. The decompiler derives these entirely from the  
`.bin`; supplying them changes nothing except that the real code/stat names are  
used instead of placeholders.

```
actinfo          armor            armtype          automagic        automap
belts            books            charstats        compcode         composit
cubemain         cubemod          difficultylevels experience       gamble
gems             hireling         hitclass         inventory        itemratio
itemstatcost     itemtypes        itemuicategories levelgroups      levels
lowqualityitems  lvlmaze          lvlprest         lvlsub           lvltypes
lvlwarp          magicprefix      magicsuffix      misc             missiles
monequip         monlvl           monmode          monpet           monplace
monpreset        monprop          monseq           monsounds        monstats
monstats2        montype          monumod          npc              objects
objgroup         objmode          objpreset        objtype          overlay
pettype          playerclass      plrmode          plrtype          properties
propertygroups   qualityitems     rareprefix       raresuffix       runes
runeworduicategories              setitems         sets             shrines
skilldesc        skills           soundenviron     sounds           states
storepage        superuniques     treasureclassex  uniqueappellation uniqueitems
uniqueprefix     uniquesuffix     wanderingmon     weapons
```

Notes on the optional list:

- `monstats` is optional even though it carries the `SkNmode` callbacks: those  
  resolve through `monmode_code_index`, which the placeholder fallback keeps  
  bijective.
- `cubemod` and `objtype` are shipped as `.txt` but are **not registered by  
  `D2R.exe` at all** (no matching string exists in the image), so they are never  
  loaded in either direction.
- `levels` is optional: `levels.Name` is type 17/18, so its own names degrade to  
  placeholders, but nothing else in the set resolves them *by name*.
- `itemstatcost` is optional because `stat()` now falls back to the decimal stat  
  id when the name is unrecoverable (see below).
- `treasureclassex`, `uniqueitems`, `monpet` are optional because their calc  
  pages ship as `tccode.bin` / `moncode.bin` (see above).
- `actinfo`, `plrmode`, `monmode` are optional for the reasons in the section  
  above.

### Why `itemstatcost` became optional

`itemstatcost.Stat` is type 17 (index only) — the record stores the numeric id,  
never the name. Calc expressions resolve stats **by name**  
(`stat('hitpoints'.accr)`), so with no `itemstatcost.txt` the seeded name is a  
`itemstatcost_stat_index#6` placeholder. That placeholder is longer than the real  
name and used to re-encode differently, shifting every later page offset in  
`monpet`.

The fix is that `ResolveNumArg` (case `stat`) now refuses a placeholder name and  
emits the decimal id instead:

```
stat('itemstatcost_stat_index#6',accr)   ->   stat(6,accr)
```

Digits lex straight to `NUM`, exactly like a name that resolves to the same id, so  
the operand re-encodes byte-identically. This is the same "decimal fallback is  
always byte-exact" rule the disassembler already relies on elsewhere; the real  
name is still emitted whenever `itemstatcost.txt` is present, so output stays  
readable in the normal case.

---

## How the required set was determined

Each candidate was removed individually from the full 89 `.bin` + candidate-`.txt`  
set and the round trip re-run; every table above produced at least one round-trip  
mismatch when removed. `monstats` did not, so it is not required.

## Why any txt is needed at all

The `.txt` format carries information that the `.bin` format does not:

1. **Index-only fields (types 17/18)** store only an index; the name text is not  
   in the image (this is now survivable for `stat()` thanks to the numeric  
   fallback, but not for the txt-only providers, which have no `.bin` at all).
2. **Early-registration code columns** are not part of the shipped record layout  
   at all; only their index survives.

A `.bin`-only data directory is also not a valid D2R layout — the game itself  
loads the nine txt-only provider tables from `.txt`.

## Reproducing

```sh
# required 9 (the shipped files are lower-case)
REQ="bodylocs colors elemtypes events misscalc monai monplace skillcalc storepage"
mkdir -p d9 && cp data/global/excel/*.bin d9/ && for t in $REQ; do cp data/global/excel/$t.txt d9/; done
d2datatables decompile d9 out_d9
# -> decompiled=91 mismatched=0 skipped=4 warns=0
```
