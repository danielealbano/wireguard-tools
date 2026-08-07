// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * Unit tests for the WebSocket/wstunnel config-file and CLI parsing in config.c.
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "unity.h"
#include "../config.c"
#include "../encoding.c"

#define PUBKEY "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="

void setUp(void) {}
void tearDown(void) {}

/* Parse a whole config-file body (a setconf, append=false). Returns the device or NULL on any
 * error. On failure config_read_line/finish already free the partial device. */
static struct wgdevice *parse_conf(const char *body)
{
	struct config_ctx ctx;
	char *copy = strdup(body), *save = NULL, *line;
	bool ok = true;

	TEST_ASSERT_NOT_NULL(copy);
	if (!config_read_init(&ctx, false)) {
		free(copy);
		return NULL;
	}
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!config_read_line(&ctx, line)) {
			ok = false;
			break;
		}
	}
	free(copy);
	return ok ? config_read_finish(&ctx) : NULL;
}

static struct wgpeer *first_peer(struct wgdevice *dev)
{
	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(dev->first_peer);
	return dev->first_peer;
}

/* Capture stderr produced by fn(arg) into buf; returns fn's result. */
static bool capture_stderr(bool (*fn)(const char *), const char *arg, char *buf, size_t bufsz)
{
	int saved = dup(STDERR_FILENO);
	FILE *tmp = tmpfile();
	long n;
	bool ret;

	TEST_ASSERT_TRUE(saved >= 0);
	TEST_ASSERT_NOT_NULL(tmp);
	fflush(stderr);
	dup2(fileno(tmp), STDERR_FILENO);
	ret = fn(arg);
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
	rewind(tmp);
	n = (long)fread(buf, 1, bufsz - 1, tmp);
	buf[n < 0 ? 0 : n] = '\0';
	fclose(tmp);
	return ret;
}

static bool parse_conf_ok(const char *body)
{
	struct wgdevice *dev = parse_conf(body);
	bool ok = dev != NULL;

	free_wgdevice(dev);
	return ok;
}

static void test_config_parse_endpoint_ws_dialing_resolves(void)
{
	struct wgdevice *dev = parse_conf("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://127.0.0.1:8443/wg\nWSMode = websocket\n");
	struct wgpeer *peer = first_peer(dev);

	TEST_ASSERT_EQUAL_INT(WGPEER_TRANSPORT_WEBSOCKET, peer->transport);
	TEST_ASSERT_EQUAL_STRING("wss://127.0.0.1:8443/wg", peer->ws_url);
	TEST_ASSERT_EQUAL_INT(AF_INET, peer->endpoint.addr.sa_family);
	TEST_ASSERT_EQUAL_UINT16(htons(8443), peer->endpoint.addr4.sin_port);
	free_wgdevice(dev);
}

static void test_config_parse_wstunnel_requires_target(void)
{
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://127.0.0.1:8443/v1\nWSMode = wstunnel\n"));
	TEST_ASSERT_TRUE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://127.0.0.1:8443/v1\nWSMode = wstunnel\nWSTunnelTarget = 127.0.0.1:51820\n"));
}

static void test_config_parse_wsmode_without_endpoint_inbound(void)
{
	struct wgdevice *dev = parse_conf("[Peer]\nPublicKey = " PUBKEY "\nWSMode = websocket\n");
	struct wgpeer *peer = first_peer(dev);

	TEST_ASSERT_EQUAL_INT(WGPEER_TRANSPORT_WEBSOCKET, peer->transport);
	TEST_ASSERT_NULL(peer->ws_url);
	TEST_ASSERT_EQUAL_INT(AF_UNSPEC, peer->endpoint.addr.sa_family);
	free_wgdevice(dev);
}

static void test_config_parse_wstunnel_inbound_rejected(void)
{
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nWSMode = wstunnel\n"));
}

static void test_config_parse_wsmode_required_for_ws_endpoint(void)
{
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://127.0.0.1:8443/wg\n"));
}

static void test_config_parse_ws_keys_rejected_on_udp(void)
{
	/* Including false/zero values, which the WGPEER_HAS_WS_SETTINGS presence flag still catches. */
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 127.0.0.1:51820\nWSBearer = tok\n"));
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 127.0.0.1:51820\nWSMask = false\n"));
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 127.0.0.1:51820\nWSTLSInsecure = false\n"));
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 127.0.0.1:51820\nWSPingInterval = 0\n"));
}

static void test_config_parse_endpoint_and_wsmode_conflict(void)
{
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 127.0.0.1:51820\nWSMode = websocket\n"));
}

static void test_config_parse_ws_bool_millis_invalid(void)
{
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://127.0.0.1:8443/wg\nWSMode = websocket\nWSMask = maybe\n"));
	TEST_ASSERT_FALSE(parse_conf_ok("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://127.0.0.1:8443/wg\nWSMode = websocket\nWSPingInterval = 1x\n"));
}

static void test_config_parse_bearer_error_hides_value(void)
{
	char buf[512];
	bool ret = capture_stderr(parse_conf_ok,
				  "[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 127.0.0.1:51820\nWSBearer = supersecrettoken\n",
				  buf, sizeof buf);

	TEST_ASSERT_FALSE(ret);
	TEST_ASSERT_NULL_MESSAGE(strstr(buf, "supersecrettoken"), "bearer value leaked into a diagnostic");
}

static void test_config_parse_wslisten_empty_clears(void)
{
	struct wgdevice *dev = parse_conf("[Interface]\nWSListen =\n");

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_TRUE(dev->flags & WGDEVICE_HAS_WS_LISTEN);
	TEST_ASSERT_NULL(dev->ws_listen);
	free_wgdevice(dev);
}

static void test_config_parse_device_ws_empty_accepted(void)
{
	struct wgdevice *dev = parse_conf("[Interface]\nWSServerTLSCert =\nWSServerBearer =\nWSTrustedProxies =\n");

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_TRUE(dev->flags & WGDEVICE_HAS_WS_SERVER_TLS_CERT);
	TEST_ASSERT_TRUE(dev->flags & WGDEVICE_HAS_WS_SERVER_BEARER);
	TEST_ASSERT_TRUE(dev->flags & WGDEVICE_HAS_WS_TRUSTED_PROXIES);
	TEST_ASSERT_NULL(dev->ws_server_tls_cert);
	free_wgdevice(dev);
}

static void test_config_parse_ws_endpoint_ipv6_literal(void)
{
	struct wgdevice *dev = parse_conf("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://[::1]:443/wg\nWSMode = websocket\n");
	struct wgpeer *peer = first_peer(dev);

	TEST_ASSERT_EQUAL_INT(AF_INET6, peer->endpoint.addr.sa_family);
	TEST_ASSERT_EQUAL_UINT16(htons(443), peer->endpoint.addr6.sin6_port);
	TEST_ASSERT_EQUAL_STRING("wss://[::1]:443/wg", peer->ws_url);
	free_wgdevice(dev);
}

static void test_config_cmd_ws_tokens_parity(void)
{
	const char *argv[] = { "peer", PUBKEY, "endpoint", "wss://127.0.0.1:8443/v1", "ws-mode", "wstunnel", "wstunnel-target", "127.0.0.1:51820" };
	struct wgdevice *dev = config_read_cmd(argv, 8);
	struct wgpeer *peer = first_peer(dev);

	TEST_ASSERT_EQUAL_INT(WGPEER_TRANSPORT_WSTUNNEL, peer->transport);
	TEST_ASSERT_EQUAL_STRING("wss://127.0.0.1:8443/v1", peer->ws_url);
	TEST_ASSERT_EQUAL_STRING("127.0.0.1:51820", peer->wstunnel_target);
	TEST_ASSERT_TRUE(peer->flags & WGPEER_HAS_TRANSPORT);
	free_wgdevice(dev);
}

static void test_config_cmd_incremental_omits_transport(void)
{
	const char *argv[] = { "peer", PUBKEY, "persistent-keepalive", "25" };
	struct wgdevice *dev = config_read_cmd(argv, 4);
	struct wgpeer *peer = first_peer(dev);

	TEST_ASSERT_FALSE(peer->flags & WGPEER_HAS_TRANSPORT);
	free_wgdevice(dev);
}

static void test_config_cmd_incremental_ws_setting_allowed(void)
{
	char path[] = "/tmp/wg-ws-bearer-XXXXXX";
	int fd = mkstemp(path);
	const char *argv[] = { "peer", PUBKEY, "ws-bearer", path };
	struct wgdevice *dev;
	struct wgpeer *peer;

	TEST_ASSERT_TRUE(fd >= 0);
	TEST_ASSERT_TRUE(write(fd, "tok\n", 4) == 4);
	close(fd);
	dev = config_read_cmd(argv, 4);
	unlink(path);
	peer = first_peer(dev);
	TEST_ASSERT_FALSE(peer->flags & WGPEER_HAS_TRANSPORT);
	TEST_ASSERT_TRUE(peer->flags & WGPEER_HAS_WS_SETTINGS);
	TEST_ASSERT_EQUAL_STRING("tok", peer->ws_bearer);
	free_wgdevice(dev);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_config_parse_endpoint_ws_dialing_resolves);
	RUN_TEST(test_config_parse_wstunnel_requires_target);
	RUN_TEST(test_config_parse_wsmode_without_endpoint_inbound);
	RUN_TEST(test_config_parse_wstunnel_inbound_rejected);
	RUN_TEST(test_config_parse_wsmode_required_for_ws_endpoint);
	RUN_TEST(test_config_parse_ws_keys_rejected_on_udp);
	RUN_TEST(test_config_parse_endpoint_and_wsmode_conflict);
	RUN_TEST(test_config_parse_ws_bool_millis_invalid);
	RUN_TEST(test_config_parse_bearer_error_hides_value);
	RUN_TEST(test_config_parse_wslisten_empty_clears);
	RUN_TEST(test_config_parse_device_ws_empty_accepted);
	RUN_TEST(test_config_parse_ws_endpoint_ipv6_literal);
	RUN_TEST(test_config_cmd_ws_tokens_parity);
	RUN_TEST(test_config_cmd_incremental_omits_transport);
	RUN_TEST(test_config_cmd_incremental_ws_setting_allowed);
	return UNITY_END();
}
