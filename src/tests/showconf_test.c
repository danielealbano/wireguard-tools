// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include "../showconf.c"
#include "../encoding.c"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"

const char *PROG_NAME = "wg";

/* Link-time stub for the SUT's only external dependency. */
static struct wgdevice *g_dev;
int ipc_get_device(struct wgdevice **dev, const char *iface)
{
	(void)iface;
	*dev = g_dev;
	return 0;
}

void setUp(void) {}
void tearDown(void) {}

/* showconf_main takes ownership of g_dev (frees it). */
static void capture_showconf(struct wgdevice *dev, char *out, size_t outlen)
{
	const char *argv[] = { "showconf", "wg0" };
	FILE *tmp = tmpfile();
	int saved;

	TEST_ASSERT_NOT_NULL(tmp);
	g_dev = dev;
	fflush(stdout);
	saved = dup(fileno(stdout));
	TEST_ASSERT_TRUE(saved >= 0);
	TEST_ASSERT_TRUE(dup2(fileno(tmp), fileno(stdout)) >= 0);
	showconf_main(2, argv);
	fflush(stdout);
	TEST_ASSERT_TRUE(dup2(saved, fileno(stdout)) >= 0);
	close(saved);
	rewind(tmp);
	memset(out, 0, outlen);
	TEST_ASSERT_TRUE(fread(out, 1, outlen - 1, tmp) > 0);
	fclose(tmp);
}

static struct wgpeer *ws_peer(void)
{
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(peer);
	peer->flags |= WGPEER_HAS_PUBLIC_KEY;
	peer->endpoint_url = strdup("wss://h:443/wg");
	peer->ws_mode = strdup("wstunnel");
	peer->wstunnel_target = strdup("127.0.0.1:51820");
	peer->ws_bearer = strdup("tok");
	return peer;
}

static void test_showconf_emits_ws_listen_and_peer_keys(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	char out[4096];

	TEST_ASSERT_NOT_NULL(dev);
	dev->flags |= WGDEVICE_HAS_WS_LISTEN;
	dev->ws_listen = strdup("wss://0.0.0.0:443/wg");
	dev->first_peer = dev->last_peer = ws_peer();
	capture_showconf(dev, out, sizeof(out));
	TEST_ASSERT_NOT_NULL(strstr(out, "WSListen = wss://0.0.0.0:443/wg\n"));
	TEST_ASSERT_NOT_NULL(strstr(out, "WSMode = wstunnel\n"));
	TEST_ASSERT_NOT_NULL(strstr(out, "WSTunnelTarget = 127.0.0.1:51820\n"));
	TEST_ASSERT_NOT_NULL(strstr(out, "WSPeerBearer = tok\n"));
}

static void test_showconf_url_endpoint(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	char out[4096];

	TEST_ASSERT_NOT_NULL(dev);
	dev->first_peer = dev->last_peer = ws_peer();
	capture_showconf(dev, out, sizeof(out));
	TEST_ASSERT_NOT_NULL(strstr(out, "Endpoint = wss://h:443/wg\n"));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_showconf_emits_ws_listen_and_peer_keys);
	RUN_TEST(test_showconf_url_endpoint);
	return UNITY_END();
}
