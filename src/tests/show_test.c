// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * Verifies `wg show` rendering: machine outputs keep endpoint=ip:port and never print the bearer;
 * the human output adds the transport and ws-url lines.
 */

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "unity.h"
#include "../encoding.c"
#include "../terminal.c"
#include "../show.c"

/* Symbols referenced by show_main but not exercised by these unit tests. */
const char *PROG_NAME = "wg";
int ipc_get_device(struct wgdevice **dev, const char *iface) { (void)dev; (void)iface; errno = ENOTSUP; return -ENOTSUP; }
char *ipc_list_devices(void) { return NULL; }

void setUp(void) {}
void tearDown(void) {}

static struct wgdevice *g_dev;

static char capture_buf[8192];
static const char *capture(void (*fn)(void))
{
	int saved = dup(STDOUT_FILENO);
	FILE *tmp = tmpfile();
	long n;

	TEST_ASSERT_TRUE(saved >= 0);
	TEST_ASSERT_NOT_NULL(tmp);
	fflush(stdout);
	dup2(fileno(tmp), STDOUT_FILENO);
	fn();
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
	rewind(tmp);
	n = (long)fread(capture_buf, 1, sizeof capture_buf - 1, tmp);
	capture_buf[n < 0 ? 0 : n] = '\0';
	fclose(tmp);
	return capture_buf;
}

static void do_pretty(void) { pretty_print(g_dev); }
static void do_dump(void) { dump_print(g_dev, true); }

static struct wgdevice *ws_device(void)
{
	struct wgdevice *dev = calloc(1, sizeof(*dev));
	struct wgpeer *peer = calloc(1, sizeof(*peer));

	TEST_ASSERT_NOT_NULL(dev);
	TEST_ASSERT_NOT_NULL(peer);
	strncpy(dev->name, "wsshow", IFNAMSIZ - 1);
	dev->first_peer = dev->last_peer = peer;
	peer->flags = WGPEER_HAS_PUBLIC_KEY | WGPEER_HAS_TRANSPORT;
	peer->transport = WGPEER_TRANSPORT_WEBSOCKET;
	peer->ws_url = strdup("wss://127.0.0.1:8443/wg");
	peer->ws_bearer = strdup("supersecretbearer");
	peer->endpoint.addr4.sin_family = AF_INET;
	peer->endpoint.addr4.sin_port = htons(8443);
	inet_pton(AF_INET, "127.0.0.1", &peer->endpoint.addr4.sin_addr);
	return dev;
}

static void test_show_pretty_adds_transport_and_url(void)
{
	const char *out;

	g_dev = ws_device();
	out = capture(do_pretty);
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "websocket"), "transport line");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "wss://127.0.0.1:8443/wg"), "ws url line");
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "127.0.0.1:8443"), "endpoint ip:port");
	TEST_ASSERT_NULL_MESSAGE(strstr(out, "supersecretbearer"), "bearer must never be printed");
	free_wgdevice(g_dev);
}

static void test_show_dump_keeps_ip_endpoint_hides_bearer(void)
{
	const char *out;

	g_dev = ws_device();
	out = capture(do_dump);
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "127.0.0.1:8443"), "dump endpoint stays ip:port");
	TEST_ASSERT_NULL_MESSAGE(strstr(out, "wss://"), "dump does not print the ws url");
	TEST_ASSERT_NULL_MESSAGE(strstr(out, "supersecretbearer"), "dump never prints the bearer");
	free_wgdevice(g_dev);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_show_pretty_adds_transport_and_url);
	RUN_TEST(test_show_dump_keeps_ip_endpoint_hides_bearer);
	return UNITY_END();
}
