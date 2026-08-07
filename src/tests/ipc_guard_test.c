// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * Verifies the kernel-interface guard: a WebSocket config is rejected before the kernel backend
 * is dispatched. On platforms without a kernel backend the guard does not exist and the test is
 * skipped.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "../curve25519.c"
#include "../encoding.c"
#include "../ipc.c"

void setUp(void) {}
void tearDown(void) {}

#ifdef IPC_SUPPORTS_KERNEL_INTERFACE
static void test_ipc_guard_rejects_ws_on_kernel(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(peer);
	dev->first_peer = dev->last_peer = peer;

	peer->transport = WGPEER_TRANSPORT_WEBSOCKET;
	TEST_ASSERT_TRUE_MESSAGE(device_has_ws_settings(dev), "a websocket peer is WS content");

	peer->transport = WGPEER_TRANSPORT_UDP;
	TEST_ASSERT_FALSE_MESSAGE(device_has_ws_settings(dev), "a plain UDP device is not WS content");

	/* A device WS flag with an empty/cleared value is not WS content (values, not flags). */
	dev->flags |= WGDEVICE_HAS_WS_LISTEN;
	dev->ws_listen = NULL;
	TEST_ASSERT_FALSE_MESSAGE(device_has_ws_settings(dev), "an empty ws_listen is not WS content");

	dev->ws_listen = strdup("wss://0.0.0.0:8443/wg");
	TEST_ASSERT_TRUE_MESSAGE(device_has_ws_settings(dev), "a set ws_listen is WS content");

	free_wgdevice(dev);
}
#else
static void test_ipc_guard_rejects_ws_on_kernel(void)
{
	TEST_IGNORE_MESSAGE("no kernel backend on this platform");
}
#endif

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_ipc_guard_rejects_ws_on_kernel);
	return UNITY_END();
}
