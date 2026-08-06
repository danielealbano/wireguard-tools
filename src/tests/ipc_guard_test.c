// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include "../curve25519.c"
#include "../ipc.c" /* keeps the kernel backend so IPC_SUPPORTS_KERNEL_INTERFACE + the guard exist */
#include "../encoding.c"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "unity.h"

const char *PROG_NAME = "wg";

void setUp(void) {}
void tearDown(void) {}

#ifdef IPC_SUPPORTS_KERNEL_INTERFACE
static void test_ipc_device_has_ws_settings_detects_each(void)
{
	struct wgdevice dev = { 0 };
	struct wgpeer peer = { 0 };

	dev.first_peer = dev.last_peer = &peer;
	TEST_ASSERT_FALSE(device_has_ws_settings(&dev));
	dev.flags |= WGDEVICE_HAS_WS_LISTEN;
	TEST_ASSERT_TRUE(device_has_ws_settings(&dev));
	dev.flags = 0;
	peer.endpoint_url = (char *)"x"; TEST_ASSERT_TRUE(device_has_ws_settings(&dev)); peer.endpoint_url = NULL;
	peer.ws_mode = (char *)"x"; TEST_ASSERT_TRUE(device_has_ws_settings(&dev)); peer.ws_mode = NULL;
	peer.wstunnel_target = (char *)"x"; TEST_ASSERT_TRUE(device_has_ws_settings(&dev)); peer.wstunnel_target = NULL;
	peer.ws_bearer = (char *)"x"; TEST_ASSERT_TRUE(device_has_ws_settings(&dev)); peer.ws_bearer = NULL;
}

static void test_ipc_device_has_ws_settings_false_for_udp(void)
{
	struct wgdevice dev = { 0 };
	struct wgpeer peer = { 0 };

	dev.first_peer = dev.last_peer = &peer;
	dev.flags |= WGDEVICE_HAS_LISTEN_PORT | WGDEVICE_HAS_FWMARK;
	peer.endpoint.addr4.sin_family = AF_INET;
	TEST_ASSERT_FALSE(device_has_ws_settings(&dev));
}

static void test_ipc_set_device_rejects_ws_on_kernel(void)
{
	struct wgdevice dev = { 0 };
	char buf[256] = { 0 };
	FILE *f;
	int ret;

	/* No socket exists for this name, so userspace_has_wireguard_interface() is
	 * false and the kernel path is selected — where the guard must fire. */
	strncpy(dev.name, "wgnope0", IFNAMSIZ - 1);
	dev.flags |= WGDEVICE_HAS_WS_LISTEN;
	f = freopen("ipc_guard_stderr.tmp", "w+", stderr);
	TEST_ASSERT_NOT_NULL(f);
	ret = ipc_set_device(&dev);
	fflush(stderr);
	rewind(f);
	TEST_ASSERT_TRUE(fread(buf, 1, sizeof(buf) - 1, f) > 0);
	remove("ipc_guard_stderr.tmp");
	TEST_ASSERT_TRUE(ret != 0);
	TEST_ASSERT_NOT_NULL(strstr(buf, "userspace"));
}
#endif

int main(void)
{
	UNITY_BEGIN();
#ifdef IPC_SUPPORTS_KERNEL_INTERFACE
	RUN_TEST(test_ipc_device_has_ws_settings_detects_each);
	RUN_TEST(test_ipc_device_has_ws_settings_false_for_udp);
	RUN_TEST(test_ipc_set_device_rejects_ws_on_kernel);
#endif
	return UNITY_END();
}
