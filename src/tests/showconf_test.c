// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * Verifies `wg showconf` round-trips a WebSocket peer: it emits WSMode + the WS* keys and the
 * ws(s):// Endpoint, and an inbound peer round-trips as WSMode with no Endpoint.
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "unity.h"
#include "../encoding.c"
#include "../showconf.c"

const char *PROG_NAME = "wg";

/* showconf reads the device through ipc_get_device; feed it a prebuilt one. showconf_main frees it. */
static struct wgdevice *g_stub_dev;
int ipc_get_device(struct wgdevice **dev, const char *iface) { (void)iface; *dev = g_stub_dev; return 0; }
char *ipc_list_devices(void) { return NULL; }

void setUp(void) {}
void tearDown(void) {}

static char capture_buf[8192];
static const char *run_showconf(void)
{
	const char *argv[] = { "showconf", "wsconf" };
	int saved = dup(STDOUT_FILENO);
	FILE *tmp = tmpfile();
	long n;

	TEST_ASSERT_TRUE(saved >= 0);
	TEST_ASSERT_NOT_NULL(tmp);
	fflush(stdout);
	dup2(fileno(tmp), STDOUT_FILENO);
	showconf_main(2, argv);
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
	rewind(tmp);
	n = (long)fread(capture_buf, 1, sizeof capture_buf - 1, tmp);
	capture_buf[n < 0 ? 0 : n] = '\0';
	fclose(tmp);
	return capture_buf;
}

static struct wgpeer *new_ws_peer(enum wgpeer_transport transport)
{
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(peer);
	peer->flags = WGPEER_HAS_PUBLIC_KEY | WGPEER_HAS_TRANSPORT;
	peer->transport = transport;
	return peer;
}

static void test_showconf_roundtrip_ws(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = new_ws_peer(WGPEER_TRANSPORT_WEBSOCKET);
	const char *out;

	TEST_ASSERT_NOT_NULL(dev);
	peer->ws_url = strdup("wss://127.0.0.1:8443/wg");
	peer->ws_bearer = strdup("btok");
	peer->endpoint.addr4.sin_family = AF_INET;
	peer->endpoint.addr4.sin_port = htons(8443);
	inet_pton(AF_INET, "127.0.0.1", &peer->endpoint.addr4.sin_addr);
	dev->first_peer = dev->last_peer = peer;
	g_stub_dev = dev;

	out = run_showconf();
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "WSMode = websocket"), "WSMode line");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "Endpoint = wss://127.0.0.1:8443/wg"), "URL endpoint");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "WSBearer = btok"), "bearer round-trips in showconf");
	/* showconf must not emit a numeric ip:port Endpoint for a WS peer. */
	TEST_ASSERT_NULL_MESSAGE(strstr(out, "Endpoint = 127.0.0.1:8443"), "no numeric endpoint for WS peer");
}

static void test_showconf_roundtrip_inbound_ws(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = new_ws_peer(WGPEER_TRANSPORT_WEBSOCKET);
	const char *out;

	TEST_ASSERT_NOT_NULL(dev);
	dev->first_peer = dev->last_peer = peer;   /* inbound: no ws_url, no endpoint */
	g_stub_dev = dev;

	out = run_showconf();
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "WSMode = websocket"), "inbound keeps WSMode");
	TEST_ASSERT_NULL_MESSAGE(strstr(out, "Endpoint = "), "inbound peer has no Endpoint");
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_showconf_roundtrip_ws);
	RUN_TEST(test_showconf_roundtrip_inbound_ws);
	return UNITY_END();
}
