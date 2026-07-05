// Dev tool: offline Xenos ucode disassembly through the SDK's shader
// analyzer. When skate3_disasm_ucode_dir is set, every *.bin in that
// directory is treated as raw big-endian microcode (filename prefix vs_/ps_
// selects the shader type) and a .txt disassembly is written next to it at
// startup. Used to decode captured guest shaders (e.g. the spline/beam
// renderer) when porting draw paths to the native renderer.

#include "skate3_shader_disasm.h"

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>

REXCVAR_DEFINE_STRING(skate3_disasm_ucode_dir, "", "Skate 3",
                      "Dev: disassemble every *.bin (raw big-endian Xenos microcode, "
                      "vs_/ps_ filename prefix = shader type) in this directory at "
                      "startup, writing <name>.txt beside each file")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace skate3::shader_disasm {

void RunIfRequested() {
  const std::string dir_str(REXCVAR_GET(skate3_disasm_ucode_dir));
  if (dir_str.empty()) {
    return;
  }
  const std::filesystem::path dir(dir_str);
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    REXLOG_WARN("shader-disasm: {} is not a directory", dir_str);
    return;
  }
  rex::string::StringBuffer disasm_buffer;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() != ".bin") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const bool pixel = name.rfind("ps_", 0) == 0;
    std::ifstream in(entry.path(), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < 8 || (bytes.size() % 4) != 0) {
      REXLOG_WARN("shader-disasm: {} skipped (size {})", name, bytes.size());
      continue;
    }
    rex::graphics::Shader shader(
        pixel ? rex::graphics::xenos::ShaderType::kPixel
              : rex::graphics::xenos::ShaderType::kVertex,
        0, reinterpret_cast<const uint32_t*>(bytes.data()), bytes.size() / 4,
        std::endian::big);
    shader.AnalyzeUcode(disasm_buffer);
    std::filesystem::path out_path = entry.path();
    out_path += ".txt";
    std::ofstream out(out_path);
    out << shader.ucode_disassembly();
    REXLOG_INFO("shader-disasm: {} -> {} ({} bytes of disassembly)", name,
                out_path.filename().string(), shader.ucode_disassembly().size());
  }
}

}  // namespace skate3::shader_disasm
