// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <arpa/inet.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>

#include "config.h"
#include "containers.h"
#include "ipc.h"
#include "encoding.h"
#include "ctype.h"

#define COMMENT_CHAR '#'

static const char *get_value(const char *line, const char *key)
{
	size_t linelen = strlen(line);
	size_t keylen = strlen(key);

	if (keylen >= linelen)
		return NULL;

	if (strncasecmp(line, key, keylen) != 0)
		return NULL;

	return line + keylen;
}

static inline bool parse_port(uint16_t *port, uint32_t *flags, const char *value)
{
	int ret;
	struct addrinfo *resolved;
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM,
		.ai_protocol = IPPROTO_UDP,
		.ai_flags = AI_PASSIVE
	};

	if (!strlen(value)) {
		(void) fprintf(stderr, "Unable to parse empty port\n");
		return false;
	}

	ret = getaddrinfo(NULL, value, &hints, &resolved);
	if (ret) {
		(void) fprintf(stderr, "%s: `%s'\n", ret == EAI_SYSTEM ? strerror(errno) : gai_strerror(ret), value);
		return false;
	}

	ret = -1;
	if (resolved->ai_family == AF_INET && resolved->ai_addrlen == sizeof(struct sockaddr_in)) {
		*port = ntohs(((struct sockaddr_in *)resolved->ai_addr)->sin_port);
		ret = 0;
	} else if (resolved->ai_family == AF_INET6 && resolved->ai_addrlen == sizeof(struct sockaddr_in6)) {
		*port = ntohs(((struct sockaddr_in6 *)resolved->ai_addr)->sin6_port);
		ret = 0;
	} else
		(void) fprintf(stderr, "Neither IPv4 nor IPv6 address found: `%s'\n", value);

	freeaddrinfo(resolved);
	if (!ret)
		*flags |= WGDEVICE_HAS_LISTEN_PORT;
	return ret == 0;
}

/* fwmark and flags are distinct out-params by role; a wrapper type is not worth the churn. */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static inline bool parse_fwmark(uint32_t *fwmark, uint32_t *flags, const char *value)
{
	unsigned long ret;
	char *end;
	int base = 10;

	if (!strcasecmp(value, "off")) {
		*fwmark = 0;
		*flags |= WGDEVICE_HAS_FWMARK;
		return true;
	}

	if (!char_is_digit(value[0]))
		goto err;

	if (strlen(value) > 2 && value[0] == '0' && value[1] == 'x')
		base = 16;

	ret = strtoul(value, &end, base);
	if (*end || ret > UINT32_MAX)
		goto err;

	*fwmark = ret;
	*flags |= WGDEVICE_HAS_FWMARK;
	return true;
err:
	fprintf(stderr, "Fwmark is neither 0/off nor 0-0xffffffff: `%s'\n", value);
	return false;
}

static inline bool parse_key(uint8_t key[static WG_KEY_LEN], const char *value)
{
	if (!key_from_base64(key, value)) {
		(void) fprintf(stderr, "Key is not the correct length or format: `%s'\n", value);
		memset(key, 0, WG_KEY_LEN);
		return false;
	}
	return true;
}

static bool parse_keyfile(uint8_t key[static WG_KEY_LEN], const char *path)
{
	FILE *f;
	int c;
	char dst[WG_KEY_LEN_BASE64];
	bool ret = false;

	f = fopen(path, "r");
	if (!f) {
		perror("fopen");
		return false;
	}

	if (fread(dst, WG_KEY_LEN_BASE64 - 1, 1, f) != 1) {
		/* If we're at the end and we didn't read anything, we're /dev/null or an empty file. */
		if (!ferror(f) && feof(f) && !ftell(f)) {
			memset(key, 0, WG_KEY_LEN);
			ret = true;
			goto out;
		}

		(void) fprintf(stderr, "Invalid length key in key file\n");
		goto out;
	}
	dst[WG_KEY_LEN_BASE64 - 1] = '\0';

	while ((c = getc(f)) != EOF) {
		if (!char_is_space(c)) {
			(void) fprintf(stderr, "Found trailing character in key file: `%c'\n", c);
			goto out;
		}
	}
	if (ferror(f) && errno) {
		perror("getc");
		goto out;
	}
	ret = parse_key(key, dst);

out:
	fclose(f);
	return ret;
}

static inline bool parse_ip(struct wgallowedip *allowedip, const char *value)
{
	allowedip->family = AF_UNSPEC;
	if (strchr(value, ':')) {
		if (inet_pton(AF_INET6, value, &allowedip->ip6) == 1)
			allowedip->family = AF_INET6;
	} else {
		if (inet_pton(AF_INET, value, &allowedip->ip4) == 1)
			allowedip->family = AF_INET;
	}
	if (allowedip->family == AF_UNSPEC) {
		(void) fprintf(stderr, "Unable to parse IP address: `%s'\n", value);
		return false;
	}
	return true;
}

static inline int parse_dns_retries(void)
{
	unsigned long ret;
	char *retries = getenv("WG_ENDPOINT_RESOLUTION_RETRIES"), *end;

	if (!retries)
		return 15;
	if (!strcmp(retries, "infinity"))
		return -1;

	ret = strtoul(retries, &end, 10);
	if (*end || ret > INT_MAX) {
		(void) fprintf(stderr, "Unable to parse WG_ENDPOINT_RESOLUTION_RETRIES: `%s'\n", retries);
		exit(1);
	}
	return (int)ret;
}

static inline bool parse_endpoint(struct sockaddr *endpoint, const char *value)
{
	char *mutable = strdup(value);
	char *begin, *end;
	char sep;
	bool last_try;
	int ret, retries = parse_dns_retries();
	struct addrinfo *resolved;
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM,
		.ai_protocol = IPPROTO_UDP
	};
	if (!mutable) {
		perror("strdup");
		return false;
	}
	if (!strlen(value)) {
		free(mutable);
		(void) fprintf(stderr, "Unable to parse empty endpoint\n");
		return false;
	}
	if (mutable[0] == '[') {
		begin = &mutable[1];
		end = strchr(mutable, ']');
		if (!end) {
			free(mutable);
			(void) fprintf(stderr, "Unable to find matching brace of endpoint: `%s'\n", value);
			return false;
		}
		*end++ = '\0';
		sep = *end;
		++end;
		if (sep != ':' || !*end) {
			free(mutable);
			(void) fprintf(stderr, "Unable to find port of endpoint: `%s'\n", value);
			return false;
		}
	} else {
		begin = mutable;
		end = strrchr(mutable, ':');
		if (!end || !*(end + 1)) {
			free(mutable);
			(void) fprintf(stderr, "Unable to find port of endpoint: `%s'\n", value);
			return false;
		}
		*end++ = '\0';
	}

	#define min(a, b) ((a) < (b) ? (a) : (b))
	for (unsigned int timeout = 1000000;; timeout = min(20000000, timeout * 6 / 5)) {
		ret = getaddrinfo(begin, end, &hints, &resolved);
		if (!ret)
			break;
		/* The set of return codes that are "permanent failures". All other possibilities are potentially transient.
		 *
		 * This is according to https://sourceware.org/glibc/wiki/NameResolver which states:
		 *	"From the perspective of the application that calls getaddrinfo() it perhaps
		 *	 doesn't matter that much since EAI_FAIL, EAI_NONAME and EAI_NODATA are all
		 *	 permanent failure codes and the causes are all permanent failures in the
		 *	 sense that there is no point in retrying later."
		 *
		 * So this is what we do, except FreeBSD removed EAI_NODATA some time ago, so that's conditional.
		 */
		last_try = (retries == 0);
		if (retries >= 0)
			--retries;
		if (ret == EAI_NONAME || ret == EAI_FAIL ||
			#ifdef EAI_NODATA
				ret == EAI_NODATA ||
			#endif
				last_try) {
			free(mutable);
			(void) fprintf(stderr, "%s: `%s'\n", ret == EAI_SYSTEM ? strerror(errno) : gai_strerror(ret), value);
			return false;
		}
		(void) fprintf(stderr, "%s: `%s'. Trying again in %.2f seconds...\n", ret == EAI_SYSTEM ? strerror(errno) : gai_strerror(ret), value, timeout / 1000000.0);
		usleep(timeout);
	}

	if ((resolved->ai_family == AF_INET && resolved->ai_addrlen == sizeof(struct sockaddr_in)) ||
	    (resolved->ai_family == AF_INET6 && resolved->ai_addrlen == sizeof(struct sockaddr_in6)))
		memcpy(endpoint, resolved->ai_addr, resolved->ai_addrlen);
	else {
		freeaddrinfo(resolved);
		free(mutable);
		(void) fprintf(stderr, "Neither IPv4 nor IPv6 address found: `%s'\n", value);
		return false;
	}
	freeaddrinfo(resolved);
	free(mutable);
	return true;
}

static inline bool parse_persistent_keepalive(uint16_t *interval, uint32_t *flags, const char *value)
{
	unsigned long ret;
	char *end;

	if (!strcasecmp(value, "off")) {
		*interval = 0;
		*flags |= WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL;
		return true;
	}

	if (!char_is_digit(value[0]))
		goto err;

	ret = strtoul(value, &end, 10);
	if (*end || ret > 65535)
		goto err;

	*interval = (uint16_t)ret;
	*flags |= WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL;
	return true;
err:
	fprintf(stderr, "Persistent keepalive interval is neither 0/off nor 1-65535: `%s'\n", value);
	return false;
}

static bool is_ws_url(const char *v)
{
	return !strncasecmp(v, "ws://", 5) || !strncasecmp(v, "wss://", 6);
}

/* Split ws(s)://host:port/path into host (IPv6 literal unwrapped from [...]) and port.
 * Rejects a missing scheme/host/:port and oversize. Does not allocate.
 */
static bool ws_url_split(const char *url, char *host, size_t hostsz, char *port, size_t portsz)
{
	const char *p, *hstart, *hend, *pstart, *pend;

	if (!strncasecmp(url, "wss://", 6))
		p = url + 6;
	else if (!strncasecmp(url, "ws://", 5))
		p = url + 5;
	else
		return false;
	if (*p == '[') {
		hstart = p + 1;
		hend = strchr(hstart, ']');
		if (!hend || hend[1] != ':')
			return false;
		pstart = hend + 2;
	} else {
		const char *colon = NULL;

		for (const char *q = p; *q && *q != '/'; ++q) {
			if (*q == ':')
				colon = q;
		}
		if (!colon)
			return false;
		hstart = p;
		hend = colon;
		pstart = colon + 1;
	}
	for (pend = pstart; *pend && *pend != '/'; ++pend)
		;
	if (hend == hstart || pend == pstart)
		return false;
	if ((size_t)(hend - hstart) >= hostsz || (size_t)(pend - pstart) >= portsz)
		return false;
	memcpy(host, hstart, hend - hstart);
	host[hend - hstart] = '\0';
	memcpy(port, pstart, pend - pstart);
	port[pend - pstart] = '\0';
	return true;
}

/* A ws(s):// Endpoint is resolved host-side to peer->endpoint (exactly like a UDP endpoint), and
 * the whole URL is kept as ws_url for the daemon's TLS/HTTP layer. */
static bool parse_ws_endpoint(struct wgpeer *peer, const char *url)
{
	char host[256], port[16], hostport[300];

	if (!ws_url_split(url, host, sizeof host, port, sizeof port)) {
		fprintf(stderr, "Endpoint is not a valid ws(s):// URL: `%s'\n", url);
		return false;
	}
	if (strchr(host, ':'))
		snprintf(hostport, sizeof hostport, "[%s]:%s", host, port);
	else
		snprintf(hostport, sizeof hostport, "%s:%s", host, port);
	if (!parse_endpoint(&peer->endpoint.addr, hostport))
		return false;
	free(peer->ws_url);
	peer->ws_url = strdup(url);
	if (!peer->ws_url) {
		perror("strdup");
		return false;
	}
	return true;
}

static bool parse_ws_mode(enum wgpeer_transport *transport, const char *value)
{
	if (strcmp(value, "websocket") != 0 && strcmp(value, "wstunnel") != 0) {
		fprintf(stderr, "WSMode is neither websocket nor wstunnel: `%s'\n", value);
		return false;
	}
	*transport = strcmp(value, "wstunnel") == 0 ? WGPEER_TRANSPORT_WSTUNNEL : WGPEER_TRANSPORT_WEBSOCKET;
	return true;
}

/* WSTunnelTarget: validate host:port and store it verbatim — the wstunnel relay resolves the inner
 * target, so the tools must not DNS-resolve it. */
static bool parse_ws_target(char **dst, const char *value)
{
	const char *colon = strrchr(value, ':');
	bool bad = !colon || colon == value || !colon[1];

	for (const char *p = colon ? colon + 1 : ""; !bad && *p; ++p)
		bad = *p < '0' || *p > '9';
	if (bad) {
		fprintf(stderr, "WSTunnelTarget is not in host:port form: `%s'\n", value);
		return false;
	}
	free(*dst);
	*dst = strdup(value);
	if (!*dst) {
		perror("strdup");
		return false;
	}
	return true;
}

static bool parse_ws_bool(bool *dst, const char *value)
{
	if (!strcmp(value, "true"))
		*dst = true;
	else if (!strcmp(value, "false"))
		*dst = false;
	else {
		fprintf(stderr, "Expected true or false, got: `%s'\n", value);
		return false;
	}
	return true;
}

static bool parse_ws_millis(uint32_t *dst, const char *value)
{
	char *end;
	unsigned long long n = strtoull(value, &end, 10);

	if (*value == '\0' || *end != '\0' || n > 0xffffffffULL) {
		fprintf(stderr, "Value is not a valid millisecond count: `%s'\n", value);
		return false;
	}
	*dst = (uint32_t)n;
	return true;
}

static bool parse_ws_str(char **dst, const char *value)
{
	if (!*value) {
		fprintf(stderr, "Value is empty\n");
		return false;
	}
	free(*dst);
	*dst = strdup(value);
	if (!*dst) {
		perror("strdup");
		return false;
	}
	return true;
}

/* WSListen: an empty value clears the listener (emits ws_listen= empty, which the daemon accepts as
 * a clear); otherwise it must be a ws(s):// URL. Always sets WGDEVICE_HAS_WS_LISTEN. */
static bool parse_ws_listen(char **dst, uint32_t *flags, const char *value)
{
	if (*value && !is_ws_url(value)) {
		fprintf(stderr, "WSListen is neither empty nor a ws(s):// URL: `%s'\n", value);
		return false;
	}
	free(*dst);
	*dst = NULL;
	if (*value) {
		*dst = strdup(value);
		if (!*dst) {
			perror("strdup");
			return false;
		}
	}
	*flags |= WGDEVICE_HAS_WS_LISTEN;
	return true;
}

/* Device WS string keys (server cert/key/bearer, trusted proxies): an empty value clears the field
 * and still sets its flag so key= (empty) is emitted; a non-empty value is stored verbatim. Never
 * prints the value, which keeps it safe for the secret WSServerBearer. */
static bool parse_ws_device_str(char **dst, uint32_t *flags, uint32_t bit, const char *value)
{
	free(*dst);
	*dst = NULL;
	if (*value) {
		*dst = strdup(value);
		if (!*dst) {
			perror("strdup");
			return false;
		}
	}
	*flags |= bit;
	return true;
}

/* Inline bearer (config file): validate non-empty and never print the value. */
static bool parse_ws_secret(char **dst, const char *value)
{
	if (!*value) {
		fprintf(stderr, "A WebSocket bearer value is empty\n");
		return false;
	}
	free(*dst);
	*dst = strdup(value);
	if (!*dst) {
		perror("strdup");
		return false;
	}
	return true;
}

/* CLI bearer: read the token from a file so the secret never sits in argv (parity with
 * private-key/preshared-key). Bounded read; an over-length token is rejected, the value is never
 * printed (errors reference the path), and the stack copy is wiped. */
static bool parse_ws_secret_file(char **dst, const char *path)
{
	char buf[8192];
	FILE *f = fopen(path, "r");
	size_t len;

	if (!f) {
		fprintf(stderr, "Unable to open `%s': %s\n", path, strerror(errno));
		return false;
	}
	if (!fgets(buf, sizeof buf, f)) {
		fclose(f);
		fprintf(stderr, "Unable to read bearer from `%s'\n", path);
		return false;
	}
	len = strlen(buf);
	if (len && buf[len - 1] == '\n')
		buf[--len] = '\0';
	else if (len == sizeof buf - 1 && fgetc(f) != EOF) {
		fclose(f);
		memset(buf, 0, sizeof buf);
		fprintf(stderr, "Bearer in `%s' is too long (max %zu bytes)\n", path, sizeof buf - 1);
		return false;
	}
	fclose(f);
	if (!len) {
		fprintf(stderr, "Bearer file `%s' is empty\n", path);
		return false;
	}
	free(*dst);
	*dst = strdup(buf);
	memset(buf, 0, sizeof buf);
	if (!*dst) {
		perror("strdup");
		return false;
	}
	return true;
}

static bool validate_ws_peer(const struct wgpeer *peer)
{
	bool has_ep = peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6;

	/* Incremental CLI update (transport not declared this command): the peer's real transport is the
	 * daemon's persisted value, unknown here, so defer to the daemon. */
	if (!(peer->flags & WGPEER_HAS_TRANSPORT))
		return true;
	if (peer->transport == WGPEER_TRANSPORT_UDP) {
		if (peer->ws_url) {
			fprintf(stderr, "A ws(s):// Endpoint requires WSMode\n");
			return false;
		}
		if (peer->flags & WGPEER_HAS_WS_SETTINGS) {
			fprintf(stderr, "A UDP peer has WebSocket settings\n");
			return false;
		}
		return true;
	}
	if (peer->ws_url) {
		if (!has_ep) {
			fprintf(stderr, "A dialing WebSocket peer has no resolved endpoint\n");
			return false;
		}
		if (peer->transport == WGPEER_TRANSPORT_WSTUNNEL && !peer->wstunnel_target) {
			fprintf(stderr, "WSMode=wstunnel requires WSTunnelTarget\n");
			return false;
		}
	} else {
		if (has_ep) {
			fprintf(stderr, "An inbound WebSocket peer must not set an Endpoint\n");
			return false;
		}
		if (peer->transport == WGPEER_TRANSPORT_WSTUNNEL) {
			fprintf(stderr, "WSMode=wstunnel requires a ws(s):// Endpoint (wstunnel is a dialing mode)\n");
			return false;
		}
		if (peer->wstunnel_target) {
			fprintf(stderr, "An inbound WebSocket peer must not set WSTunnelTarget\n");
			return false;
		}
	}
	if (peer->transport == WGPEER_TRANSPORT_WEBSOCKET && peer->wstunnel_target) {
		fprintf(stderr, "WSTunnelTarget requires WSMode=wstunnel\n");
		return false;
	}
	return true;
}

static bool validate_netmask(struct wgallowedip *allowedip)
{
	uint32_t *ip;
	int last;

	switch (allowedip->family) {
		case AF_INET:
			last = 0;
			ip = (uint32_t *)&allowedip->ip4;
			break;
		case AF_INET6:
			last = 3;
			ip = (uint32_t *)&allowedip->ip6;
			break;
		default:
			return true; /* We don't know how to validate it, so say 'okay'. */
	}

	for (int i = last; i >= 0; --i) {
		uint32_t mask = ~0;

		if (allowedip->cidr >= 32 * (i + 1))
			break;
		if (allowedip->cidr > 32 * i)
			mask >>= (allowedip->cidr - 32 * i);
		if (ntohl(ip[i]) & mask)
			return false;
	}

	return true;
}

static inline void parse_ip_prefix(struct wgpeer *peer, uint32_t *flags, char **mask)
{
	/* If the IP is prefixed with either '+' or '-' consider this an
	 * incremental change. Disable WGPEER_REPLACE_ALLOWEDIPS. */
	switch ((*mask)[0]) {
	case '-':
		*flags |= WGALLOWEDIP_REMOVE_ME;
		/* fall through */
	case '+':
		peer->flags &= ~WGPEER_REPLACE_ALLOWEDIPS;
		++(*mask);
		break;
	default:
		break;
	}
}

static inline bool parse_allowedips(struct wgpeer *peer, struct wgallowedip **last_allowedip, const char *value)
{
	struct wgallowedip *allowedip = *last_allowedip, *new_allowedip;
	char *mask, *mutable = strdup(value), *sep, *saved_entry;

	if (!mutable) {
		perror("strdup");
		return false;
	}
	peer->flags |= WGPEER_REPLACE_ALLOWEDIPS;
	if (!strlen(value)) {
		free(mutable);
		return true;
	}
	sep = mutable;
	while ((mask = strsep(&sep, ","))) {
		uint32_t flags = 0;
		unsigned long cidr;
		char *end, *ip;

		parse_ip_prefix(peer, &flags, &mask);

		saved_entry = strdup(mask);
		if (!saved_entry) {
			perror("strdup");
			free(mutable);
			return false;
		}
		ip = strsep(&mask, "/");

		new_allowedip = calloc(1, sizeof(*new_allowedip));
		if (!new_allowedip) {
			perror("calloc");
			free(saved_entry);
			free(mutable);
			return false;
		}

		if (!parse_ip(new_allowedip, ip)) {
			free(new_allowedip);
			free(saved_entry);
			free(mutable);
			return false;
		}

		if (mask) {
			if (!char_is_digit(mask[0]))
				goto err;
			cidr = strtoul(mask, &end, 10);
			if (*end || (cidr > 32 && new_allowedip->family == AF_INET) || (cidr > 128 && new_allowedip->family == AF_INET6))
				goto err;
		} else if (new_allowedip->family == AF_INET)
			cidr = 32;
		else if (new_allowedip->family == AF_INET6)
			cidr = 128;
		else
			goto err;
		new_allowedip->cidr = cidr;
		new_allowedip->flags = flags;

		if (!validate_netmask(new_allowedip))
			(void) fprintf(stderr, "Warning: AllowedIP has nonzero host part: %s/%s\n", ip, mask);

		if (allowedip)
			allowedip->next_allowedip = new_allowedip;
		else
			peer->first_allowedip = new_allowedip;
		allowedip = new_allowedip;
		free(saved_entry);
	}
	free(mutable);
	*last_allowedip = allowedip;
	return true;

err:
	free(new_allowedip);
	free(mutable);
	(void) fprintf(stderr, "AllowedIP is not in the correct format: `%s'\n", saved_entry);
	free(saved_entry);
	return false;
}

static bool process_line(struct config_ctx *ctx, const char *line)
{
	const char *value;
	bool ret = true;

	if (!strcasecmp(line, "[Interface]")) {
		ctx->is_peer_section = false;
		ctx->is_device_section = true;
		return true;
	}
	if (!strcasecmp(line, "[Peer]")) {
		struct wgpeer *new_peer = calloc(1, sizeof(struct wgpeer));

		if (!new_peer) {
			perror("calloc");
			return false;
		}
		ctx->last_allowedip = NULL;
		if (ctx->last_peer)
			ctx->last_peer->next_peer = new_peer;
		else
			ctx->device->first_peer = new_peer;
		ctx->last_peer = new_peer;
		ctx->is_peer_section = true;
		ctx->is_device_section = false;
		/* A config file fully declares each peer, so transport= is always emitted for it. */
		ctx->last_peer->flags |= WGPEER_REPLACE_ALLOWEDIPS | WGPEER_HAS_TRANSPORT;
		return true;
	}

#define key_match(key) (value = get_value(line, key "="))

	/* key_match() is an intentional assign-and-test idiom driving the
	 * section/key dispatch. A table-driven refactor is deferred until the
	 * unit-test suite exists to guard this parser (see docs/PROJECT.md). */
	/* NOLINTBEGIN(bugprone-assignment-in-if-condition) */
	if (ctx->is_device_section) {
		if (key_match("ListenPort"))
			ret = parse_port(&ctx->device->listen_port, &ctx->device->flags, value);
		else if (key_match("FwMark"))
			ret = parse_fwmark(&ctx->device->fwmark, &ctx->device->flags, value);
		else if (key_match("PrivateKey")) {
			ret = parse_key(ctx->device->private_key, value);
			if (ret)
				ctx->device->flags |= WGDEVICE_HAS_PRIVATE_KEY;
		}
		/* Device WS keys are dispatched by strncasecmp so an empty value (which get_value
		 * rejects) can clear the key. The flags are set only when the key is present, and are
		 * NOT default-set in config_read_init, so a non-WS config emits no ws_* keys. */
		else if (!strncasecmp(line, "WSListen=", sizeof("WSListen=") - 1))
			ret = parse_ws_listen(&ctx->device->ws_listen, &ctx->device->flags, line + sizeof("WSListen=") - 1);
		else if (!strncasecmp(line, "WSServerTLSCert=", sizeof("WSServerTLSCert=") - 1))
			ret = parse_ws_device_str(&ctx->device->ws_server_tls_cert, &ctx->device->flags, WGDEVICE_HAS_WS_SERVER_TLS_CERT, line + sizeof("WSServerTLSCert=") - 1);
		else if (!strncasecmp(line, "WSServerTLSKey=", sizeof("WSServerTLSKey=") - 1))
			ret = parse_ws_device_str(&ctx->device->ws_server_tls_key, &ctx->device->flags, WGDEVICE_HAS_WS_SERVER_TLS_KEY, line + sizeof("WSServerTLSKey=") - 1);
		else if (!strncasecmp(line, "WSServerBearer=", sizeof("WSServerBearer=") - 1))
			ret = parse_ws_device_str(&ctx->device->ws_server_bearer, &ctx->device->flags, WGDEVICE_HAS_WS_SERVER_BEARER, line + sizeof("WSServerBearer=") - 1);
		else if (!strncasecmp(line, "WSTrustedProxies=", sizeof("WSTrustedProxies=") - 1))
			ret = parse_ws_device_str(&ctx->device->ws_trusted_proxies, &ctx->device->flags, WGDEVICE_HAS_WS_TRUSTED_PROXIES, line + sizeof("WSTrustedProxies=") - 1);
		else
			goto error;
	} else if (ctx->is_peer_section) {
		if (key_match("Endpoint"))
			ret = is_ws_url(value) ? parse_ws_endpoint(ctx->last_peer, value) : parse_endpoint(&ctx->last_peer->endpoint.addr, value);
		else if (key_match("PublicKey")) {
			ret = parse_key(ctx->last_peer->public_key, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_PUBLIC_KEY;
		} else if (key_match("AllowedIPs"))
			ret = parse_allowedips(ctx->last_peer, &ctx->last_allowedip, value);
		else if (key_match("PersistentKeepalive"))
			ret = parse_persistent_keepalive(&ctx->last_peer->persistent_keepalive_interval, &ctx->last_peer->flags, value);
		else if (key_match("PresharedKey")) {
			ret = parse_key(ctx->last_peer->preshared_key, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_PRESHARED_KEY;
		} else if (key_match("WSMode"))
			ret = parse_ws_mode(&ctx->last_peer->transport, value);
		else if (key_match("WSTunnelTarget")) {
			ret = parse_ws_target(&ctx->last_peer->wstunnel_target, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSBearer")) {
			ret = parse_ws_secret(&ctx->last_peer->ws_bearer, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSMask")) {
			ret = parse_ws_bool(&ctx->last_peer->ws_mask, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSTLSCA")) {
			ret = parse_ws_str(&ctx->last_peer->ws_tls_ca, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSTLSCert")) {
			ret = parse_ws_str(&ctx->last_peer->ws_tls_cert, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSTLSKey")) {
			ret = parse_ws_str(&ctx->last_peer->ws_tls_key, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSTLSInsecure")) {
			ret = parse_ws_bool(&ctx->last_peer->ws_tls_insecure, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSPingInterval")) {
			ret = parse_ws_millis(&ctx->last_peer->ws_ping_interval_ms, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSBackoffMin")) {
			ret = parse_ws_millis(&ctx->last_peer->ws_backoff_min_ms, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else if (key_match("WSBackoffMax")) {
			ret = parse_ws_millis(&ctx->last_peer->ws_backoff_max_ms, value);
			if (ret)
				ctx->last_peer->flags |= WGPEER_HAS_WS_SETTINGS;
		} else
			goto error;
	} else
		goto error;
	/* NOLINTEND(bugprone-assignment-in-if-condition) */
	return ret;

#undef key_match

error:
	fprintf(stderr, "Line unrecognized: `%s'\n", line);
	return false;
}

bool config_read_line(struct config_ctx *ctx, const char *input)
{
	size_t len, cleaned_len = 0;
	const char *comment;
	bool ret = true;
	char *line;

	/* This is what strchrnul is for, but that isn't portable. */
	comment = strchr(input, COMMENT_CHAR);
	if (comment)
		len = comment - input;
	else
		len = strlen(input);

	line = calloc(len + 1, sizeof(char));
	if (!line) {
		perror("calloc");
		ret = false;
		goto out;
	}

	for (size_t i = 0; i < len; ++i) {
		if (!char_is_space(input[i]))
			line[cleaned_len++] = input[i];
	}
	if (!cleaned_len)
		goto out;
	ret = process_line(ctx, line);
out:
	free(line);
	if (!ret)
		free_wgdevice(ctx->device);
	return ret;
}

bool config_read_init(struct config_ctx *ctx, bool append)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->device = calloc(1, sizeof(*ctx->device));
	if (!ctx->device) {
		perror("calloc");
		return false;
	}
	if (!append)
		ctx->device->flags |= WGDEVICE_REPLACE_PEERS | WGDEVICE_HAS_PRIVATE_KEY | WGDEVICE_HAS_FWMARK | WGDEVICE_HAS_LISTEN_PORT;
	return true;
}

struct wgdevice *config_read_finish(struct config_ctx *ctx)
{
	struct wgpeer *peer;

	for_each_wgpeer(ctx->device, peer) {
		if (!(peer->flags & WGPEER_HAS_PUBLIC_KEY)) {
			(void) fprintf(stderr, "A peer is missing a public key\n");
			goto err;
		}
		if (!validate_ws_peer(peer))
			goto err;
	}
	return ctx->device;
err:
	free_wgdevice(ctx->device);
	return NULL;
}

static char *strip_spaces(const char *in)
{
	char *out;
	size_t t, l, i;

	t = strlen(in);
	out = calloc(t + 1, sizeof(char));
	if (!out) {
		perror("calloc");
		return NULL;
	}
	for (i = 0, l = 0; i < t; ++i) {
		if (!char_is_space(in[i]))
			out[l++] = in[i];
	}
	return out;
}

struct wgdevice *config_read_cmd(const char *argv[], int argc)
{
	struct wgdevice *device = calloc(1, sizeof(*device));
	struct wgpeer *peer = NULL;
	struct wgallowedip *allowedip = NULL;

	if (!device) {
		perror("calloc");
		return false;
	}
	while (argc > 0) {
		if (!strcmp(argv[0], "listen-port") && argc >= 2 && !peer) {
			if (!parse_port(&device->listen_port, &device->flags, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "fwmark") && argc >= 2 && !peer) {
			if (!parse_fwmark(&device->fwmark, &device->flags, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "private-key") && argc >= 2 && !peer) {
			if (!parse_keyfile(device->private_key, argv[1]))
				goto error;
			device->flags |= WGDEVICE_HAS_PRIVATE_KEY;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "peer") && argc >= 2) {
			struct wgpeer *new_peer = calloc(1, sizeof(*new_peer));

			allowedip = NULL;
			if (!new_peer) {
				perror("calloc");
				goto error;
			}
			if (peer)
				peer->next_peer = new_peer;
			else
				device->first_peer = new_peer;
			peer = new_peer;
			if (!parse_key(peer->public_key, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_PUBLIC_KEY;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "remove") && argc >= 1 && peer) {
			peer->flags |= WGPEER_REMOVE_ME;
			argv += 1;
			argc -= 1;
		} else if (!strcmp(argv[0], "endpoint") && argc >= 2 && peer) {
			/* An endpoint (a bare ip:port OR a ws(s):// URL) declares the transport. */
			peer->flags |= WGPEER_HAS_TRANSPORT;
			if (!(is_ws_url(argv[1]) ? parse_ws_endpoint(peer, argv[1]) : parse_endpoint(&peer->endpoint.addr, argv[1])))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-mode") && argc >= 2 && peer) {
			peer->flags |= WGPEER_HAS_TRANSPORT;
			if (!parse_ws_mode(&peer->transport, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "wstunnel-target") && argc >= 2 && peer) {
			if (!parse_ws_target(&peer->wstunnel_target, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-bearer") && argc >= 2 && peer) {
			if (!parse_ws_secret_file(&peer->ws_bearer, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-mask") && argc >= 2 && peer) {
			if (!parse_ws_bool(&peer->ws_mask, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-tls-ca") && argc >= 2 && peer) {
			if (!parse_ws_str(&peer->ws_tls_ca, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-tls-cert") && argc >= 2 && peer) {
			if (!parse_ws_str(&peer->ws_tls_cert, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-tls-key") && argc >= 2 && peer) {
			if (!parse_ws_str(&peer->ws_tls_key, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-tls-insecure") && argc >= 2 && peer) {
			if (!parse_ws_bool(&peer->ws_tls_insecure, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-ping-interval") && argc >= 2 && peer) {
			if (!parse_ws_millis(&peer->ws_ping_interval_ms, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-backoff-min") && argc >= 2 && peer) {
			if (!parse_ws_millis(&peer->ws_backoff_min_ms, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-backoff-max") && argc >= 2 && peer) {
			if (!parse_ws_millis(&peer->ws_backoff_max_ms, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_WS_SETTINGS;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-listen") && argc >= 2 && !peer) {
			if (!parse_ws_listen(&device->ws_listen, &device->flags, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-server-tls-cert") && argc >= 2 && !peer) {
			if (!parse_ws_device_str(&device->ws_server_tls_cert, &device->flags, WGDEVICE_HAS_WS_SERVER_TLS_CERT, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-server-tls-key") && argc >= 2 && !peer) {
			if (!parse_ws_device_str(&device->ws_server_tls_key, &device->flags, WGDEVICE_HAS_WS_SERVER_TLS_KEY, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-server-bearer") && argc >= 2 && !peer) {
			if (!parse_ws_secret_file(&device->ws_server_bearer, argv[1]))
				goto error;
			device->flags |= WGDEVICE_HAS_WS_SERVER_BEARER;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "ws-trusted-proxies") && argc >= 2 && !peer) {
			if (!parse_ws_device_str(&device->ws_trusted_proxies, &device->flags, WGDEVICE_HAS_WS_TRUSTED_PROXIES, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "allowed-ips") && argc >= 2 && peer) {
			char *line = strip_spaces(argv[1]);

			if (!line)
				goto error;
			if (!parse_allowedips(peer, &allowedip, line)) {
				free(line);
				goto error;
			}
			free(line);
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "persistent-keepalive") && argc >= 2 && peer) {
			if (!parse_persistent_keepalive(&peer->persistent_keepalive_interval, &peer->flags, argv[1]))
				goto error;
			argv += 2;
			argc -= 2;
		} else if (!strcmp(argv[0], "preshared-key") && argc >= 2 && peer) {
			if (!parse_keyfile(peer->preshared_key, argv[1]))
				goto error;
			peer->flags |= WGPEER_HAS_PRESHARED_KEY;
			argv += 2;
			argc -= 2;
		} else {
			(void) fprintf(stderr, "Invalid argument: %s\n", argv[0]);
			goto error;
		}
	}
	for (struct wgpeer *p = device->first_peer; p; p = p->next_peer) {
		if (!validate_ws_peer(p))
			goto error;
	}
	return device;
error:
	free_wgdevice(device);
	return false;
}
