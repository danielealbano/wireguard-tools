/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifndef CONTAINERS_H
#define CONTAINERS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#if defined(__linux__)
#include <linux/wireguard.h>
#elif defined(__OpenBSD__)
#include <net/if_wg.h>
#endif

#ifndef WG_KEY_LEN
#define WG_KEY_LEN 32
#endif

/* Cross platform __kernel_timespec */
struct timespec64 {
	int64_t tv_sec;
	int64_t tv_nsec;
};

enum {
	WGALLOWEDIP_REMOVE_ME = 1U << 0,
};

struct wgallowedip {
	uint16_t family;
	union {
		struct in_addr ip4;
		struct in6_addr ip6;
	};
	uint8_t cidr;
	uint32_t flags;
	struct wgallowedip *next_allowedip;
};

/* Per-peer carrier, emitted as the mandatory transport= UAPI key. */
enum wgpeer_transport {
	WGPEER_TRANSPORT_UDP = 0,
	WGPEER_TRANSPORT_WEBSOCKET,
	WGPEER_TRANSPORT_WSTUNNEL
};

enum {
	WGPEER_REMOVE_ME = 1U << 0,
	WGPEER_REPLACE_ALLOWEDIPS = 1U << 1,
	WGPEER_HAS_PUBLIC_KEY = 1U << 2,
	WGPEER_HAS_PRESHARED_KEY = 1U << 3,
	WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL = 1U << 4,
	/* Gates the transport= UAPI line: an incremental `wg set` that does not set it
	 * omits transport=, so the daemon keeps the peer's persisted value. */
	WGPEER_HAS_TRANSPORT = 1U << 5,
	/* Set by any per-peer ws-setting key parse, so a WebSocket setting on a UDP peer is
	 * rejected even when its value is false/0 (WSMask=false, WSPingInterval=0, ...). */
	WGPEER_HAS_WS_SETTINGS = 1U << 6
};

struct wgpeer {
	uint32_t flags;

	uint8_t public_key[WG_KEY_LEN];
	uint8_t preshared_key[WG_KEY_LEN];

	union {
		struct sockaddr addr;
		struct sockaddr_in addr4;
		struct sockaddr_in6 addr6;
	} endpoint;

	struct timespec64 last_handshake_time;
	uint64_t rx_bytes, tx_bytes;
	uint16_t persistent_keepalive_interval;

	enum wgpeer_transport transport;      /* emitted as transport=udp|websocket|wstunnel */
	char *ws_url;                         /* ws(s)://host:port/path; NULL for udp / inbound */
	char *wstunnel_target;                /* host:port; wstunnel dialing only */
	char *ws_bearer;                      /* secret; never shown by show/dump */
	char *ws_tls_ca, *ws_tls_cert, *ws_tls_key;  /* file paths */
	uint32_t ws_ping_interval_ms, ws_backoff_min_ms, ws_backoff_max_ms; /* 0 => default */
	bool ws_mask;
	bool ws_tls_insecure;

	struct wgallowedip *first_allowedip, *last_allowedip;
	struct wgpeer *next_peer;
};

enum {
	WGDEVICE_REPLACE_PEERS = 1U << 0,
	WGDEVICE_HAS_PRIVATE_KEY = 1U << 1,
	WGDEVICE_HAS_PUBLIC_KEY = 1U << 2,
	WGDEVICE_HAS_LISTEN_PORT = 1U << 3,
	WGDEVICE_HAS_FWMARK = 1U << 4,
	/* Each device WebSocket key gets a HAS_* flag set ONLY when the key is present (or read
	 * back by get), so emission is gated on it and a plain UDP config emits no ws_* keys.
	 * These are deliberately NOT default-set in config_read_init (unlike listen_port/fwmark)
	 * so a non-WebSocket setconf never emits fork-only keys. To clear one, set it explicitly
	 * empty (e.g. WSListen=), which emits key= (empty); the daemon accepts empty as a clear. */
	WGDEVICE_HAS_WS_LISTEN = 1U << 5,
	WGDEVICE_HAS_WS_SERVER_TLS_CERT = 1U << 6,
	WGDEVICE_HAS_WS_SERVER_TLS_KEY = 1U << 7,
	WGDEVICE_HAS_WS_SERVER_BEARER = 1U << 8,
	WGDEVICE_HAS_WS_TRUSTED_PROXIES = 1U << 9
};

struct wgdevice {
	char name[IFNAMSIZ];
	uint32_t ifindex;

	uint32_t flags;

	uint8_t public_key[WG_KEY_LEN];
	uint8_t private_key[WG_KEY_LEN];

	uint32_t fwmark;
	uint16_t listen_port;

	char *ws_listen;                      /* ws(s)://host:port/path; gated by WGDEVICE_HAS_WS_LISTEN */
	char *ws_server_tls_cert, *ws_server_tls_key;
	char *ws_server_bearer;               /* secret; never shown by show/dump */
	char *ws_trusted_proxies;             /* comma-separated CIDRs, passed through verbatim */

	struct wgpeer *first_peer, *last_peer;
};

#define for_each_wgpeer(__dev, __peer) for ((__peer) = (__dev)->first_peer; (__peer); (__peer) = (__peer)->next_peer)
#define for_each_wgallowedip(__peer, __allowedip) for ((__allowedip) = (__peer)->first_allowedip; (__allowedip); (__allowedip) = (__allowedip)->next_allowedip)

static inline void free_wgdevice(struct wgdevice *dev)
{
	if (!dev)
		return;
	for (struct wgpeer *peer = dev->first_peer, *np = peer ? peer->next_peer : NULL; peer; peer = np, np = peer ? peer->next_peer : NULL) {
		for (struct wgallowedip *allowedip = peer->first_allowedip, *na = allowedip ? allowedip->next_allowedip : NULL; allowedip; allowedip = na, na = allowedip ? allowedip->next_allowedip : NULL)
			free(allowedip);
		free(peer->ws_url);
		free(peer->wstunnel_target);
		free(peer->ws_bearer);
		free(peer->ws_tls_ca);
		free(peer->ws_tls_cert);
		free(peer->ws_tls_key);
		free(peer);
	}
	free(dev->ws_listen);
	free(dev->ws_server_tls_cert);
	free(dev->ws_server_tls_key);
	free(dev->ws_server_bearer);
	free(dev->ws_trusted_proxies);
	free(dev);
}

#endif
