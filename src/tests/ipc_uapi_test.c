// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include "../curve25519.c"
#undef __linux__ /* test the userspace UAPI path only (no kernel backend), like fuzz/uapi.c */
#include "../ipc.c"
#include "../encoding.c"
#include "test_uapi_seam.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"

/* 64 hex chars = one WG key. */
#define HEX64 "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"

void setUp(void) {}
void tearDown(void) {}

static struct wgdevice *new_dev(const char *name)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));

	TEST_ASSERT_NOT_NULL(dev);
	strncpy(dev->name, name, IFNAMSIZ - 1);
	return dev;
}

static struct wgpeer *add_peer(struct wgdevice *dev)
{
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(peer);
	peer->flags |= WGPEER_HAS_PUBLIC_KEY;
	dev->first_peer = dev->last_peer = peer;
	return peer;
}

static void test_uapi_set_emits_ws_listen_when_flag(void)
{
	struct wgdevice *dev = new_dev("wgt0");
	struct seam s;

	dev->flags |= WGDEVICE_HAS_WS_LISTEN;
	dev->ws_listen = strdup("wss://0.0.0.0:443/wg");
	seam_start(&s, "wgt0", "errno=0\n\n");
	TEST_ASSERT_EQUAL_INT(0, userspace_set_device(dev));
	seam_stop(&s);
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "ws_listen=wss://0.0.0.0:443/wg\n"));
	free_wgdevice(dev);
}

static void test_uapi_set_omits_ws_listen_when_absent(void)
{
	struct wgdevice *dev = new_dev("wgt0");
	struct seam s;

	seam_start(&s, "wgt0", "errno=0\n\n");
	TEST_ASSERT_EQUAL_INT(0, userspace_set_device(dev));
	seam_stop(&s);
	TEST_ASSERT_NULL(strstr(s.captured, "ws_listen"));
	free_wgdevice(dev);
}

static void test_uapi_set_emits_url_endpoint_and_ws_keys(void)
{
	struct wgdevice *dev = new_dev("wgt0");
	struct wgpeer *peer = add_peer(dev);
	struct seam s;

	peer->endpoint_url = strdup("wss://h:443");
	peer->ws_mode = strdup("wstunnel");
	peer->wstunnel_target = strdup("1.2.3.4:5");
	peer->ws_bearer = strdup("tok");
	seam_start(&s, "wgt0", "errno=0\n\n");
	TEST_ASSERT_EQUAL_INT(0, userspace_set_device(dev));
	seam_stop(&s);
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "endpoint=wss://h:443\n"));
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "ws_mode=wstunnel\n"));
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "wstunnel_target=1.2.3.4:5\n"));
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "ws_bearer=tok\n"));
	free_wgdevice(dev);
}

static void test_uapi_set_udp_peer_byte_identical(void)
{
	struct wgdevice *dev = new_dev("wgt0");
	struct wgpeer *peer = add_peer(dev);
	struct seam s;

	peer->endpoint.addr4.sin_family = AF_INET;
	peer->endpoint.addr4.sin_port = htons(51820);
	TEST_ASSERT_EQUAL_INT(1, inet_pton(AF_INET, "1.2.3.4", &peer->endpoint.addr4.sin_addr));
	seam_start(&s, "wgt0", "errno=0\n\n");
	TEST_ASSERT_EQUAL_INT(0, userspace_set_device(dev));
	seam_stop(&s);
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "endpoint=1.2.3.4:51820\n"));
	TEST_ASSERT_NULL(strstr(s.captured, "ws_"));
	free_wgdevice(dev);
}

static void test_uapi_get_reads_back_ws_fields(void)
{
	struct wgdevice *dev = NULL;
	struct seam s;

	seam_start(&s, "wgt1",
		"ws_listen=wss://0.0.0.0:443/wg\n"
		"public_key=" HEX64 "\n"
		"endpoint=wss://h:443\n"
		"ws_mode=wstunnel\n"
		"wstunnel_target=1.2.3.4:5\n"
		"ws_bearer=tok\n"
		"errno=0\n\n");
	TEST_ASSERT_EQUAL_INT(0, userspace_get_device(&dev, "wgt1"));
	seam_stop(&s);
	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_EQUAL_STRING("wss://0.0.0.0:443/wg", dev->ws_listen);
	TEST_ASSERT_EQUAL_STRING("wss://h:443", dev->first_peer->endpoint_url);
	TEST_ASSERT_EQUAL_STRING("wstunnel", dev->first_peer->ws_mode);
	TEST_ASSERT_EQUAL_STRING("1.2.3.4:5", dev->first_peer->wstunnel_target);
	TEST_ASSERT_EQUAL_STRING("tok", dev->first_peer->ws_bearer);
	free_wgdevice(dev);
}

static void test_uapi_get_ws_endpoint_is_url(void)
{
	struct wgdevice *dev = NULL;
	struct seam s;

	seam_start(&s, "wgt1", "public_key=" HEX64 "\nendpoint=wss://h:443\nws_mode=standard\nerrno=0\n\n");
	TEST_ASSERT_EQUAL_INT(0, userspace_get_device(&dev, "wgt1"));
	seam_stop(&s);
	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_EQUAL_STRING("wss://h:443", dev->first_peer->endpoint_url);
	TEST_ASSERT_EQUAL_INT(0, dev->first_peer->endpoint.addr.sa_family);
	free_wgdevice(dev);
}

static void test_uapi_get_unknown_key_ignored(void)
{
	struct wgdevice *dev = NULL;
	struct seam s;

	seam_start(&s, "wgt1", "public_key=" HEX64 "\nws_future=x\nerrno=0\n\n");
	TEST_ASSERT_EQUAL_INT(0, userspace_get_device(&dev, "wgt1"));
	seam_stop(&s);
	TEST_ASSERT_NOT_NULL(dev);
	free_wgdevice(dev);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_uapi_set_emits_ws_listen_when_flag);
	RUN_TEST(test_uapi_set_omits_ws_listen_when_absent);
	RUN_TEST(test_uapi_set_emits_url_endpoint_and_ws_keys);
	RUN_TEST(test_uapi_set_udp_peer_byte_identical);
	RUN_TEST(test_uapi_get_reads_back_ws_fields);
	RUN_TEST(test_uapi_get_ws_endpoint_is_url);
	RUN_TEST(test_uapi_get_unknown_key_ignored);
	return UNITY_END();
}
