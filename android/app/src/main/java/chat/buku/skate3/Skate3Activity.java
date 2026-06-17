package chat.buku.skate3;

import org.libsdl.app.SDLActivity;

/**
 * Skate 3 (native aarch64 recompilation) Android entry activity.
 *
 * The game is split across native shared libraries:
 *   - libc++_shared.so : shared C++ runtime (one copy across all libs)
 *   - librexruntime.so : the rexglue Xbox 360 recomp runtime; statically links
 *                        SDL3 and therefore hosts the SDL Android JNI bridge
 *                        (Java_org_libsdl_app_*, JNI_OnLoad).
 *   - libskate3.so     : the recompiled game; exports SDL_main (the entry point).
 *
 * Load order matters: dependencies first, the SDL_main-carrying lib last (SDL
 * derives the main shared object from the final entry in getLibraries()).
 */
public class Skate3Activity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "rexruntime",
            "skate3",
        };
    }
}
