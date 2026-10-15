#include <dlfcn.h>
#include <stddef.h>
#include <stdbool.h>

static void *sdl3_handle(void) {
    static void *handle;
    static int tried;
    if (!tried) {
        tried = 1;
        /* Prefer the copy sdl2-compat already loaded. */
        handle = dlopen("libSDL3.so.0", RTLD_LAZY | RTLD_NOLOAD);
        if (handle == NULL) {
            handle = dlopen("libSDL3.so.0", RTLD_LAZY);
        }
    }
    return handle;
}

bool SDL_webOSGetPanelResolution(int *width, int *height) {
    bool (*fn)(int *, int *);
    void *lib = sdl3_handle();
    if (lib == NULL) {
        return false;
    }
    fn = dlsym(lib, "SDL_webOSGetPanelResolution");
    if (fn == NULL) {
        return false;
    }
    return fn(width, height);
}

bool SDL_webOSGetRefreshRate(int *rate) {
    bool (*fn)(int *);
    void *lib = sdl3_handle();
    if (lib == NULL) {
        return false;
    }
    fn = dlsym(lib, "SDL_webOSGetRefreshRate");
    if (fn == NULL) {
        return false;
    }
    return fn(rate);
}
