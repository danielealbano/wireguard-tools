// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "containers.h"
#include "curve25519.h"
#include "encoding.h"
#include "ctype.h"

#ifdef _WIN32
#include "ipc-uapi-windows.h"
#else
#include "ipc-uapi-unix.h"
#endif

static int userspace_set_device(struct wgdevice *dev)
{
	char hex[WG_KEY_LEN_HEX], ip[INET6_ADDRSTRLEN], host[4096 + 1], service[512 + 1];
	struct wgpeer *peer;
	struct wgallowedip *allowedip;
	FILE *f;
	int ret, set_errno = -EPROTO;
	socklen_t addr_len;
	size_t line_buffer_len = 0, line_len;
	char *key = NULL, *value;

	f = userspace_interface_file(dev->name);
	if (!f)
		return -errno;
	(void) fprintf(f, "set=1\n");

	if (dev->flags & WGDEVICE_HAS_PRIVATE_KEY) {
		key_to_hex(hex, dev->private_key);
		(void) fprintf(f, "private_key=%s\n", hex);
	}
	if (dev->flags & WGDEVICE_HAS_LISTEN_PORT)
		(void) fprintf(f, "listen_port=%u\n", dev->listen_port);
	if (dev->flags & WGDEVICE_HAS_FWMARK)
		(void) fprintf(f, "fwmark=%u\n", dev->fwmark);
	/* Device WebSocket keys are gated on their HAS_* flag (set only when the key was present in
	 * config or read by get, never default-set) and emitted value-or-empty, so an explicit empty
	 * value clears the daemon's key while a plain UDP config emits nothing here. */
	if (dev->flags & WGDEVICE_HAS_WS_LISTEN)
		(void) fprintf(f, "ws_listen=%s\n", dev->ws_listen ? dev->ws_listen : "");
	if (dev->flags & WGDEVICE_HAS_WS_SERVER_TLS_CERT)
		(void) fprintf(f, "ws_server_tls_cert=%s\n", dev->ws_server_tls_cert ? dev->ws_server_tls_cert : "");
	if (dev->flags & WGDEVICE_HAS_WS_SERVER_TLS_KEY)
		(void) fprintf(f, "ws_server_tls_key=%s\n", dev->ws_server_tls_key ? dev->ws_server_tls_key : "");
	if (dev->flags & WGDEVICE_HAS_WS_SERVER_BEARER)
		(void) fprintf(f, "ws_server_bearer=%s\n", dev->ws_server_bearer ? dev->ws_server_bearer : "");
	if (dev->flags & WGDEVICE_HAS_WS_TRUSTED_PROXIES)
		(void) fprintf(f, "ws_trusted_proxies=%s\n", dev->ws_trusted_proxies ? dev->ws_trusted_proxies : "");
	if (dev->flags & WGDEVICE_REPLACE_PEERS)
		(void) fprintf(f, "replace_peers=true\n");

	for_each_wgpeer(dev, peer) {
		key_to_hex(hex, peer->public_key);
		(void) fprintf(f, "public_key=%s\n", hex);
		if (peer->flags & WGPEER_REMOVE_ME) {
			(void) fprintf(f, "remove=true\n");
			continue;
		}
		if (peer->flags & WGPEER_HAS_PRESHARED_KEY) {
			key_to_hex(hex, peer->preshared_key);
			(void) fprintf(f, "preshared_key=%s\n", hex);
		}
		/* transport= is emitted only when this operation declares it (an incremental `wg set`
		 * that omits it keeps the daemon's persisted transport). */
		if (peer->flags & WGPEER_HAS_TRANSPORT)
			(void) fprintf(f, "transport=%s\n",
				       peer->transport == WGPEER_TRANSPORT_WSTUNNEL ? "wstunnel" :
				       peer->transport == WGPEER_TRANSPORT_WEBSOCKET ? "websocket" : "udp");
		if (peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6) {
			addr_len = 0;
			if (peer->endpoint.addr.sa_family == AF_INET)
				addr_len = sizeof(struct sockaddr_in);
			else if (peer->endpoint.addr.sa_family == AF_INET6)
				addr_len = sizeof(struct sockaddr_in6);
			if (!getnameinfo(&peer->endpoint.addr, addr_len, host, sizeof(host), service, sizeof(service), NI_DGRAM | NI_NUMERICSERV | NI_NUMERICHOST)) {
				if (peer->endpoint.addr.sa_family == AF_INET6 && strchr(host, ':'))
					(void) fprintf(f, "endpoint=[%s]:%s\n", host, service);
				else
					(void) fprintf(f, "endpoint=%s:%s\n", host, service);
			}
		}
		/* Emit the ws_* keys for a WebSocket peer, or for an incremental `wg set` that sets a ws
		 * setting without (re)declaring transport (so the daemon validates it against the peer's
		 * persisted transport). ws_mask/ws_tls_insecure are emitted only when true, matching the
		 * daemon's own get=1. */
		if (peer->transport != WGPEER_TRANSPORT_UDP ||
		    ((peer->flags & WGPEER_HAS_WS_SETTINGS) && !(peer->flags & WGPEER_HAS_TRANSPORT))) {
			if (peer->ws_url)
				(void) fprintf(f, "ws_url=%s\n", peer->ws_url);
			if (peer->wstunnel_target)
				(void) fprintf(f, "wstunnel_target=%s\n", peer->wstunnel_target);
			if (peer->ws_bearer)
				(void) fprintf(f, "ws_bearer=%s\n", peer->ws_bearer);
			if (peer->ws_mask)
				(void) fprintf(f, "ws_mask=true\n");
			if (peer->ws_tls_ca)
				(void) fprintf(f, "ws_tls_ca=%s\n", peer->ws_tls_ca);
			if (peer->ws_tls_cert)
				(void) fprintf(f, "ws_tls_cert=%s\n", peer->ws_tls_cert);
			if (peer->ws_tls_key)
				(void) fprintf(f, "ws_tls_key=%s\n", peer->ws_tls_key);
			if (peer->ws_tls_insecure)
				(void) fprintf(f, "ws_tls_insecure=true\n");
			if (peer->ws_ping_interval_ms)
				(void) fprintf(f, "ws_ping_interval=%u\n", peer->ws_ping_interval_ms);
			if (peer->ws_backoff_min_ms)
				(void) fprintf(f, "ws_backoff_min=%u\n", peer->ws_backoff_min_ms);
			if (peer->ws_backoff_max_ms)
				(void) fprintf(f, "ws_backoff_max=%u\n", peer->ws_backoff_max_ms);
		}
		if (peer->flags & WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL)
			(void) fprintf(f, "persistent_keepalive_interval=%u\n", peer->persistent_keepalive_interval);
		if (peer->flags & WGPEER_REPLACE_ALLOWEDIPS)
			(void) fprintf(f, "replace_allowed_ips=true\n");
		for_each_wgallowedip(peer, allowedip) {
			if (allowedip->family == AF_INET) {
				if (!inet_ntop(AF_INET, &allowedip->ip4, ip, INET6_ADDRSTRLEN))
					continue;
			} else if (allowedip->family == AF_INET6) {
				if (!inet_ntop(AF_INET6, &allowedip->ip6, ip, INET6_ADDRSTRLEN))
					continue;
			} else
				continue;
			(void) fprintf(f, "allowed_ip=%s%s/%d\n", (allowedip->flags & WGALLOWEDIP_REMOVE_ME) ? "-" : "", ip, allowedip->cidr);
		}
	}
	(void) fprintf(f, "\n");
	(void) fflush(f);

	errno = 0;
	while (getline(&key, &line_buffer_len, f) > 0) {
		line_len = strlen(key);
		ret = set_errno;
		if (line_len == 1 && key[0] == '\n')
			goto out;
		value = strchr(key, '=');
		if (!value || line_len == 0 || key[line_len - 1] != '\n')
			break;
		*value++ = key[--line_len] = '\0';

		if (!strcmp(key, "errno")) {
			long long num;
			char *end;
			if (value[0] != '-' && !char_is_digit(value[0]))
				break;
			num = strtoll(value, &end, 10);
			if (*end || num > INT_MAX || num < INT_MIN)
				break;
			set_errno = (int)num;
		}
	}
	ret = errno ? -errno : -EPROTO;
out:
	free(key);
	(void) fclose(f);
	errno = -ret;
	return ret;
}

#define NUM(max) ({ \
	unsigned long long num; \
	char *end; \
	if (!char_is_digit(value[0])) \
		break; \
	num = strtoull(value, &end, 10); \
	if (*end || num > (max)) \
		break; \
	num; \
})

/* Replace an owned string field with a copy of a get=1 value (free-before-strdup). */
static bool uapi_dup(char **dst, const char *value)
{
	free(*dst);
	*dst = strdup(value);
	return *dst != NULL;
}

static int userspace_get_device(struct wgdevice **out, const char *iface)
{
	struct wgdevice *dev;
	struct wgpeer *peer = NULL;
	struct wgallowedip *allowedip = NULL;
	size_t line_buffer_len = 0, line_len;
	char *key = NULL, *value;
	FILE *f;
	int ret = -EPROTO;

	*out = dev = calloc(1, sizeof(*dev));
	if (!dev)
		return -errno;

	f = userspace_interface_file(iface);
	if (!f) {
		ret = -errno;
		free(dev);
		*out = NULL;
		return ret;
	}

	(void) fprintf(f, "get=1\n\n");
	(void) fflush(f);

	strncpy(dev->name, iface, IFNAMSIZ - 1);
	dev->name[IFNAMSIZ - 1] = '\0';

	while (getline(&key, &line_buffer_len, f) > 0) {
		line_len = strlen(key);
		if (line_len == 1 && key[0] == '\n')
			goto err;
		value = strchr(key, '=');
		if (!value || line_len == 0 || key[line_len - 1] != '\n')
			break;
		*value++ = key[--line_len] = '\0';

		if (!peer && !strcmp(key, "private_key")) {
			if (!key_from_hex(dev->private_key, value))
				break;
			curve25519_generate_public(dev->public_key, dev->private_key);
			dev->flags |= WGDEVICE_HAS_PRIVATE_KEY | WGDEVICE_HAS_PUBLIC_KEY;
		} else if (!peer && !strcmp(key, "listen_port")) {
			dev->listen_port = NUM(0xffffU);
			dev->flags |= WGDEVICE_HAS_LISTEN_PORT;
		} else if (!peer && !strcmp(key, "fwmark")) {
			dev->fwmark = NUM(0xffffffffU);
			dev->flags |= WGDEVICE_HAS_FWMARK;
		} else if (!strcmp(key, "public_key")) {
			struct wgpeer *new_peer = calloc(1, sizeof(*new_peer));

			if (!new_peer) {
				ret = -ENOMEM;
				goto err;
			}
			allowedip = NULL;
			if (peer)
				peer->next_peer = new_peer;
			else
				dev->first_peer = new_peer;
			peer = new_peer;
			if (!key_from_hex(peer->public_key, value))
				break;
			peer->flags |= WGPEER_HAS_PUBLIC_KEY;
		} else if (peer && !strcmp(key, "preshared_key")) {
			if (!key_from_hex(peer->preshared_key, value))
				break;
			if (!key_is_zero(peer->preshared_key))
				peer->flags |= WGPEER_HAS_PRESHARED_KEY;
		} else if (peer && !strcmp(key, "endpoint")) {
			char *begin, *end;
			char sep;
			struct addrinfo *resolved;
			struct addrinfo hints = {
				.ai_family = AF_UNSPEC,
				.ai_socktype = SOCK_DGRAM,
				.ai_protocol = IPPROTO_UDP
			};
			if (!strlen(value))
				break;
			if (value[0] == '[') {
				begin = &value[1];
				end = strchr(value, ']');
				if (!end)
					break;
				*end++ = '\0';
				sep = *end;
				++end;
				if (sep != ':' || !*end)
					break;
			} else {
				begin = value;
				end = strrchr(value, ':');
				if (!end || !*(end + 1))
					break;
				*end++ = '\0';
			}
			if (getaddrinfo(begin, end, &hints, &resolved) != 0) {
				ret = ENETUNREACH;
				goto err;
			}
			if ((resolved->ai_family == AF_INET && resolved->ai_addrlen == sizeof(struct sockaddr_in)) ||
			    (resolved->ai_family == AF_INET6 && resolved->ai_addrlen == sizeof(struct sockaddr_in6)))
				memcpy(&peer->endpoint.addr, resolved->ai_addr, resolved->ai_addrlen);
			else  {
				freeaddrinfo(resolved);
				break;
			}
			freeaddrinfo(resolved);
		} else if (peer && !strcmp(key, "transport")) {
			peer->transport = !strcmp(value, "wstunnel") ? WGPEER_TRANSPORT_WSTUNNEL :
					  !strcmp(value, "websocket") ? WGPEER_TRANSPORT_WEBSOCKET : WGPEER_TRANSPORT_UDP;
			peer->flags |= WGPEER_HAS_TRANSPORT;
		} else if (peer && !strcmp(key, "ws_url")) {
			if (!uapi_dup(&peer->ws_url, value)) {
				ret = -ENOMEM;
				goto err;
			}
		} else if (peer && !strcmp(key, "wstunnel_target")) {
			if (!uapi_dup(&peer->wstunnel_target, value)) {
				ret = -ENOMEM;
				goto err;
			}
		} else if (peer && !strcmp(key, "ws_bearer")) {
			if (!uapi_dup(&peer->ws_bearer, value)) {
				ret = -ENOMEM;
				goto err;
			}
		} else if (peer && !strcmp(key, "ws_tls_ca")) {
			if (!uapi_dup(&peer->ws_tls_ca, value)) {
				ret = -ENOMEM;
				goto err;
			}
		} else if (peer && !strcmp(key, "ws_tls_cert")) {
			if (!uapi_dup(&peer->ws_tls_cert, value)) {
				ret = -ENOMEM;
				goto err;
			}
		} else if (peer && !strcmp(key, "ws_tls_key")) {
			if (!uapi_dup(&peer->ws_tls_key, value)) {
				ret = -ENOMEM;
				goto err;
			}
		} else if (peer && !strcmp(key, "ws_mask")) {
			peer->ws_mask = !strcmp(value, "true");
		} else if (peer && !strcmp(key, "ws_tls_insecure")) {
			peer->ws_tls_insecure = !strcmp(value, "true");
		} else if (peer && !strcmp(key, "ws_ping_interval")) {
			peer->ws_ping_interval_ms = NUM(0xffffffffU);
		} else if (peer && !strcmp(key, "ws_backoff_min")) {
			peer->ws_backoff_min_ms = NUM(0xffffffffU);
		} else if (peer && !strcmp(key, "ws_backoff_max")) {
			peer->ws_backoff_max_ms = NUM(0xffffffffU);
		} else if (!peer && !strcmp(key, "ws_listen")) {
			if (!uapi_dup(&dev->ws_listen, value)) {
				ret = -ENOMEM;
				goto err;
			}
			dev->flags |= WGDEVICE_HAS_WS_LISTEN;
		} else if (!peer && !strcmp(key, "ws_server_tls_cert")) {
			if (!uapi_dup(&dev->ws_server_tls_cert, value)) {
				ret = -ENOMEM;
				goto err;
			}
			dev->flags |= WGDEVICE_HAS_WS_SERVER_TLS_CERT;
		} else if (!peer && !strcmp(key, "ws_server_tls_key")) {
			if (!uapi_dup(&dev->ws_server_tls_key, value)) {
				ret = -ENOMEM;
				goto err;
			}
			dev->flags |= WGDEVICE_HAS_WS_SERVER_TLS_KEY;
		} else if (!peer && !strcmp(key, "ws_server_bearer")) {
			if (!uapi_dup(&dev->ws_server_bearer, value)) {
				ret = -ENOMEM;
				goto err;
			}
			dev->flags |= WGDEVICE_HAS_WS_SERVER_BEARER;
		} else if (!peer && !strcmp(key, "ws_trusted_proxies")) {
			if (!uapi_dup(&dev->ws_trusted_proxies, value)) {
				ret = -ENOMEM;
				goto err;
			}
			dev->flags |= WGDEVICE_HAS_WS_TRUSTED_PROXIES;
		} else if (peer && !strcmp(key, "persistent_keepalive_interval")) {
			peer->persistent_keepalive_interval = NUM(0xffffU);
			peer->flags |= WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL;
		} else if (peer && !strcmp(key, "allowed_ip")) {
			struct wgallowedip *new_allowedip;
			char *end, *mask = value, *ip = strsep(&mask, "/");

			if (!mask || !char_is_digit(mask[0]))
				break;
			new_allowedip = calloc(1, sizeof(*new_allowedip));
			if (!new_allowedip) {
				ret = -ENOMEM;
				goto err;
			}
			if (allowedip)
				allowedip->next_allowedip = new_allowedip;
			else
				peer->first_allowedip = new_allowedip;
			allowedip = new_allowedip;
			allowedip->family = AF_UNSPEC;
			if (strchr(ip, ':')) {
				if (inet_pton(AF_INET6, ip, &allowedip->ip6) == 1)
					allowedip->family = AF_INET6;
			} else {
				if (inet_pton(AF_INET, ip, &allowedip->ip4) == 1)
					allowedip->family = AF_INET;
			}
			allowedip->cidr = strtoul(mask, &end, 10);
			if (*end || allowedip->family == AF_UNSPEC || (allowedip->family == AF_INET6 && allowedip->cidr > 128) || (allowedip->family == AF_INET && allowedip->cidr > 32))
				break;
		} else if (peer && !strcmp(key, "last_handshake_time_sec"))
			peer->last_handshake_time.tv_sec = NUM(0x7fffffffffffffffULL);
		else if (peer && !strcmp(key, "last_handshake_time_nsec"))
			peer->last_handshake_time.tv_nsec = NUM(0x7fffffffffffffffULL);
		else if (peer && !strcmp(key, "rx_bytes"))
			peer->rx_bytes = NUM(0xffffffffffffffffULL);
		else if (peer && !strcmp(key, "tx_bytes"))
			peer->tx_bytes = NUM(0xffffffffffffffffULL);
		else if (!strcmp(key, "errno"))
			ret = -(int)NUM(0x7fffffffU);
	}
	ret = -EPROTO;
err:
	free(key);
	if (ret) {
		free_wgdevice(dev);
		*out = NULL;
	}
	(void) fclose(f);
	errno = -ret;
	return ret;

}
#undef NUM
