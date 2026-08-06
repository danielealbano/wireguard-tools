// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "containers.h"
#include "ipc.h"

struct string_list {
	char *buffer;
	size_t len;
	size_t cap;
};

static int string_list_add(struct string_list *list, const char *str)
{
	size_t len = strlen(str) + 1;

	if (len == 1)
		return 0;

	if (len >= list->cap - list->len) {
		char *new_buffer;
		size_t new_cap = list->cap * 2;

		if (new_cap < list->len + len + 1)
			new_cap = list->len + len + 1;
		new_buffer = realloc(list->buffer, new_cap);
		if (!new_buffer)
			return -errno;
		list->buffer = new_buffer;
		list->cap = new_cap;
	}
	memcpy(list->buffer + list->len, str, len);
	list->len += len;
	list->buffer[list->len] = '\0';
	return 0;
}

#include "ipc-uapi.h"
#if defined(__linux__)
#include "ipc-linux.h"
#elif defined(__OpenBSD__)
#include "ipc-openbsd.h"
#elif defined(__FreeBSD__)
#include "ipc-freebsd.h"
#elif defined(_WIN32)
#include "ipc-windows.h"
#endif

/* first\0second\0third\0forth\0last\0\0 */
char *ipc_list_devices(void)
{
	struct string_list list = { 0 };
	int ret;

#ifdef IPC_SUPPORTS_KERNEL_INTERFACE
	ret = kernel_get_wireguard_interfaces(&list);
	if (ret < 0)
		goto cleanup;
#endif
	ret = userspace_get_wireguard_interfaces(&list);
	if (ret < 0)
		goto cleanup;

cleanup:
	errno = -ret;
	if (errno) {
		free(list.buffer);
		return NULL;
	}
	return list.buffer ?: strdup("\0");
}

int ipc_get_device(struct wgdevice **dev, const char *iface)
{
#ifdef IPC_SUPPORTS_KERNEL_INTERFACE
	if (userspace_has_wireguard_interface(iface))
		return userspace_get_device(dev, iface);
	return kernel_get_device(dev, iface);
#else
	return userspace_get_device(dev, iface);
#endif
}

#ifdef IPC_SUPPORTS_KERNEL_INTERFACE
/* WebSocket/wstunnel settings are meaningful only on the userspace backend.
 * Defined inside the kernel-interface guard so it is not an unused static on
 * userspace-only builds (e.g. macOS/darwin). */
static bool device_has_ws_settings(const struct wgdevice *dev)
{
	struct wgpeer *peer;

	if (dev->flags & WGDEVICE_HAS_WS_LISTEN)
		return true;
	for_each_wgpeer(dev, peer) {
		if (peer->endpoint_url || peer->ws_mode || peer->wstunnel_target || peer->ws_bearer)
			return true;
	}
	return false;
}
#endif

int ipc_set_device(struct wgdevice *dev)
{
#ifdef IPC_SUPPORTS_KERNEL_INTERFACE
	if (userspace_has_wireguard_interface(dev->name))
		return userspace_set_device(dev);
	if (device_has_ws_settings(dev)) {
		(void) fprintf(stderr, "WebSocket settings require a userspace WireGuard implementation; `%s' is a kernel interface\n", dev->name);
		errno = EOPNOTSUPP;
		return -errno;
	}
	return kernel_set_device(dev);
#else
	return userspace_set_device(dev);
#endif
}
