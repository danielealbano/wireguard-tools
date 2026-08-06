/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifndef ENCODING_H
#define ENCODING_H

#include <stdbool.h>
#include <stdint.h>
#include "containers.h"

#define WG_KEY_LEN_BASE64 ((((WG_KEY_LEN) + 2) / 3) * 4 + 1)
#define WG_KEY_LEN_HEX (WG_KEY_LEN * 2 + 1)

void key_to_base64(char base64[static WG_KEY_LEN_BASE64], const uint8_t key[static WG_KEY_LEN]);
bool key_from_base64(uint8_t key[static WG_KEY_LEN], const char *base64);

void key_to_hex(char hex[static WG_KEY_LEN_HEX], const uint8_t key[static WG_KEY_LEN]);
bool key_from_hex(uint8_t key[static WG_KEY_LEN], const char *hex);

bool key_is_zero(const uint8_t key[static WG_KEY_LEN]);

/* Case-sensitive ws:// or wss:// prefix (mirrors wireguard-go device/uapi.go).
 * Implemented with explicit character comparisons so this header needs no <string.h>;
 * && short-circuits at the first mismatch/NUL, so every read is in-bounds. */
static inline bool is_ws_url(const char *v)
{
	return (v[0] == 'w' && v[1] == 's' && v[2] == ':' && v[3] == '/' && v[4] == '/') ||
	       (v[0] == 'w' && v[1] == 's' && v[2] == 's' && v[3] == ':' && v[4] == '/' && v[5] == '/');
}

#endif
