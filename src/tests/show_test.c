// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include "../show.c"
#include "../terminal.c"
#include "../encoding.c"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"

const char *PROG_NAME = "wg";

/* Link-time stubs for the SUT's external dependencies. */
static struct wgdevice *g_dev;
int ipc_get_device(struct wgdevice **dev, const char *iface)
{
	(void)iface;
	*dev = g_dev;
	return 0;
}
char *ipc_list_devices(void)
{
	return NULL; /* never reached for a specific-interface show */
}

void setUp(void)
{
	setenv("WG_COLOR_MODE", "never", 1); /* deterministic plain output */
}
void tearDown(void) {}

/* show_main takes ownership of g_dev (frees it). */
static void capture_show(struct wgdevice *dev, int argc, const char *argv[], char *out, size_t outlen)
{
	FILE *tmp = tmpfile();
	int saved;

	TEST_ASSERT_NOT_NULL(tmp);
	g_dev = dev;
	fflush(stdout);
	saved = dup(fileno(stdout));
	TEST_ASSERT_TRUE(saved >= 0);
	TEST_ASSERT_TRUE(dup2(fileno(tmp), fileno(stdout)) >= 0);
	show_main(argc, argv);
	fflush(stdout);
	TEST_ASSERT_TRUE(dup2(saved, fileno(stdout)) >= 0);
	close(saved);
	rewind(tmp);
	memset(out, 0, outlen);
	TEST_ASSERT_TRUE(fread(out, 1, outlen - 1, tmp) > 0);
	fclose(tmp);
}

static struct wgdevice *build_ws_dev(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(peer);
	peer->flags |= WGPEER_HAS_PUBLIC_KEY;
	peer->endpoint_url = strdup("wss://h:443");
	peer->ws_mode = strdup("wstunnel");
	peer->wstunnel_target = strdup("127.0.0.1:51820");
	peer->ws_bearer = strdup("SECRETTOK123");
	dev->first_peer = dev->last_peer = peer;
	return dev;
}

static void test_show_dump_url_endpoint_same_columns(void)
{
	const char *argv[] = { "show", "wg0", "dump" };
	char out[4096];

	capture_show(build_ws_dev(), 3, argv, out, sizeof(out));
	TEST_ASSERT_NOT_NULL(strstr(out, "wss://h:443\t")); /* URL fills the endpoint column (URL + column tab) */
	TEST_ASSERT_NOT_NULL(strstr(out, "off\n"));          /* trailing keepalive field still present */
}

static void test_show_pretty_url_endpoint(void)
{
	const char *argv[] = { "show", "wg0" };
	char out[4096];

	capture_show(build_ws_dev(), 2, argv, out, sizeof(out));
	TEST_ASSERT_NOT_NULL(strstr(out, "wss://h:443"));
}

static void test_show_endpoints_url(void)
{
	const char *argv[] = { "show", "wg0", "endpoints" };
	char out[4096];

	capture_show(build_ws_dev(), 3, argv, out, sizeof(out));
	TEST_ASSERT_NOT_NULL(strstr(out, "wss://h:443"));
}

static void test_show_bearer_absent(void)
{
	const char *dump_argv[] = { "show", "wg0", "dump" };
	const char *pretty_argv[] = { "show", "wg0" };
	char out[4096];

	capture_show(build_ws_dev(), 3, dump_argv, out, sizeof(out));
	TEST_ASSERT_NULL(strstr(out, "SECRETTOK123"));
	capture_show(build_ws_dev(), 2, pretty_argv, out, sizeof(out));
	TEST_ASSERT_NULL(strstr(out, "SECRETTOK123"));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_show_dump_url_endpoint_same_columns);
	RUN_TEST(test_show_pretty_url_endpoint);
	RUN_TEST(test_show_endpoints_url);
	RUN_TEST(test_show_bearer_absent);
	return UNITY_END();
}
