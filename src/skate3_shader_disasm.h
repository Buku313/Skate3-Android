#pragma once

namespace skate3::shader_disasm {

// If skate3_disasm_ucode_dir is set, disassembles every raw microcode *.bin
// in that directory to <name>.bin.txt. Call once at startup; no-op otherwise.
void RunIfRequested();

}  // namespace skate3::shader_disasm
