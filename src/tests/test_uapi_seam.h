/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * In-process fake UAPI endpoint: a test-owned UNIX socket at
 * RUNSTATEDIR/wireguard/<iface>.sock. A helper thread accepts one connection,
 * captures everything the SUT writes, and replies with a caller-supplied
 * response — driving the real userspace_set_device/userspace_get_device over
 * the stock userspace_interface_file() (no libc/macro hacks).
 */
#ifndef TEST_UAPI_SEAM_H
#define TEST_UAPI_SEAM_H
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "unity.h"

struct seam { char path[108]; char captured[1 << 16]; const char *reply; int lfd; pthread_t th; };

static void *seam_serve(void *arg)
{
	struct seam *s = arg;
	int c = accept(s->lfd, NULL, NULL);
	size_t n = 0; ssize_t r;
	if (c < 0) return NULL;
	while (n < sizeof(s->captured) - 1 && (r = read(c, s->captured + n, sizeof(s->captured) - 1 - n)) > 0) {
		n += (size_t)r;
		if (n >= 2 && s->captured[n - 1] == '\n' && s->captured[n - 2] == '\n') break; /* SUT's trailing blank line */
	}
	s->captured[n] = '\0';
	if (s->reply) { size_t w = 0, len = strlen(s->reply); while (w < len) { r = write(c, s->reply + w, len - w); if (r <= 0) break; w += (size_t)r; } }
	close(c);
	return NULL;
}

/* Start the fake for <iface>; <reply> is the bytes to send back (e.g. "errno=0\n\n" for set). */
static void seam_start(struct seam *s, const char *iface, const char *reply)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	memset(s->captured, 0, sizeof(s->captured));
	s->reply = reply;
	TEST_ASSERT_TRUE(mkdir(RUNSTATEDIR, 0700) == 0 || errno == EEXIST);
	TEST_ASSERT_TRUE(mkdir(RUNSTATEDIR "/wireguard", 0700) == 0 || errno == EEXIST);
	TEST_ASSERT_TRUE(snprintf(addr.sun_path, sizeof(addr.sun_path), RUNSTATEDIR "/wireguard/%s.sock", iface) > 0);
	strncpy(s->path, addr.sun_path, sizeof(s->path) - 1); s->path[sizeof(s->path) - 1] = '\0';
	unlink(addr.sun_path);
	TEST_ASSERT_TRUE((s->lfd = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
	TEST_ASSERT_EQUAL_INT(0, bind(s->lfd, (struct sockaddr *)&addr, sizeof(addr)));
	TEST_ASSERT_EQUAL_INT(0, listen(s->lfd, 1));
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&s->th, NULL, seam_serve, s));
}

static void seam_stop(struct seam *s)
{
	pthread_join(s->th, NULL);
	close(s->lfd);
	unlink(s->path);
}
#endif
