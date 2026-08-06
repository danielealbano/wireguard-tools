// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * Verifies free_wgdevice frees the full device/peer graph including the WebSocket strings.
 */

#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "../containers.h"

void setUp(void) {}
void tearDown(void) {}

/* Built with ASan+LSan: a leaked WebSocket string fails the run. */
static void test_containers_free_ws_graph(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(peer);
	dev->ws_listen = strdup("wss://0.0.0.0:8443/wg");
	dev->ws_server_tls_cert = strdup("/c.pem");
	dev->ws_server_tls_key = strdup("/k.pem");
	dev->ws_server_bearer = strdup("srvsecret");
	dev->ws_trusted_proxies = strdup("10.0.0.0/8");
	peer->ws_url = strdup("wss://h:8443/v1");
	peer->wstunnel_target = strdup("127.0.0.1:51820");
	peer->ws_bearer = strdup("btok");
	peer->ws_tls_ca = strdup("/ca.pem");
	peer->ws_tls_cert = strdup("/cc.pem");
	peer->ws_tls_key = strdup("/ck.pem");
	dev->first_peer = dev->last_peer = peer;

	free_wgdevice(dev);
	TEST_ASSERT_TRUE(true);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_containers_free_ws_graph);
	return UNITY_END();
}
