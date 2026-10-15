/* NDL's impl is linked to SONAME libSDL2-2.0.so.0. The UI uses sdl2-compat
 * under a private name (libSDL2-webos.so.0) on top of webOS SDL3, which is
 * the library that actually owns the exported video window. This shim occupies
 * the system SONAME and forwards:
 *   - ordinary SDL calls to sdl2-compat (same window the UI created)
 *   - SDL_webOS* export calls to SDL3 (the implementation that talks to the plane)
 * Without it, NDL_DirectMediaLoad fails with "Invalid Load Parameter".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    int x;
    int y;
    int w;
    int h;
} ShimRect;

static void *lib_open(const char *name) {
    void *handle = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
    if (handle == NULL) {
        handle = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
    }
    return handle;
}

static void *compat_lib(void) {
    static void *handle;
    static int tried;
    if (!tried) {
        tried = 1;
        handle = lib_open("libSDL2-webos.so.0");
    }
    return handle;
}

static void *sdl3_lib(void) {
    static void *handle;
    static int tried;
    if (!tried) {
        tried = 1;
        handle = lib_open("libSDL3.so.0");
    }
    return handle;
}

void *SDL_GL_GetCurrentWindow(void) {
    void *(*fn)(void);
    void *lib = compat_lib();
    if (lib == NULL) {
        return NULL;
    }
    fn = dlsym(lib, "SDL_GL_GetCurrentWindow");
    return fn != NULL ? fn() : NULL;
}

const char *SDL_GetHint(const char *name) {
    const char *(*fn)(const char *);
    void *lib = compat_lib();
    if (lib == NULL) {
        return NULL;
    }
    fn = dlsym(lib, "SDL_GetHint");
    return fn != NULL ? fn(name) : NULL;
}

int SDL_SetHint(const char *name, const char *value) {
    int (*fn)(const char *, const char *);
    void *lib = compat_lib();
    if (lib == NULL) {
        return 0;
    }
    fn = dlsym(lib, "SDL_SetHint");
    return fn != NULL ? fn(name, value) : 0;
}

void *SDL_GetWindowFromID(unsigned int id) {
    void *(*fn)(unsigned int);
    void *lib = compat_lib();
    if (lib == NULL) {
        return NULL;
    }
    fn = dlsym(lib, "SDL_GetWindowFromID");
    return fn != NULL ? fn(id) : NULL;
}

void SDL_GetWindowSize(void *window, int *w, int *h) {
    void (*fn)(void *, int *, int *);
    void *lib = compat_lib();
    if (lib == NULL) {
        return;
    }
    fn = dlsym(lib, "SDL_GetWindowSize");
    if (fn != NULL) {
        fn(window, w, h);
    }
}

static char last_window_id[64];
static int last_video_w;
static int last_video_h;

int SDL_webOSSetExportedWindow(const char *window_id, ShimRect *src, ShimRect *dst);

static int set_exported_raw(const char *window_id, ShimRect *src, ShimRect *dst) {
    int (*fn)(const char *, ShimRect *, ShimRect *);
    void *lib = sdl3_lib();
    if (lib == NULL || window_id == NULL || window_id[0] == '\0') {
        return 0;
    }
    fn = dlsym(lib, "SDL_webOSSetExportedWindow");
    return fn != NULL ? fn(window_id, src, dst) : 0;
}

const char *SDL_webOSCreateExportedWindow(int type) {
    const char *(*fn)(int);
    const char *id;
    void *lib = sdl3_lib();
    if (lib == NULL) {
        return NULL;
    }
    fn = dlsym(lib, "SDL_webOSCreateExportedWindow");
    if (fn == NULL) {
        return NULL;
    }
    id = fn(type);
    if (id != NULL && id[0] != '\0') {
        strncpy(last_window_id, id, sizeof(last_window_id) - 1);
        last_window_id[sizeof(last_window_id) - 1] = '\0';
        if (last_video_w > 0 && last_video_h > 0) {
            SDL_webOSSetExportedWindow(last_window_id, NULL, NULL);
        }
    }
    return id;
}

void SDL_webOSDestroyExportedWindow(const char *window_id) {
    void (*fn)(const char *);
    void *lib = sdl3_lib();
    if (lib == NULL) {
        return;
    }
    fn = dlsym(lib, "SDL_webOSDestroyExportedWindow");
    if (fn != NULL) {
        fn(window_id);
    }
    if (window_id != NULL && strcmp(window_id, last_window_id) == 0) {
        last_window_id[0] = '\0';
    }
}

int SDL_webOSSetExportedWindow(const char *window_id, ShimRect *src, ShimRect *dst) {
    ShimRect src_fixed;
    ShimRect dst_fixed;
    int ww = 0;
    int hh = 0;
    void *window;

    if (window_id != NULL && window_id[0] != '\0') {
        strncpy(last_window_id, window_id, sizeof(last_window_id) - 1);
        last_window_id[sizeof(last_window_id) - 1] = '\0';
    }
    /* A 0-size source is what the compositor logged as sourceInput 0x0,
     * which samples no video and leaves the panel black. */
    if (src == NULL || src->w <= 0 || src->h <= 0) {
        if (last_video_w <= 0 || last_video_h <= 0) {
            return 0;
        }
        src_fixed.x = 0;
        src_fixed.y = 0;
        src_fixed.w = last_video_w;
        src_fixed.h = last_video_h;
        src = &src_fixed;
    }
    if (dst == NULL || dst->w <= 0 || dst->h <= 0) {
        window = SDL_GL_GetCurrentWindow();
        if (window != NULL) {
            SDL_GetWindowSize(window, &ww, &hh);
        }
        if (ww <= 0 || hh <= 0) {
            ww = 1920;
            hh = 1080;
        }
        dst_fixed.x = 0;
        dst_fixed.y = 0;
        dst_fixed.w = ww;
        dst_fixed.h = hh;
        dst = &dst_fixed;
    }
    return set_exported_raw(window_id != NULL && window_id[0] != '\0' ? window_id : last_window_id, src, dst);
}

/* Called after NDL_DirectMediaLoad with the stream size. NDL's own
 * setDisplayWindow uses a 0x0 source, so the plane stays black. */
void aurora_ndl_apply_video_rect(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    last_video_w = width;
    last_video_h = height;
    if (last_window_id[0] == '\0') {
        return;
    }
    SDL_webOSSetExportedWindow(last_window_id, NULL, NULL);
}

int SDL_webOSExportedSetProperty(const char *window_id, const char *name, const char *value) {
    int (*fn)(const char *, const char *, const char *);
    void *lib = sdl3_lib();
    if (lib == NULL) {
        return 0;
    }
    fn = dlsym(lib, "SDL_webOSExportedSetProperty");
    return fn != NULL ? fn(window_id, name, value) : 0;
}
