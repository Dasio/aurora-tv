#pragma once

#include <stdbool.h>

/**
 * The client's own link to the host, sent to Rubylight hosts for packet pacing
 * (x-rl-video[0].clientWireless and x-ss-video[0].pyrowaveLinkMbps). The host
 * sees only its own route, not a TV behind an access point or a 100 Mb/s port.
 */
typedef struct {
    /** Interface the route to the host leaves by (IF_NAMESIZE). */
    char ifname[16];
    bool wireless;
    /** Speed of a wired interface in Mb/s; 0 when wireless or unknown. */
    int link_mbps;
} link_hint_t;

/**
 * Finds the interface that routes to host (an address or host name) and reads
 * its type and speed. Returns false, with a zeroed hint, when the route or the
 * interface cannot be found, or the host is on loopback.
 */
bool link_hint_for_host(const char *host, link_hint_t *hint);

/** Reads ifname's type and speed under sysfs_net (normally /sys/class/net). */
void link_hint_read_sysfs(const char *sysfs_net, const char *ifname, link_hint_t *hint);
