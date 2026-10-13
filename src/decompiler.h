// d2datatables decompiler - inverse of the verified forward compile path.
//
// There is no txt writer in D2R.exe (sub_1401162EC is only a docs generator),
// so this is our own inverse of the ported engine: for every table it emits a
// txt whose cells, compiled in the exact same pipeline order with the same
// engine index state, reproduce the shipped .bin byte-for-byte.
#pragma once
#include <string>

namespace d2dt {

class Engine;

// Decompile every table in <in_dir> into <out_dir>.  Returns the number of
// tables whose regenerated bin differed from the shipped image (0 == all
// round-trip exact).  `only` restricts the table loop (seeds still run).
int RunDecompile(Engine& eng, const std::string& in_dir,
                 const std::string& out_dir, const std::string& only);

}  // namespace d2dt
