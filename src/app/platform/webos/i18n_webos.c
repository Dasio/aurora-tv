#include "util/i18n.h"
#include "util/path.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <webosi18n_C.h>

#include "logging.h"

static ResBundleC *bundle = NULL;
static char locale_[16] = "\0";
static char language[4] = "\0";

/* resBundle_getLocString() returns a fresh strdup on every call, so locstr() hands out a cached
 * copy instead, one table per locale. Tables are never freed: the UI keeps these pointers
 * (lv_label_set_text_static, msgbox button maps), and settings re-applies the locale while the
 * launcher is still alive. Memory is bounded by the number of locales loaded. */
typedef struct locstr_entry_t {
    struct locstr_entry_t *next;
    const char *text;
    char msgid[];
} locstr_entry_t;

typedef struct locstr_table_t {
    struct locstr_table_t *next;
    char locale[sizeof(locale_)];
    locstr_entry_t *entries;
} locstr_table_t;

/* Guards bundle and the locstr tables. locstr() is also called from worker threads. */
static SDL_mutex *lock = NULL;
static locstr_table_t *tables = NULL;
static locstr_table_t *table = NULL;

static void i18n_lock(void) {
    static SDL_SpinLock init_lock = 0;
    SDL_AtomicLock(&init_lock);
    if (!lock) {
        lock = SDL_CreateMutex();
    }
    SDL_AtomicUnlock(&init_lock);
    SDL_LockMutex(lock);
}

static void i18n_unlock(void) {
    SDL_UnlockMutex(lock);
}

static locstr_table_t *locstr_table_for(const char *locale) {
    for (locstr_table_t *t = tables; t; t = t->next) {
        if (strcmp(t->locale, locale) == 0) {
            return t;
        }
    }
    locstr_table_t *t = calloc(1, sizeof(locstr_table_t));
    if (!t) {
        return NULL;
    }
    SDL_strlcpy(t->locale, locale, sizeof(t->locale));
    t->next = tables;
    tables = t;
    return t;
}

/* Caller holds lock, and table is set. */
static const char *locstr_lookup(const char *msgid) {
    for (locstr_entry_t *e = table->entries; e; e = e->next) {
        if (strcmp(e->msgid, msgid) == 0) {
            return e->text;
        }
    }
    const char *translated = resBundle_getLocString(bundle, msgid);
    if (!translated) {
        return msgid;
    }
    size_t msgid_size = strlen(msgid) + 1;
    locstr_entry_t *e = malloc(sizeof(locstr_entry_t) + msgid_size);
    if (!e) {
        free((void *) translated);
        return msgid;
    }
    memcpy(e->msgid, msgid, msgid_size);
    e->text = translated;
    e->next = table->entries;
    table->entries = e;
    return translated;
}

/* Convert locale "pt-BR" to path format "pt/BR" to match PackageWebOS. */
static void locale_to_path(const char *locale, char *path, size_t path_size) {
    if (!locale || !path || path_size < 2) {
        return;
    }
    size_t i = 0, j = 0;
    while (locale[i] && j < path_size - 1) {
        path[j++] = (locale[i] == '-') ? '/' : locale[i];
        i++;
    }
    path[j] = '\0';
}

static char *strip_trailing_slash(char *path) {
    if (!path) {
        return path;
    }
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        path[--n] = '\0';
    }
    return path;
}

static bool probe_translated(ResBundleC *b, const char *msgid) {
    char *probe = (char *) resBundle_getLocString(b, msgid);
    bool translated = probe != NULL && probe[0] && strcmp(probe, msgid) != 0;
    free(probe);
    return translated;
}

static bool bundle_is_translated(ResBundleC *b) {
    if (!b) {
        return false;
    }
    return probe_translated(b, "[Localized Language]") || probe_translated(b, "Cancel");
}

static bool try_load_bundle(const char *locale_tag, const char *root) {
    if (!locale_tag || !locale_tag[0] || !root || !root[0]) {
        return false;
    }
    ResBundleC *b = resBundle_createWithRootPath(locale_tag, "cstrings.json", root);
    if (!b) {
        return false;
    }
    if (!bundle_is_translated(b)) {
        commons_log_debug("I18N", "Bundle at %s/%s/cstrings.json is not translated", root, locale_tag);
        resBundle_destroy(b);
        return false;
    }
    if (bundle) {
        resBundle_destroy(bundle);
    }
    bundle = b;
    commons_log_info("I18N", "Loaded '%s' from %s/%s/cstrings.json", locale_tag, root, locale_tag);
    return true;
}

const char *locstr(const char *msgid) {
    i18n_lock();
    const char *text = table ? locstr_lookup(msgid) : msgid;
    i18n_unlock();
    return text;
}

const char *i18n_locale() {
    return locale_;
}

bool i18n_is_loaded(void) {
    return bundle != NULL;
}

static void load_locale(const char *locale) {
    if (bundle) {
        resBundle_destroy(bundle);
        bundle = NULL;
    }
    table = NULL;
    language[0] = '\0';
    locale_[0] = '\0';

    if (!locale || !locale[0] || strcmp(locale, "auto") == 0) {
        return;
    }

    size_t locale_len = strlen(locale) + 1;
    if (locale_len > sizeof(locale_)) {
        locale_len = sizeof(locale_);
    }
    SDL_memcpy(locale_, locale, locale_len);
    locale_[sizeof(locale_) - 1] = '\0';

    char locale_path[16];
    locale_to_path(locale, locale_path, sizeof(locale_path));

    char *base = SDL_GetBasePath();
    strip_trailing_slash(base);
    char *base_parent = NULL;
    if (base) {
        char *slash = strrchr(base, '/');
        if (slash && slash != base) {
            base_parent = strdup(base);
            if (base_parent) {
                strrchr(base_parent, '/')[0] = '\0';
            }
        }
    }
    const char *home = SDL_getenv("HOME");

    const char *roots[4];
    int nroots = 0;
    if (home && home[0]) {
        roots[nroots++] = home;
    }
    if (base_parent && base_parent[0] && (!home || strcmp(base_parent, home) != 0)) {
        roots[nroots++] = base_parent;
    }
    if (base && base[0] && (!home || strcmp(base, home) != 0)) {
        roots[nroots++] = base;
    }

    bool loaded = false;
    for (int i = 0; i < nroots && !loaded; i++) {
        /* Original moonlight-tv: locale "pt-BR" + app HOME (libwebosi18n adds resources/). */
        loaded = try_load_bundle(locale, roots[i]);
        if (!loaded) {
            char *resources = path_join(roots[i], "resources");
            loaded = try_load_bundle(locale, resources);
            if (!loaded) {
                loaded = try_load_bundle(locale_path, resources);
            }
            if (!loaded) {
                loaded = try_load_bundle(locale_path, roots[i]);
            }
            free(resources);
        }
    }
    if (base) {
        SDL_free(base);
    }
    free(base_parent);

    if (!loaded) {
        commons_log_error("I18N", "Failed to load locale '%s'", locale);
        locale_[0] = '\0';
        return;
    }
    table = locstr_table_for(locale_);

    if (SDL_strlen(locale) > 2) {
        SDL_memcpy(language, locale, 2);
        language[2] = '\0';
    }
}

void i18n_setlocale(const char *locale) {
    i18n_lock();
    load_locale(locale);
    i18n_unlock();
}

const char *app_get_locale_lang() {
    if (!language[0]) {
        return "C";
    }
    return language;
}
