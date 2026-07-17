// Application icon from the user's own game files, see skate3_win_icon.h.

#include "skate3_win_icon.h"

#if defined(_WIN32)

#include <windows.h>
// initguid must precede wincodec so the WIC CLSIDs/IIDs are defined in this
// translation unit (no dependency on uuid.lib link order).
#include <initguid.h>
#include <wincodec.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include <rex/logging.h>

namespace skate3 {
namespace {

// First embedded PNG in the nxeart dashboard-art package = the 64x64 title
// tile (two identical copies exist; either is fine).
std::vector<uint8_t> ExtractFirstPng(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return {};
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  static const uint8_t kMagic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  static const uint8_t kIend[4] = {'I', 'E', 'N', 'D'};
  for (size_t i = 0; i + 8 <= data.size(); ++i) {
    if (std::memcmp(data.data() + i, kMagic, 8) != 0) {
      continue;
    }
    for (size_t j = i + 8; j + 8 <= data.size(); ++j) {
      if (std::memcmp(data.data() + j, kIend, 4) == 0) {
        // IEND chunk data end + 4-byte CRC.
        return std::vector<uint8_t>(data.begin() + i, data.begin() + j + 8);
      }
    }
    break;
  }
  return {};
}

// Decode the PNG and scale to `size` x `size` 32bpp BGRA (top-down rows).
bool WicDecodeScaled(IWICImagingFactory* wic, const std::vector<uint8_t>& png,
                     uint32_t size, std::vector<uint8_t>& bgra_out) {
  IWICStream* stream = nullptr;
  if (FAILED(wic->CreateStream(&stream))) {
    return false;
  }
  bool ok = false;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  IWICFormatConverter* converter = nullptr;
  do {
    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(png.data()), DWORD(png.size())))) {
      break;
    }
    if (FAILED(wic->CreateDecoderFromStream(stream, nullptr,
                                            WICDecodeMetadataCacheOnDemand,
                                            &decoder))) {
      break;
    }
    if (FAILED(decoder->GetFrame(0, &frame))) {
      break;
    }
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame, size, size,
                                  WICBitmapInterpolationModeFant))) {
      break;
    }
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
      break;
    }
    bgra_out.resize(size_t(size) * size * 4);
    if (FAILED(converter->CopyPixels(nullptr, size * 4,
                                     UINT(bgra_out.size()), bgra_out.data()))) {
      break;
    }
    ok = true;
  } while (false);
  if (converter) converter->Release();
  if (scaler) scaler->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (stream) stream->Release();
  return ok;
}

// Icon-resource DIB image: BITMAPINFOHEADER with DOUBLED height, XOR (BGRA)
// rows bottom-up, then an all-zero AND mask (alpha carries transparency).
std::vector<uint8_t> BuildIconDib(uint32_t size,
                                  const std::vector<uint8_t>& bgra_topdown) {
  const uint32_t xor_bytes = size * size * 4;
  const uint32_t and_stride = ((size + 31) / 32) * 4;
  const uint32_t and_bytes = and_stride * size;
  std::vector<uint8_t> out(sizeof(BITMAPINFOHEADER) + xor_bytes + and_bytes, 0);
  auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(out.data());
  bih->biSize = sizeof(BITMAPINFOHEADER);
  bih->biWidth = LONG(size);
  bih->biHeight = LONG(size) * 2;  // XOR + AND, both bottom-up
  bih->biPlanes = 1;
  bih->biBitCount = 32;
  bih->biCompression = BI_RGB;
  bih->biSizeImage = xor_bytes + and_bytes;
  uint8_t* xor_dst = out.data() + sizeof(BITMAPINFOHEADER);
  for (uint32_t y = 0; y < size; ++y) {
    std::memcpy(xor_dst + size_t(y) * size * 4,
                bgra_topdown.data() + size_t(size - 1 - y) * size * 4,
                size_t(size) * 4);
  }
  return out;
}

#pragma pack(push, 2)
struct GrpIconDirEntry {
  BYTE bWidth;
  BYTE bHeight;
  BYTE bColorCount;
  BYTE bReserved;
  WORD wPlanes;
  WORD wBitCount;
  DWORD dwBytesInRes;
  WORD nId;
};
struct GrpIconDir {
  WORD idReserved;
  WORD idType;
  WORD idCount;
};
#pragma pack(pop)

// The renamed-aside original exe (still the locked running image) cannot be
// deleted from this process. Hand it to a detached waiter that deletes it
// the moment this process exits (clean exit or crash), so a freshly
// deployed exe does not leave a .pre-icon copy sitting next to it between
// runs. The next-run cleanup in EnsureExeIconResource stays as the backstop
// if the helper never gets to run.
void SpawnPreIconDeleter(const std::filesystem::path& pre) {
  // PowerShell single-quoted literal: double any embedded single quotes.
  std::wstring esc;
  for (wchar_t c : pre.wstring()) {
    esc += c;
    if (c == L'\'') {
      esc += c;
    }
  }
  std::wstring cmd =
      L"powershell.exe -NoProfile -WindowStyle Hidden -Command "
      L"\"Wait-Process -Id " +
      std::to_wstring(GetCurrentProcessId()) +
      L" -ErrorAction SilentlyContinue; Remove-Item -LiteralPath '" + esc +
      L"' -Force -ErrorAction SilentlyContinue\"";
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

// One-time self-patch: write RT_ICON entries + a MAINICON RT_GROUP_ICON into
// the exe file. The running image is write-locked but RENAMABLE: patch a
// copy, then swap it into place. The renamed original stays locked until
// exit; a detached helper deletes it at process exit (next run = backstop).
void EnsureExeIconResource(const std::vector<uint8_t>& png) {
  wchar_t exe_w[MAX_PATH];
  if (GetModuleFileNameW(nullptr, exe_w, MAX_PATH) == 0) {
    return;
  }
  const std::filesystem::path exe(exe_w);
  const std::filesystem::path pre_local = exe.wstring() + L".pre-icon";
  wchar_t temp_dir[MAX_PATH];
  const bool have_temp = GetTempPathW(MAX_PATH, temp_dir) != 0;
  if (FindResourceW(GetModuleHandleW(nullptr), L"MAINICON",
                    reinterpret_cast<LPCWSTR>(RT_GROUP_ICON)) != nullptr) {
    // Already patched (or a build that ships an icon): backstop-sweep any
    // leftover pre-patch original the exit-time deleter didn't get to:
    // the exe-adjacent legacy name and the temp-dir parking spots (a
    // still-running other instance's parked file just fails the delete).
    DeleteFileW(pre_local.c_str());
    if (have_temp) {
      WIN32_FIND_DATAW fd;
      const std::wstring pattern = std::wstring(temp_dir) + L"skate3-pre-icon-*.old";
      HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
      if (find != INVALID_HANDLE_VALUE) {
        do {
          DeleteFileW((std::wstring(temp_dir) + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
      }
    }
    return;
  }

  // Build the multi-size images. WIC failure falls back to a single raw-PNG
  // entry (valid icon image format since Vista).
  struct IconImage {
    uint32_t size;
    std::vector<uint8_t> data;
  };
  std::vector<IconImage> images;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    IWICImagingFactory* wic = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wic)))) {
      for (uint32_t s : {16u, 24u, 32u, 48u, 64u, 256u}) {
        std::vector<uint8_t> bgra;
        if (WicDecodeScaled(wic, png, s, bgra)) {
          images.push_back({s, BuildIconDib(s, bgra)});
        }
      }
      wic->Release();
    }
  }
  if (SUCCEEDED(co)) {
    CoUninitialize();
  }
  if (images.empty()) {
    images.push_back({64, png});
  }

  GrpIconDir dir{0, 1, WORD(images.size())};
  std::vector<uint8_t> group(sizeof(GrpIconDir) +
                             images.size() * sizeof(GrpIconDirEntry));
  std::memcpy(group.data(), &dir, sizeof(dir));
  for (size_t i = 0; i < images.size(); ++i) {
    GrpIconDirEntry e{};
    e.bWidth = BYTE(images[i].size >= 256 ? 0 : images[i].size);
    e.bHeight = e.bWidth;
    e.wPlanes = 1;
    e.wBitCount = 32;
    e.dwBytesInRes = DWORD(images[i].data.size());
    e.nId = WORD(i + 1);
    std::memcpy(group.data() + sizeof(GrpIconDir) + i * sizeof(e), &e,
                sizeof(e));
  }

  // Patch a COPY first so a failure changes nothing.
  const std::filesystem::path temp = exe.wstring() + L".icon-new";
  if (!CopyFileW(exe.c_str(), temp.c_str(), FALSE)) {
    REXLOG_INFO("win-icon: exe copy for icon patch failed ({}); skipping",
                GetLastError());
    return;
  }
  const WORD lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
  HANDLE update = BeginUpdateResourceW(temp.c_str(), FALSE);
  bool ok = update != nullptr;
  for (size_t i = 0; ok && i < images.size(); ++i) {
    ok = UpdateResourceW(update, reinterpret_cast<LPCWSTR>(RT_ICON),
                         MAKEINTRESOURCEW(WORD(i + 1)), lang,
                         images[i].data.data(), DWORD(images[i].data.size()));
  }
  if (ok) {
    ok = UpdateResourceW(update, reinterpret_cast<LPCWSTR>(RT_GROUP_ICON),
                         L"MAINICON", lang, group.data(), DWORD(group.size()));
  }
  if (update != nullptr) {
    // Commit (discard on failure).
    ok = EndUpdateResourceW(update, !ok) && ok;
  }
  if (!ok) {
    REXLOG_INFO("win-icon: icon resource patch failed ({}); skipping",
                GetLastError());
    DeleteFileW(temp.c_str());
    return;
  }
  // Swap: rename the (locked but renamable) running exe aside, move the
  // patched copy into place. On the second step failing, restore. Parking
  // spot: %TEMP% (a locked image can be renamed anywhere on the same
  // volume, metadata-only op), so no duplicate ever shows up next to the
  // exe; falls back to exe-adjacent when temp is on another volume.
  std::filesystem::path pre =
      have_temp ? std::filesystem::path(std::wstring(temp_dir) +
                                        L"skate3-pre-icon-" +
                                        std::to_wstring(GetCurrentProcessId()) +
                                        L".old")
                : pre_local;
  if (!MoveFileExW(exe.c_str(), pre.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    pre = pre_local;
    if (!MoveFileExW(exe.c_str(), pre.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      REXLOG_INFO("win-icon: could not move running exe aside ({}); skipping",
                  GetLastError());
      DeleteFileW(temp.c_str());
      return;
    }
  }
  if (!MoveFileW(temp.c_str(), exe.c_str())) {
    MoveFileW(pre.c_str(), exe.c_str());
    return;
  }
  SetFileAttributesW(pre.c_str(), FILE_ATTRIBUTE_HIDDEN);
  SpawnPreIconDeleter(pre);
  REXLOG_INFO(
      "win-icon: Explorer icon patched into {} from the game's own art "
      "(one-time; {} removed when this process exits)",
      exe.filename().string(), pre.filename().string());
}

}  // namespace

void ApplyGameIconFromGameData(const std::filesystem::path& game_data_root,
                               void* native_window_handle) {
  std::filesystem::path nxeart = game_data_root / "nxeart";
  std::error_code ec;
  if (!std::filesystem::exists(nxeart, ec)) {
    nxeart = game_data_root / "game" / "nxeart";
  }
  HWND hwnd = static_cast<HWND>(native_window_handle);
  std::thread([nxeart, hwnd] {
    const std::vector<uint8_t> png = ExtractFirstPng(nxeart);
    if (png.empty()) {
      REXLOG_INFO("win-icon: no title icon found in {}", nxeart.string());
      return;
    }
    if (hwnd != nullptr) {
      // CreateIconFromResourceEx accepts PNG image data (Vista+). Explicit
      // small size for crisp titlebar/taskbar small renditions.
      HICON icon_big = CreateIconFromResourceEx(
          const_cast<PBYTE>(png.data()), DWORD(png.size()), TRUE, 0x00030000,
          0, 0, LR_DEFAULTCOLOR);
      HICON icon_small = CreateIconFromResourceEx(
          const_cast<PBYTE>(png.data()), DWORD(png.size()), TRUE, 0x00030000,
          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
          LR_DEFAULTCOLOR);
      DWORD_PTR unused = 0;
      if (icon_big) {
        SendMessageTimeoutW(hwnd, WM_SETICON, ICON_BIG,
                            reinterpret_cast<LPARAM>(icon_big), SMTO_NORMAL,
                            2000, &unused);
      }
      if (icon_small) {
        SendMessageTimeoutW(hwnd, WM_SETICON, ICON_SMALL,
                            reinterpret_cast<LPARAM>(icon_small), SMTO_NORMAL,
                            2000, &unused);
      }
      // HICONs stay alive for the window's lifetime (never destroyed:
      // process-lifetime singletons).
    }
    EnsureExeIconResource(png);
  }).detach();
}

}  // namespace skate3

#else  // !_WIN32

namespace skate3 {
void ApplyGameIconFromGameData(const std::filesystem::path&, void*) {}
}  // namespace skate3

#endif
