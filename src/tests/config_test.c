// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <stdio.h>
#undef stderr
#define stderr stdin /* silence the parser's diagnostics during tests */
#include "../config.c"
#include "../encoding.c"
#undef stderr

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"

const char *PROG_NAME = "wg";

#define PUBKEY "KGEENOK8oMnmpZS4PNNih4y0M0GpSAjbdejRIk+H0AI="

void setUp(void) {}
void tearDown(void) {}

/* Parse a whole config string (config-file path). Returns the device or NULL. */
static struct wgdevice *parse_file(const char *cfg, bool append)
{
	struct config_ctx ctx;
	char *buf = strdup(cfg), *saveptr, *line;

	if (!buf)
		return NULL;
	if (!config_read_init(&ctx, append)) {
		free(buf);
		return NULL;
	}
	for (line = strtok_r(buf, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
		if (!config_read_line(&ctx, line)) { /* frees ctx.device on failure */
			free(buf);
			return NULL;
		}
	}
	free(buf);
	return config_read_finish(&ctx);
}

static void test_config_parse_wslisten_valid(void)
{
	struct wgdevice *d = parse_file("[Interface]\nWSListen = wss://h:443/wg\n", false);

	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_TRUE(d->flags & WGDEVICE_HAS_WS_LISTEN);
	TEST_ASSERT_EQUAL_STRING("wss://h:443/wg", d->ws_listen);
	free_wgdevice(d);
}

static void test_config_parse_wslisten_empty_disables(void)
{
	struct wgdevice *d = parse_file("[Interface]\nWSListen =\n", false);

	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_TRUE(d->flags & WGDEVICE_HAS_WS_LISTEN);
	TEST_ASSERT_EQUAL_STRING("", d->ws_listen);
	free_wgdevice(d);
}

static void test_config_parse_wslisten_bad_scheme(void)
{
	TEST_ASSERT_NULL(parse_file("[Interface]\nWSListen = http://x\n", false));
}

static void test_config_parse_wslisten_absent_leaves_flag_unset(void)
{
	struct wgdevice *d = parse_file("[Interface]\nListenPort = 51820\n", false);

	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_FALSE(d->flags & WGDEVICE_HAS_WS_LISTEN);
	TEST_ASSERT_NULL(d->ws_listen);
	free_wgdevice(d);
}

static void test_config_parse_endpoint_ws_url(void)
{
	struct wgdevice *d = parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://h:443/wg\nWSMode = standard\n", false);

	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_EQUAL_STRING("wss://h:443/wg", d->first_peer->endpoint_url);
	TEST_ASSERT_EQUAL_INT(0, d->first_peer->endpoint.addr.sa_family);
	free_wgdevice(d);
}

static void test_config_parse_endpoint_udp_unchanged(void)
{
	struct wgdevice *d = parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 1.2.3.4:51820\n", false);

	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_NULL(d->first_peer->endpoint_url);
	TEST_ASSERT_EQUAL_INT(AF_INET, d->first_peer->endpoint.addr.sa_family);
	free_wgdevice(d);
}

static void test_config_parse_wsmode_enum(void)
{
	struct wgdevice *d;

	d = parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://h:443\nWSMode = wstunnel\nWSTunnelTarget = 127.0.0.1:51820\n", false);
	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_EQUAL_STRING("wstunnel", d->first_peer->ws_mode);
	free_wgdevice(d);

	TEST_ASSERT_NULL(parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://h:443\nWSMode = bogus\n", false));
}

struct ws_url_case { const char *v; bool want; };

static void test_is_ws_url_variants(void)
{
	static const struct ws_url_case cases[] = {
		{ "ws://h", true }, { "wss://h", true },
		{ "WS://h", false }, { "WSS://h", false }, { "ws:/", false },
		{ "wss:", false }, { "", false }, { "http://h", false },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
		TEST_ASSERT_EQUAL_MESSAGE(cases[i].want, is_ws_url(cases[i].v), cases[i].v);
}

struct target_case { const char *v; bool want_ok; };

static void test_config_parse_wstunnel_target_form(void)
{
	static const struct target_case cases[] = {
		{ "h:1234", true }, { "[::1]:1234", true },
		{ "hostonly", false }, { ":p", false }, { "h:", false },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		char *out = NULL;
		bool ok = parse_wstunnel_target(&out, cases[i].v);

		TEST_ASSERT_EQUAL_MESSAGE(cases[i].want_ok, ok, cases[i].v);
		free(out);
	}
}

static void test_config_parse_ws_bearer_empty_rejected(void)
{
	const char *argv[] = { "peer", PUBKEY, "ws-bearer", "" };

	/* config-file form: rejected before parse_ws_bearer (get_value drops empty values) */
	TEST_ASSERT_NULL(parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://h:443\nWSMode = standard\nWSPeerBearer =\n", false));
	/* CLI form: exercises parse_ws_bearer's empty-token rejection (message echoes no value) */
	TEST_ASSERT_NULL(config_read_cmd(argv, sizeof(argv) / sizeof(argv[0])));
}

static void test_config_finish_wstunnel_requires_target(void)
{
	TEST_ASSERT_NULL(parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://h:443\nWSMode = wstunnel\n", false));
}

static void test_config_finish_wskeys_require_ws_endpoint(void)
{
	TEST_ASSERT_NULL(parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 1.2.3.4:51820\nWSMode = standard\n", false));
}

static void test_config_finish_ws_endpoint_requires_mode(void)
{
	TEST_ASSERT_NULL(parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = wss://h:443/wg\n", false));
}

static void test_config_finish_rejects_double_endpoint(void)
{
	TEST_ASSERT_NULL(parse_file("[Peer]\nPublicKey = " PUBKEY "\nEndpoint = 1.2.3.4:51820\nEndpoint = wss://h:443\nWSMode = standard\n", false));
}

static void test_config_cmd_ws_tokens_parity(void)
{
	const char *argv[] = { "peer", PUBKEY, "endpoint", "wss://h:443", "ws-mode", "wstunnel",
			       "wstunnel-target", "127.0.0.1:51820", "ws-bearer", "tok" };
	struct wgdevice *d = config_read_cmd(argv, sizeof(argv) / sizeof(argv[0]));

	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_EQUAL_STRING("wss://h:443", d->first_peer->endpoint_url);
	TEST_ASSERT_EQUAL_STRING("wstunnel", d->first_peer->ws_mode);
	TEST_ASSERT_EQUAL_STRING("127.0.0.1:51820", d->first_peer->wstunnel_target);
	TEST_ASSERT_EQUAL_STRING("tok", d->first_peer->ws_bearer);
	free_wgdevice(d);
}

static void test_config_cmd_wslisten_rejected_after_peer(void)
{
	const char *argv[] = { "peer", PUBKEY, "ws-listen", "wss://h:443" };

	TEST_ASSERT_NULL(config_read_cmd(argv, sizeof(argv) / sizeof(argv[0])));
}

static void test_config_cmd_ws_endpoint_requires_mode(void)
{
	const char *argv[] = { "peer", PUBKEY, "endpoint", "wss://h:443" };

	TEST_ASSERT_NULL(config_read_cmd(argv, sizeof(argv) / sizeof(argv[0])));
}

static void test_config_cmd_incremental_ws_attr_ok(void)
{
	const char *argv[] = { "peer", PUBKEY, "ws-bearer", "tok" };
	struct wgdevice *d = config_read_cmd(argv, sizeof(argv) / sizeof(argv[0]));

	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_EQUAL_STRING("tok", d->first_peer->ws_bearer);
	TEST_ASSERT_NULL(d->first_peer->endpoint_url);
	free_wgdevice(d);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_config_parse_wslisten_valid);
	RUN_TEST(test_config_parse_wslisten_empty_disables);
	RUN_TEST(test_config_parse_wslisten_bad_scheme);
	RUN_TEST(test_config_parse_wslisten_absent_leaves_flag_unset);
	RUN_TEST(test_config_parse_endpoint_ws_url);
	RUN_TEST(test_config_parse_endpoint_udp_unchanged);
	RUN_TEST(test_config_parse_wsmode_enum);
	RUN_TEST(test_is_ws_url_variants);
	RUN_TEST(test_config_parse_wstunnel_target_form);
	RUN_TEST(test_config_parse_ws_bearer_empty_rejected);
	RUN_TEST(test_config_finish_wstunnel_requires_target);
	RUN_TEST(test_config_finish_wskeys_require_ws_endpoint);
	RUN_TEST(test_config_finish_ws_endpoint_requires_mode);
	RUN_TEST(test_config_finish_rejects_double_endpoint);
	RUN_TEST(test_config_cmd_ws_tokens_parity);
	RUN_TEST(test_config_cmd_wslisten_rejected_after_peer);
	RUN_TEST(test_config_cmd_ws_endpoint_requires_mode);
	RUN_TEST(test_config_cmd_incremental_ws_attr_ok);
	return UNITY_END();
}
