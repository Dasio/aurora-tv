#include "unity.h"
#include "libgamestream/client.h"

#include <string.h>

#define URL_PREFIX "https://192.168.1.100:47984/launch?appid=1"

static char url[512];

void setUp(void) {
    strcpy(url, URL_PREFIX);
}

void tearDown(void) {

}

void test_hdr_sends_hdr_mode_1() {
    gs_append_hdr_launch_params(url, sizeof(url), VIDEO_FORMAT_H265 | VIDEO_FORMAT_H265_MAIN10, true);
    TEST_ASSERT_NOT_NULL(strstr(url, "&hdrMode=1&"));
    TEST_ASSERT_NOT_NULL(strstr(url, "&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1"));
}

/* 10-bit SDR must not ask the host for HDR: Sunshine-family hosts turn the display to HDR on
 * hdrMode=1 even when the client only wanted Main10 for less banding. */
void test_10bit_sdr_sends_hdr_mode_0() {
    gs_append_hdr_launch_params(url, sizeof(url), VIDEO_FORMAT_H265 | VIDEO_FORMAT_H265_MAIN10, false);
    TEST_ASSERT_NOT_NULL(strstr(url, "&hdrMode=0&"));
    TEST_ASSERT_NULL(strstr(url, "hdrMode=1"));
}

void test_10bit_sdr_av1_sends_hdr_mode_0() {
    gs_append_hdr_launch_params(url, sizeof(url), VIDEO_FORMAT_AV1_MAIN8 | VIDEO_FORMAT_AV1_MAIN10, false);
    TEST_ASSERT_NOT_NULL(strstr(url, "&hdrMode=0&"));
}

void test_8bit_sends_no_hdr_params() {
    gs_append_hdr_launch_params(url, sizeof(url), VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265, true);
    TEST_ASSERT_EQUAL_STRING(URL_PREFIX, url);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_hdr_sends_hdr_mode_1);
    RUN_TEST(test_10bit_sdr_sends_hdr_mode_0);
    RUN_TEST(test_10bit_sdr_av1_sends_hdr_mode_0);
    RUN_TEST(test_8bit_sends_no_hdr_params);
    return UNITY_END();
}
