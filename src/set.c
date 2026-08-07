// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "containers.h"
#include "config.h"
#include "ipc.h"
#include "subcommands.h"

int set_main(int argc, const char *argv[])
{
	struct wgdevice *device = NULL;
	int ret = 1;

	if (argc < 3) {
		(void) fprintf(stderr, "Usage: %s %s <interface> [listen-port <port>] [fwmark <mark>] [private-key <file path>] [ws-listen <ws-url>] [ws-server-tls-cert <file path>] [ws-server-tls-key <file path>] [ws-server-bearer <file path>] [ws-trusted-proxies <cidr,...>] [peer <base64 public key> [remove] [preshared-key <file path>] [endpoint <ip>:<port>|<ws-url>] [ws-mode <websocket|wstunnel>] [wstunnel-target <host:port>] [ws-bearer <file path>] [ws-mask <true|false>] [ws-tls-ca <file path>] [ws-tls-cert <file path>] [ws-tls-key <file path>] [ws-tls-insecure <true|false>] [ws-ping-interval <ms>] [ws-backoff-min <ms>] [ws-backoff-max <ms>] [persistent-keepalive <interval seconds>] [allowed-ips [+|-]<ip1>/<cidr1>[,[+|-]<ip2>/<cidr2>]...] ]...\n", PROG_NAME, argv[0]);
		return 1;
	}

	device = config_read_cmd(argv + 2, argc - 2);
	if (!device)
		goto cleanup;
	strncpy(device->name, argv[1], IFNAMSIZ -  1);
	device->name[IFNAMSIZ - 1] = '\0';

	if (ipc_set_device(device) != 0) {
		perror("Unable to modify interface");
		goto cleanup;
	}

	ret = 0;

cleanup:
	free_wgdevice(device);
	return ret;
}
