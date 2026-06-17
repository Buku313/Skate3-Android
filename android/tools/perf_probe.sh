#!/usr/bin/env bash
# Skate 3 on-device perf probe. Two modes:
#   perf_probe.sh on    -> enable stats cvars in the device config + restart app
#   perf_probe.sh dump  -> pull the latest stats from the running app's log
# Workflow: run "on", get into a 3D scene, skate ~30s, then run "dump".
set -euo pipefail
PKG=chat.buku.skate3.dev
export ANDROID_SERIAL="${ANDROID_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}"

case "${1:-dump}" in
  on)
    cat > /tmp/skate3_probe.toml <<'EOF'
draw_resolution_scale_x = 1
draw_resolution_scale_y = 1
resolution_scale = 1
native_2x_msaa = false
vsync = false
store_shaders = true
async_shader_compilation = false
vulkan_async_skip_incomplete_frames = false
log_level = "info"
vulkan_present_timing_log = true
vulkan_present_timing_interval = 60
vulkan_resolve_stats = true
vulkan_frame_stats = true
EOF
    adb push /tmp/skate3_probe.toml /data/local/tmp/skate3.toml >/dev/null
    adb shell run-as "$PKG" cp /data/local/tmp/skate3.toml files/skate3.toml
    adb shell am force-stop "$PKG"
    adb shell run-as "$PKG" sh -c 'rm -f files/logs/*.log'
    adb shell am start -n "$PKG/chat.buku.skate3.Skate3Activity" >/dev/null
    echo ">> stats ON. Get into a 3D scene, skate ~30s, then: $0 dump"
    ;;
  dump)
    NEW=$(adb shell run-as "$PKG" ls -t files/logs | head -1 | tr -d '\r')
    echo "=== frame time (us) ==="
    adb shell run-as "$PKG" cat "files/logs/$NEW" | grep 'timing: frames' | tail -4
    echo "=== render-pass breaks / frame (TBDR cost) ==="
    adb shell run-as "$PKG" cat "files/logs/$NEW" | grep 'Frame stats' | tail -4
    echo "=== resolve direct vs fallback ==="
    adb shell run-as "$PKG" cat "files/logs/$NEW" | grep 'Resolve stats' | tail -4
    echo "=== tile-local EDRAM extension availability (big-win viability) ==="
    adb shell run-as "$PKG" cat "files/logs/$NEW" | grep 'Tile-local EDRAM' | tail -1
    echo "=== GPU device / features ==="
    adb shell run-as "$PKG" cat "files/logs/$NEW" | grep -iE "Vulkan device 'Mali|vertexPipeline|fillModeNonSolid" | tail -4
    ;;
  *) echo "usage: $0 {on|dump}"; exit 2 ;;
esac
