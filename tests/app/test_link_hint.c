#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "stream/link_hint.h"

static char sysfs[64];

static void make_dir(const char *ifname, const char *leaf) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", sysfs, ifname);
    mkdir(path, 0755);
    if (leaf != NULL) {
        snprintf(path, sizeof(path), "%s/%s/%s", sysfs, ifname, leaf);
        mkdir(path, 0755);
    }
}

static void write_file(const char *ifname, const char *leaf, const char *content) {
    char path[256];
    make_dir(ifname, NULL);
    snprintf(path, sizeof(path), "%s/%s/%s", sysfs, ifname, leaf);
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs(content, fp);
    fclose(fp);
}

static link_hint_t read_hint(const char *ifname) {
    link_hint_t hint = {.wireless = true, .link_mbps = -1};
    link_hint_read_sysfs(sysfs, ifname, &hint);
    return hint;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void) st;
    (void) flag;
    (void) ftw;
    return remove(path);
}

void setUp() {
    snprintf(sysfs, sizeof(sysfs), "/tmp/aurora-sysfs-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(sysfs));
}

void tearDown() {
    nftw(sysfs, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
}

void testWiredSpeed() {
    write_file("eth0", "speed", "1000\n");
    link_hint_t hint = read_hint("eth0");
    TEST_ASSERT_FALSE(hint.wireless);
    TEST_ASSERT_EQUAL_INT(1000, hint.link_mbps);
}

void testWiredSpeedUnknown() {
    write_file("eth1", "speed", "-1\n");
    make_dir("eth2", NULL);
    TEST_ASSERT_EQUAL_INT(0, read_hint("eth1").link_mbps);
    TEST_ASSERT_FALSE(read_hint("eth2").wireless);
    TEST_ASSERT_EQUAL_INT(0, read_hint("eth2").link_mbps);
}

void testWirelessIgnoresSpeed() {
    make_dir("wlan0", "phy80211");
    write_file("wlan0", "speed", "866\n");
    make_dir("wlan1", "wireless");
    write_file("wlan2", "uevent", "INTERFACE=wlan2\nIFINDEX=3\nDEVTYPE=wlan\n");
    const char *names[] = {"wlan0", "wlan1", "wlan2"};
    for (int i = 0; i < 3; i++) {
        link_hint_t hint = read_hint(names[i]);
        TEST_ASSERT_TRUE_MESSAGE(hint.wireless, names[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, hint.link_mbps, names[i]);
    }
}

void testOtherDevtypeIsWired() {
    write_file("usb0", "uevent", "INTERFACE=usb0\nDEVTYPE=wlanx\n");
    write_file("usb0", "speed", "100\n");
    link_hint_t hint = read_hint("usb0");
    TEST_ASSERT_FALSE(hint.wireless);
    TEST_ASSERT_EQUAL_INT(100, hint.link_mbps);
}

void testLoopbackHasNoHint() {
    link_hint_t hint;
    TEST_ASSERT_FALSE(link_hint_for_host("127.0.0.1", &hint));
    TEST_ASSERT_EQUAL_STRING("", hint.ifname);
    TEST_ASSERT_FALSE(link_hint_for_host(NULL, &hint));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testWiredSpeed);
    RUN_TEST(testWiredSpeedUnknown);
    RUN_TEST(testWirelessIgnoresSpeed);
    RUN_TEST(testOtherDevtypeIsWired);
    RUN_TEST(testLoopbackHasNoHint);
    return UNITY_END();
}
