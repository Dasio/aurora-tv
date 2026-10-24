#include "bt_sniff.h"

#include "logging.h"
#include "lunasynccall.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define URI_STOP_SNIFF "luna://com.webos.service.bluetooth2/device/internal/stopSniff"
#define URI_START_SNIFF "luna://com.webos.service.bluetooth2/device/internal/startSniff"

/* The stack re-enters sniff within seconds; a call replies in 1-3 ms. */
#define REASSERT_MS 250
/* Pads attach and leave mid-session. */
#define RESCAN_MS 2000
#define MAX_PADS 4
#define ADDR_LEN 18

struct webos_bt_sniff_state {
    atomic_bool stop;
};

typedef struct {
    char addr[MAX_PADS][ADDR_LEN];
    int count;
} pad_list_t;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static bool list_contains(const pad_list_t *list, const char *addr) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->addr[i], addr) == 0) {
            return true;
        }
    }
    return false;
}

/* One /proc/bus/input/devices record (blank-line separated). A pad is a node with a joystick
 * handler (jsN), which also skips its touchpad/motion nodes sharing the address. Bus=0005 is
 * Bluetooth: a wired pad publishes its MAC in Uniq too. */
static void consider_record(pad_list_t *out, bool bluetooth, bool joystick, const char *uniq) {
    if (!bluetooth || !joystick || uniq[0] == '\0' || out->count >= MAX_PADS || list_contains(out, uniq)) {
        return;
    }
    snprintf(out->addr[out->count++], ADDR_LEN, "%s", uniq);
}

static void scan_pads(pad_list_t *out) {
    out->count = 0;
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (f == NULL) {
        return;
    }
    char line[512];
    bool bluetooth = false, joystick = false;
    char uniq[ADDR_LEN] = {0};
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '\n') {
            consider_record(out, bluetooth, joystick, uniq);
            bluetooth = joystick = false;
            uniq[0] = '\0';
        } else if (strncmp(line, "I: Bus=0005", 11) == 0) {
            bluetooth = true;
        } else if (strncmp(line, "U: Uniq=", 8) == 0) {
            size_t n = strcspn(line + 8, " \r\n");
            if (n >= ADDR_LEN) {
                n = ADDR_LEN - 1;
            }
            memcpy(uniq, line + 8, n);
            uniq[n] = '\0';
            for (size_t i = 0; i < n; i++) {
                if (uniq[i] >= 'A' && uniq[i] <= 'Z') {
                    uniq[i] = (char) (uniq[i] - 'A' + 'a');
                }
            }
        } else if (strncmp(line, "H: Handlers=", 12) == 0) {
            joystick = strstr(line, " js") != NULL || strstr(line, "=js") != NULL;
        }
    }
    consider_record(out, bluetooth, joystick, uniq);
    fclose(f);
}

static bool lists_equal(const pad_list_t *a, const pad_list_t *b) {
    if (a->count != b->count) {
        return false;
    }
    for (int i = 0; i < a->count; i++) {
        if (!list_contains(b, a->addr[i])) {
            return false;
        }
    }
    return true;
}

static void *keeper_proc(void *arg) {
    webos_bt_sniff_state_t *state = arg;
    pad_list_t pads = {.count = 0};
    uint64_t scanned = 0, asserted = 0;
    bool failed_logged = false;
    while (!atomic_load(&state->stop)) {
        uint64_t now = now_ms();
        if (scanned == 0 || now - scanned >= RESCAN_MS) {
            scanned = now;
            pad_list_t found;
            scan_pads(&found);
            if (!lists_equal(&found, &pads)) {
                pads = found;
                asserted = 0;
                for (int i = 0; i < pads.count; i++) {
                    commons_log_info("BtSniff", "Holding %s out of Bluetooth sniff", pads.addr[i]);
                }
            }
        }
        if (pads.count > 0 && (asserted == 0 || now - asserted >= REASSERT_MS)) {
            asserted = now;
            for (int i = 0; i < pads.count; i++) {
                char payload[64];
                snprintf(payload, sizeof(payload), "{\"address\":\"%s\"}", pads.addr[i]);
                if (!HLunaServiceCallSync(URI_STOP_SNIFF, payload, true, NULL) && !failed_logged) {
                    failed_logged = true;
                    commons_log_warn("BtSniff", "stopSniff call failed for %s", pads.addr[i]);
                }
            }
        }
        usleep(pads.count > 0 ? 20000 : 250000);
    }
    /* Give the links back to the TV's own ~77 ms policy (96-124 slots of 0.625 ms); a bare
     * address is refused with a schema error. */
    for (int i = 0; i < pads.count; i++) {
        char payload[160];
        snprintf(payload, sizeof(payload),
                 "{\"address\":\"%s\",\"minInterval\":96,\"maxInterval\":124,\"attempt\":4,\"timeout\":1}",
                 pads.addr[i]);
        HLunaServiceCallSync(URI_START_SNIFF, payload, true, NULL);
    }
    free(state);
    return NULL;
}

webos_bt_sniff_state_t *webos_bt_sniff_enter(void) {
    webos_bt_sniff_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    atomic_init(&state->stop, false);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* Detached: a Luna reply that never comes must not hang session teardown. */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&thread, &attr, keeper_proc, state);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        commons_log_warn("BtSniff", "keeper thread did not start (%d)", rc);
        free(state);
        return NULL;
    }
    return state;
}

void webos_bt_sniff_leave(webos_bt_sniff_state_t *state) {
    if (state != NULL) {
        atomic_store(&state->stop, true);
    }
}
