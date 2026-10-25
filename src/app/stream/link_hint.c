#include "link_hint.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSFS_NET "/sys/class/net"

static bool sysfs_exists(const char *sysfs_net, const char *ifname, const char *leaf) {
    char path[256];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s/%s", sysfs_net, ifname, leaf);
    return stat(path, &st) == 0;
}

/* cfg80211 drivers have phy80211 and DEVTYPE=wlan; WEXT-only drivers have wireless. */
static bool is_wireless(const char *sysfs_net, const char *ifname) {
    if (sysfs_exists(sysfs_net, ifname, "phy80211") || sysfs_exists(sysfs_net, ifname, "wireless")) {
        return true;
    }
    char path[256], line[64];
    snprintf(path, sizeof(path), "%s/%s/uevent", sysfs_net, ifname);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }
    bool wlan = false;
    while (!wlan && fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        wlan = strcmp(line, "DEVTYPE=wlan") == 0;
    }
    fclose(fp);
    return wlan;
}

/* speed reads -1, or fails with EINVAL, while the link is down or the driver does not report it. */
static int read_speed(const char *sysfs_net, const char *ifname) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/speed", sysfs_net, ifname);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    int mbps = 0;
    if (fscanf(fp, "%d", &mbps) != 1 || mbps < 0) {
        mbps = 0;
    }
    fclose(fp);
    return mbps;
}

void link_hint_read_sysfs(const char *sysfs_net, const char *ifname, link_hint_t *hint) {
    hint->wireless = is_wireless(sysfs_net, ifname);
    hint->link_mbps = hint->wireless ? 0 : read_speed(sysfs_net, ifname);
}

static bool same_address(const struct sockaddr *a, const struct sockaddr *b) {
    if (a == NULL || a->sa_family != b->sa_family) {
        return false;
    }
    if (a->sa_family == AF_INET) {
        return ((const struct sockaddr_in *) a)->sin_addr.s_addr == ((const struct sockaddr_in *) b)->sin_addr.s_addr;
    }
    if (a->sa_family == AF_INET6) {
        return memcmp(&((const struct sockaddr_in6 *) a)->sin6_addr, &((const struct sockaddr_in6 *) b)->sin6_addr,
                      sizeof(struct in6_addr)) == 0;
    }
    return false;
}

/* Connecting a UDP socket sends nothing: the kernel only picks the local address that routes to the host. */
static bool local_address_for(const char *host, struct sockaddr_storage *local) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, "47998", &hints, &res) != 0) {
        return false;
    }
    bool found = false;
    for (struct addrinfo *ai = res; ai != NULL && !found; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            continue;
        }
        socklen_t len = sizeof(*local);
        found = connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
                getsockname(fd, (struct sockaddr *) local, &len) == 0;
        close(fd);
    }
    freeaddrinfo(res);
    return found;
}

bool link_hint_for_host(const char *host, link_hint_t *hint) {
    memset(hint, 0, sizeof(*hint));
    struct sockaddr_storage local;
    if (host == NULL || !local_address_for(host, &local)) {
        return false;
    }
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) {
        return false;
    }
    for (struct ifaddrs *ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {
        if (!(ifa->ifa_flags & IFF_LOOPBACK) && same_address(ifa->ifa_addr, (const struct sockaddr *) &local)) {
            snprintf(hint->ifname, sizeof(hint->ifname), "%s", ifa->ifa_name);
            break;
        }
    }
    freeifaddrs(ifs);
    /* IPv4 aliases (eth0:1) share their device's sysfs entry. */
    hint->ifname[strcspn(hint->ifname, ":")] = '\0';
    if (hint->ifname[0] == '\0') {
        return false;
    }
    link_hint_read_sysfs(SYSFS_NET, hint->ifname, hint);
    return true;
}
