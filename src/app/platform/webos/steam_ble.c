#include "steam_ble.h"

#include "logging.h"
#include "lunasynccall.h"

#include <pbnjson.h>

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

#define URI_BT_STATUS "luna://com.webos.service.bluetooth2/device/getStatus"
#define URI_GATT_CONNECT "luna://com.webos.service.blegatt/client/connect"
#define URI_GATT_REQUEST_MTU "luna://com.webos.service.blegatt/client/requestMtu"
#define URI_GATT_DISCONNECT "luna://com.webos.service.blegatt/client/disconnect"

/* The controller answers 247 with 67, and input starts right away. */
#define REQUEST_MTU 247
#define SCAN_MS 3000
/* A controller that refuses the exchange is not asked again on every scan. */
#define MAX_ATTEMPTS 3
#define MAX_PADS 4
#define ADDR_LEN 18
/* Valve's USB vendor id, which its HID over GATT devices report too. */
#define VALVE_VENDOR "Vendor=28de"

typedef struct {
    char addr[ADDR_LEN];
    /* client/connect succeeded, so client/disconnect is owed. */
    bool gatt_connected;
    bool mtu_done;
    int attempts;
} steam_pad_t;

struct webos_steam_ble_state {
    atomic_bool stop;
    atomic_bool finished;
    pthread_t thread;
    steam_pad_t pads[MAX_PADS];
    int count;
};

typedef struct {
    char addr[MAX_PADS][ADDR_LEN];
    int count;
} addr_list_t;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static void addr_normalize(char *dst, const char *src, size_t n) {
    if (n >= ADDR_LEN) {
        n = ADDR_LEN - 1;
    }
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        dst[i] = (char) (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    dst[n] = '\0';
}

static bool list_contains(const addr_list_t *list, const char *addr) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->addr[i], addr) == 0) {
            return true;
        }
    }
    return false;
}

static void list_add(addr_list_t *list, const char *addr, size_t len) {
    char normalized[ADDR_LEN];
    addr_normalize(normalized, addr, len);
    if (normalized[0] == '\0' || list->count >= MAX_PADS || list_contains(list, normalized)) {
        return;
    }
    memcpy(list->addr[list->count++], normalized, ADDR_LEN);
}

static jvalue_ref parse_json(const char *json, jdomparser_ref *parser) {
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    *parser = jdomparser_create(&schema, 0);
    if (*parser == NULL) {
        return NULL;
    }
    if (!jdomparser_feed(*parser, json, (int) strlen(json)) || !jdomparser_end(*parser)) {
        jdomparser_release(parser);
        return NULL;
    }
    return jdomparser_get_result(*parser);
}

static bool raw_contains_steam(raw_buffer buf) {
    static const char needle[] = "steam";
    const size_t needle_len = sizeof(needle) - 1;
    for (long i = 0; buf.m_str != NULL && i + (long) needle_len <= buf.m_len; i++) {
        if (strncasecmp(buf.m_str + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

/* Paired devices the Bluetooth service reports as connected and named like a Steam Controller. */
static void scan_bt_status(addr_list_t *out) {
    char *reply = NULL;
    if (!HLunaServiceCallSync(URI_BT_STATUS, "{}", true, &reply) || reply == NULL) {
        free(reply);
        return;
    }
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_json(reply, &parser);
    free(reply);
    if (root == NULL) {
        return;
    }
    jvalue_ref devices = jobject_get(root, J_CSTR_TO_BUF("devices"));
    for (ssize_t i = 0; jis_array(devices) && i < jarray_size(devices); i++) {
        jvalue_ref device = jarray_get(devices, i);
        jvalue_ref name = jobject_get(device, J_CSTR_TO_BUF("name"));
        jvalue_ref address = jobject_get(device, J_CSTR_TO_BUF("address"));
        jvalue_ref profiles = jobject_get(device, J_CSTR_TO_BUF("connectedProfiles"));
        if (!jis_string(name) || !jis_string(address) || !jis_array(profiles) || jarray_size(profiles) == 0) {
            continue;
        }
        if (raw_contains_steam(jstring_get_fast(name))) {
            raw_buffer addr = jstring_get_fast(address);
            list_add(out, addr.m_str, (size_t) addr.m_len);
        }
    }
    jdomparser_release(&parser);
}

/* Bluetooth (Bus=0005) input nodes from Valve, in case the service names the controller differently. */
static void scan_input_devices(addr_list_t *out) {
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (f == NULL) {
        return;
    }
    char line[512];
    bool valve_bt = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '\n') {
            valve_bt = false;
        } else if (strncmp(line, "I: Bus=0005", 11) == 0) {
            valve_bt = strstr(line, VALVE_VENDOR) != NULL;
        } else if (valve_bt && strncmp(line, "U: Uniq=", 8) == 0) {
            list_add(out, line + 8, strcspn(line + 8, " \r\n"));
        }
    }
    fclose(f);
}

/* Calls a blegatt method. On failure, errorText goes to error (may be empty); mtuSize to mtu if given. */
static bool gatt_call(const char *uri, const char *payload, char *error, size_t error_len, int *mtu) {
    char *reply = NULL;
    if (error_len > 0) {
        error[0] = '\0';
    }
    if (!HLunaServiceCallSync(uri, payload, true, &reply) || reply == NULL) {
        free(reply);
        snprintf(error, error_len, "no reply");
        return false;
    }
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_json(reply, &parser);
    if (root == NULL) {
        snprintf(error, error_len, "%.*s", (int) (error_len - 1), reply);
        free(reply);
        return false;
    }
    free(reply);
    bool ok = false;
    jvalue_ref rv = jobject_get(root, J_CSTR_TO_BUF("returnValue"));
    if (jis_boolean(rv)) {
        jboolean_get(rv, &ok);
    }
    jvalue_ref text = jobject_get(root, J_CSTR_TO_BUF("errorText"));
    if (!ok && jis_string(text)) {
        raw_buffer buf = jstring_get_fast(text);
        snprintf(error, error_len, "%.*s", (int) buf.m_len, buf.m_str);
    }
    jvalue_ref mtu_value = jobject_get(root, J_CSTR_TO_BUF("mtuSize"));
    if (mtu != NULL && jis_number(mtu_value)) {
        int32_t value = 0;
        jnumber_get_i32(mtu_value, &value);
        *mtu = value;
    }
    jdomparser_release(&parser);
    return ok;
}

static void gatt_disconnect(steam_pad_t *pad) {
    if (!pad->gatt_connected) {
        return;
    }
    char payload[64], error[128];
    snprintf(payload, sizeof(payload), "{\"address\":\"%s\"}", pad->addr);
    if (!gatt_call(URI_GATT_DISCONNECT, payload, error, sizeof(error), NULL)) {
        commons_log_warn("SteamBLE", "client/disconnect %s failed: %s", pad->addr, error);
    }
    pad->gatt_connected = false;
}

static void wake_pad(steam_pad_t *pad) {
    char payload[96], error[128];
    pad->attempts++;
    if (!pad->gatt_connected) {
        snprintf(payload, sizeof(payload), "{\"address\":\"%s\",\"subscribe\":true}", pad->addr);
        bool connected = gatt_call(URI_GATT_CONNECT, payload, error, sizeof(error), NULL);
        if (!connected && strstr(error, "already") != NULL) {
            /* Left over from a run that exited without client/disconnect. */
            commons_log_info("SteamBLE", "client/connect %s: %s; disconnecting stale client", pad->addr, error);
            pad->gatt_connected = true;
            gatt_disconnect(pad);
            connected = gatt_call(URI_GATT_CONNECT, payload, error, sizeof(error), NULL);
        }
        if (!connected) {
            commons_log_warn("SteamBLE", "client/connect %s failed (attempt %d): %s", pad->addr, pad->attempts,
                             error);
            return;
        }
        pad->gatt_connected = true;
    }
    snprintf(payload, sizeof(payload), "{\"address\":\"%s\",\"mtuSize\":%d}", pad->addr, REQUEST_MTU);
    int mtu = 0;
    if (!gatt_call(URI_GATT_REQUEST_MTU, payload, error, sizeof(error), &mtu)) {
        commons_log_warn("SteamBLE", "client/requestMtu %s failed (attempt %d): %s", pad->addr, pad->attempts,
                         error);
        return;
    }
    pad->mtu_done = true;
    if (mtu > 0) {
        commons_log_info("SteamBLE", "Steam Controller %s: MTU %d", pad->addr, mtu);
    } else {
        commons_log_info("SteamBLE", "Steam Controller %s: MTU %d requested", pad->addr, REQUEST_MTU);
    }
}

static void update_pads(webos_steam_ble_state_t *state, const addr_list_t *found) {
    /* Gone: it has to be done again after it reconnects. */
    for (int i = 0; i < state->count;) {
        if (list_contains(found, state->pads[i].addr)) {
            i++;
            continue;
        }
        commons_log_info("SteamBLE", "Steam Controller %s disconnected", state->pads[i].addr);
        gatt_disconnect(&state->pads[i]);
        state->pads[i] = state->pads[--state->count];
    }
    for (int i = 0; i < found->count; i++) {
        bool known = false;
        for (int j = 0; j < state->count && !known; j++) {
            known = strcmp(state->pads[j].addr, found->addr[i]) == 0;
        }
        if (!known && state->count < MAX_PADS) {
            steam_pad_t *pad = &state->pads[state->count++];
            memset(pad, 0, sizeof(*pad));
            memcpy(pad->addr, found->addr[i], ADDR_LEN);
            commons_log_info("SteamBLE", "Steam Controller %s connected over Bluetooth", pad->addr);
        }
    }
}

static void *watch_proc(void *arg) {
    webos_steam_ble_state_t *state = arg;
    uint64_t scanned = 0;
    while (!atomic_load(&state->stop)) {
        uint64_t now = now_ms();
        if (scanned == 0 || now - scanned >= SCAN_MS) {
            scanned = now;
            addr_list_t found = {.count = 0};
            scan_bt_status(&found);
            scan_input_devices(&found);
            update_pads(state, &found);
            for (int i = 0; i < state->count && !atomic_load(&state->stop); i++) {
                steam_pad_t *pad = &state->pads[i];
                if (!pad->mtu_done && pad->attempts < MAX_ATTEMPTS) {
                    wake_pad(pad);
                }
            }
        }
        usleep(100000);
    }
    for (int i = 0; i < state->count; i++) {
        gatt_disconnect(&state->pads[i]);
    }
    atomic_store(&state->finished, true);
    return NULL;
}

webos_steam_ble_state_t *webos_steam_ble_start(void) {
    webos_steam_ble_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    atomic_init(&state->stop, false);
    atomic_init(&state->finished, false);
    int rc = pthread_create(&state->thread, NULL, watch_proc, state);
    if (rc != 0) {
        commons_log_warn("SteamBLE", "watch thread did not start (%d)", rc);
        free(state);
        return NULL;
    }
    return state;
}

void webos_steam_ble_stop(webos_steam_ble_state_t *state) {
    if (state == NULL) {
        return;
    }
    atomic_store(&state->stop, true);
    /* A Luna reply that never comes must not hang app exit. */
    for (int waited = 0; waited < 100 && !atomic_load(&state->finished); waited++) {
        usleep(10000);
    }
    if (atomic_load(&state->finished)) {
        pthread_join(state->thread, NULL);
        free(state);
    } else {
        commons_log_warn("SteamBLE", "watch thread still busy at exit");
        pthread_detach(state->thread);
    }
}
