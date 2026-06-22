package dev.pp_browser.app;

import org.libsdl.app.SDLActivity;

/**
 * SDL3 and other native deps are statically linked into libmain.so (see root CMakeLists.txt).
 */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
