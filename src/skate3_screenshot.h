#pragma once

namespace skate3::screenshot {

// Captures the game window (F6) and writes a timestamped PNG to
// screenshots/ under the working directory (the install dir), logging the
// absolute path. Uses PrintWindow PW_RENDERFULLCONTENT, the flip-model
// swapchain reads black through a plain GDI BitBlt, and crops to the client
// area. The capture runs on the calling (UI) thread; the PNG encode runs on
// a detached worker so the frame hitch stays minimal. Windows only; a no-op
// with a warning elsewhere. hwnd is the platform-native window handle.
void CaptureWindow(void* hwnd);

}  // namespace skate3::screenshot
