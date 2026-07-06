#pragma once

namespace skate3::screenshot {

// Captures the game window (F6) and writes a timestamped PNG to
// screenshots/ under the working directory (the install dir), logging the
// absolute path. Uses PrintWindow PW_RENDERFULLCONTENT, the flip-model
// swapchain reads black through a plain GDI BitBlt, and crops to the client
// area. The capture runs on the calling thread (PrintWindow SendMessages to
// the UI thread, so any thread works); the PNG encode runs on a detached
// worker so the frame hitch stays minimal. Windows only; a no-op with a
// warning elsewhere. hwnd is the platform-native window handle. tag, when
// given, is appended to the filename (shot_<ts>_<tag>.png); the paired
// native/emulated A/B captures share one timestamp.
void CaptureWindow(void* hwnd, const char* tag = nullptr);

// The game window handle, stashed at dialog setup so frame-loop code (the
// F11 A/B parity capture) can screenshot without an app pointer.
void RememberWindow(void* hwnd);
void* RememberedWindow();

}  // namespace skate3::screenshot
