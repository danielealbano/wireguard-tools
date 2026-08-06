// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * Round-trip tests for the WebSocket UAPI keys, driving the real userspace_set_device/
 * userspace_get_device over a test-owned UNIX socket (test_uapi_seam.h).
 */

#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include "unity.h"
#include "../curve25519.c"
#include "../encoding.c"
#include "../ipc.c"
#include "test_uapi_seam.h"

void setUp(void) {}
void tearDown(void) {}

static void set_ip_endpoint(struct wgpeer *peer, const char *ip, uint16_t port)
{
	peer->endpoint.addr4.sin_family = AF_INET;
	peer->endpoint.addr4.sin_port = htons(port);
	TEST_ASSERT_EQUAL_INT(1, inet_pton(AF_INET, ip, &peer->endpoint.addr4.sin_addr));
}

static struct wgdevice *ws_peer_device(const char *iface, enum wgpeer_transport transport, uint32_t peer_flags)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(peer);
	strncpy(dev->name, iface, IFNAMSIZ - 1);
	dev->first_peer = dev->last_peer = peer;
	peer->flags = peer_flags;
	peer->transport = transport;
	return dev;
}

static void test_ipc_uapi_roundtrip_websocket(void)
{
	struct seam s;
	struct wgdevice *dev = ws_peer_device("wsset", WGPEER_TRANSPORT_WEBSOCKET, WGPEER_HAS_PUBLIC_KEY | WGPEER_HAS_TRANSPORT);
	int r;

	set_ip_endpoint(dev->first_peer, "127.0.0.1", 8443);
	dev->first_peer->ws_url = strdup("wss://127.0.0.1:8443/wg");
	dev->first_peer->ws_bearer = strdup("btok");
	dev->first_peer->ws_mask = true;
	seam_start(&s, "wsset", "errno=0\n\n");
	r = userspace_set_device(dev);
	seam_stop(&s);
	TEST_ASSERT_EQUAL_INT(0, r);
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "transport=websocket\n"), "transport=");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "endpoint=127.0.0.1:8443\n"), "endpoint=ip:port");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_url=wss://127.0.0.1:8443/wg\n"), "ws_url=");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_bearer=btok\n"), "ws_bearer=");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_mask=true\n"), "ws_mask=true");
	free_wgdevice(dev);
}

static void test_ipc_uapi_roundtrip_wstunnel(void)
{
	struct seam s;
	struct wgdevice *dev = ws_peer_device("wstun", WGPEER_TRANSPORT_WSTUNNEL, WGPEER_HAS_PUBLIC_KEY | WGPEER_HAS_TRANSPORT);
	int r;

	set_ip_endpoint(dev->first_peer, "127.0.0.1", 8443);
	dev->first_peer->ws_url = strdup("wss://127.0.0.1:8443/v1");
	dev->first_peer->wstunnel_target = strdup("127.0.0.1:51820");
	seam_start(&s, "wstun", "errno=0\n\n");
	r = userspace_set_device(dev);
	seam_stop(&s);
	TEST_ASSERT_EQUAL_INT(0, r);
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "transport=wstunnel\n"));
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "wstunnel_target=127.0.0.1:51820\n"));
	free_wgdevice(dev);
}

static void test_ipc_uapi_omits_transport_without_flag(void)
{
	struct seam s;
	struct wgdevice *dev = ws_peer_device("wsinc", WGPEER_TRANSPORT_UDP, WGPEER_HAS_PUBLIC_KEY | WGPEER_HAS_WS_SETTINGS);
	int r;

	dev->first_peer->ws_bearer = strdup("btok");   /* incremental ws-setting-only update */
	seam_start(&s, "wsinc", "errno=0\n\n");
	r = userspace_set_device(dev);
	seam_stop(&s);
	TEST_ASSERT_EQUAL_INT(0, r);
	TEST_ASSERT_NULL_MESSAGE(strstr(s.captured, "transport="), "transport= must be omitted");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_bearer=btok\n"), "ws key still emitted");
	free_wgdevice(dev);
}

static void test_ipc_uapi_udp_setconf_emits_transport_no_ws(void)
{
	struct seam s;
	struct wgdevice *dev = ws_peer_device("udpc", WGPEER_TRANSPORT_UDP, WGPEER_HAS_PUBLIC_KEY | WGPEER_HAS_TRANSPORT);
	int r;

	set_ip_endpoint(dev->first_peer, "127.0.0.1", 51820);
	seam_start(&s, "udpc", "errno=0\n\n");
	r = userspace_set_device(dev);
	seam_stop(&s);
	TEST_ASSERT_EQUAL_INT(0, r);
	TEST_ASSERT_NOT_NULL(strstr(s.captured, "transport=udp\n"));
	TEST_ASSERT_NULL_MESSAGE(strstr(s.captured, "ws_"), "no ws_* keys for a UDP peer");
	free_wgdevice(dev);
}

static void test_ipc_uapi_roundtrip_get_websocket(void)
{
	struct seam s;
	struct wgdevice *dev = NULL;
	const char *reply =
		"public_key=0000000000000000000000000000000000000000000000000000000000000000\n"
		"transport=websocket\n"
		"endpoint=127.0.0.1:8443\n"
		"ws_url=wss://127.0.0.1:8443/wg\n"
		"ws_bearer=btok\n"
		"ws_mask=true\n"
		"errno=0\n\n";
	int r;

	seam_start(&s, "wsget", reply);
	r = userspace_get_device(&dev, "wsget");
	seam_stop(&s);
	TEST_ASSERT_EQUAL_INT(0, r);
	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(dev->first_peer);
	TEST_ASSERT_EQUAL_INT(WGPEER_TRANSPORT_WEBSOCKET, dev->first_peer->transport);
	TEST_ASSERT_TRUE(dev->first_peer->flags & WGPEER_HAS_TRANSPORT);
	TEST_ASSERT_EQUAL_STRING("wss://127.0.0.1:8443/wg", dev->first_peer->ws_url);
	TEST_ASSERT_EQUAL_STRING("btok", dev->first_peer->ws_bearer);
	TEST_ASSERT_TRUE(dev->first_peer->ws_mask);
	free_wgdevice(dev);
}

static void test_ipc_uapi_explicit_device_ws_empty_clears(void)
{
	struct seam s;
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	int r;

	TEST_ASSERT_NOT_NULL(dev);
	strncpy(dev->name, "wsdev", IFNAMSIZ - 1);
	/* A non-empty ws_listen plus three device keys flagged-but-empty (an explicit clear). The
	 * ws_server_tls_key flag is left unset, so that key must not be emitted at all. */
	dev->ws_listen = strdup("wss://0.0.0.0:8443/wg");
	dev->flags = WGDEVICE_HAS_WS_LISTEN | WGDEVICE_HAS_WS_SERVER_TLS_CERT |
		     WGDEVICE_HAS_WS_SERVER_BEARER | WGDEVICE_HAS_WS_TRUSTED_PROXIES;
	seam_start(&s, "wsdev", "errno=0\n\n");
	r = userspace_set_device(dev);
	seam_stop(&s);
	TEST_ASSERT_EQUAL_INT(0, r);
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_listen=wss://0.0.0.0:8443/wg\n"), "non-empty ws_listen emitted");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_server_tls_cert=\n"), "empty cert clears");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_server_bearer=\n"), "empty bearer clears");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s.captured, "ws_trusted_proxies=\n"), "empty proxies clears");
	TEST_ASSERT_NULL_MESSAGE(strstr(s.captured, "ws_server_tls_key="), "unflagged key not emitted");
	free_wgdevice(dev);
}

static void test_ipc_uapi_device_server_keys(void)
{
	struct seam s;
	struct wgdevice *dev = NULL;
	const char *reply =
		"ws_listen=wss://0.0.0.0:8443/wg\n"
		"ws_server_bearer=srvtok\n"
		"ws_trusted_proxies=10.0.0.0/8\n"
		"errno=0\n\n";
	int r;

	seam_start(&s, "wssrv", reply);
	r = userspace_get_device(&dev, "wssrv");
	seam_stop(&s);
	TEST_ASSERT_EQUAL_INT(0, r);
	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_TRUE(dev->flags & WGDEVICE_HAS_WS_LISTEN);
	TEST_ASSERT_EQUAL_STRING("wss://0.0.0.0:8443/wg", dev->ws_listen);
	TEST_ASSERT_EQUAL_STRING("srvtok", dev->ws_server_bearer);
	TEST_ASSERT_EQUAL_STRING("10.0.0.0/8", dev->ws_trusted_proxies);
	free_wgdevice(dev);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_ipc_uapi_roundtrip_websocket);
	RUN_TEST(test_ipc_uapi_roundtrip_wstunnel);
	RUN_TEST(test_ipc_uapi_omits_transport_without_flag);
	RUN_TEST(test_ipc_uapi_udp_setconf_emits_transport_no_ws);
	RUN_TEST(test_ipc_uapi_roundtrip_get_websocket);
	RUN_TEST(test_ipc_uapi_explicit_device_ws_empty_clears);
	RUN_TEST(test_ipc_uapi_device_server_keys);
	return UNITY_END();
}
