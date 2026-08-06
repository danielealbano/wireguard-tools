// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../containers.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_containers_free_wgdevice_frees_ws_fields(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(peer);
	dev->ws_listen = strdup("wss://0.0.0.0:443/wg");
	peer->endpoint_url = strdup("wss://h:443/p");
	peer->ws_mode = strdup("wstunnel");
	peer->wstunnel_target = strdup("127.0.0.1:51820");
	peer->ws_bearer = strdup("tok");
	dev->first_peer = dev->last_peer = peer;

	free_wgdevice(dev); /* ASan/LSan: all five WS strings must be freed */
}

static void test_containers_free_wgdevice_null_ws_fields(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(peer);
	dev->first_peer = dev->last_peer = peer; /* every WS pointer NULL */

	free_wgdevice(dev); /* free(NULL) must not crash */
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_containers_free_wgdevice_frees_ws_fields);
	RUN_TEST(test_containers_free_wgdevice_null_ws_fields);
	return UNITY_END();
}
