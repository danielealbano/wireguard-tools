<!-- SACRED DOCUMENT — Edit ONLY per agent.md §2 plan-file rules: plan-review fixes, checkmarks, recorded implementation deviations, and code-review re-alignment. -->
<!-- You MUST NEVER delete this file or alter files outside this plan's scope. -->
<!-- Plans in docs/plans/ are PERMANENT artifacts. There are ZERO exceptions. -->

# Plan 1 — WebSocket/wstunnel config surface (UDP-parity redesign)

## Goal & scope

Teach `wg(8)`/`wg-quick(8)` to configure the sibling `wireguard-go` fork's per-peer
WebSocket/wstunnel transport using the **v1.3.0 UDP-parity UAPI contract**. Every peer emits a
mandatory `transport=`; the encrypted destination is a **resolved `endpoint=ip:port`** (exactly
like UDP), and the `ws(s)://` URL travels in a **separate `ws_url=`**. All WebSocket settings are
per-peer or device UAPI keys — there are **no `WG_WS_*` environment variables** and no daemon
"role". Only `WG_METRICS_LISTEN` remains an env var.

This targets **`wireguard-go` ≥ 1.3.0** (contract: its `docs/CONFIGURATION.md` +
`docs/WGQUICK_INTEGRATION.md`). The daemon owns the socket side and off-tun socket marking;
**routing stays `wg-quick`'s job** and the mark-based (Linux/FreeBSD) and route-based (darwin)
mechanisms need **no per-transport change** because `endpoint=` is a routable `ip:port`.

### The v1.3.0 UAPI contract (authoritative — from wireguard-go docs/uapi.go)

Per-peer (`set=1`, round-tripped by `get=1`):
- `transport=udp|websocket|wstunnel` — mandatory at peer creation; an incremental `set` may omit it and keeps the persisted value.
- `endpoint=ip:port` — plain resolved address for **every** transport.
- `ws_url=ws(s)://host:port/path` — required to **dial** a WS peer; TLS scheme + SNI/Host + upgrade path. A WS peer with **no** `ws_url` is **inbound** (learned from the listener).
- `wstunnel_target=host:port` — required for `transport=wstunnel` dialing; rejected otherwise.
- `ws_bearer` (secret, echoed by get, never logged), `ws_mask=true|false`, `ws_tls_ca`, `ws_tls_cert`, `ws_tls_key`, `ws_tls_insecure=true|false`, `ws_ping_interval`, `ws_backoff_min`, `ws_backoff_max` (the three durations in **milliseconds**, `0`⇒default).
- `ws_*` keys are **rejected** for `transport=udp`.

Device (`set=1`/`get=1`):
- `ws_listen=ws(s)://host:port/path` (the `listen_port` analogue), `ws_server_tls_cert`, `ws_server_tls_key`, `ws_server_bearer` (secret), `ws_trusted_proxies=cidr,cidr`.

### Config-file surface (this repo's design — CamelCase, matching `AllowedIPs`/`ListenPort`/`FwMark`)

`[Peer]`: `Endpoint` (a `ws(s)://` URL for WS peers, or `host:port` for UDP), `WSMode = websocket|wstunnel`, `WSTunnelTarget`, `WSBearer`, `WSMask`, `WSTLSCA`, `WSTLSCert`, `WSTLSKey`, `WSTLSInsecure`, `WSPingInterval`, `WSBackoffMin`, `WSBackoffMax`.

`[Interface]`: `WSListen` (a `ws(s)://` URL), `WSServerTLSCert`, `WSServerTLSKey`, `WSServerBearer`, `WSTrustedProxies`, `MetricsListen` (→ `WG_METRICS_LISTEN` env; the sole env var, consumed+stripped by `wg-quick`).

### Transport inference (config → emitted `transport=`)

`wg` never reads an explicit `Transport` config key; it **infers** and always emits `transport=`:

- `Endpoint` starts with `ws://`/`wss://` → WS **dialing** peer. `WSMode` **required**. Resolve the URL host → `endpoint=ip:port` (port from the URL); emit `ws_url=<Endpoint>`.
  - `WSMode=wstunnel` → `transport=wstunnel`; `WSTunnelTarget` **required**.
  - `WSMode=websocket` → `transport=websocket`; `WSTunnelTarget` rejected.
- `WSMode=websocket` with **no** `Endpoint` → WS **inbound** peer → `transport=websocket`, no `endpoint`/`ws_url`; `WSTunnelTarget` rejected. (`WSMode=wstunnel` with no `Endpoint` is **rejected** — wstunnel is a client-side dialing mode; inbound/server peers are websocket-only.)
- `Endpoint = host:port` (no ws scheme), no `WSMode` → `transport=udp` (default).
- `Endpoint = host:port` **and** `WSMode` → error. Any `WS*`/per-peer ws key on a udp peer → error (mirrors the daemon "`ws_*` rejected for udp").

### Decisions (design record)

- Reuse `parse_endpoint()` for URL-host resolution → true UDP parity (retries via `WG_ENDPOINT_RESOLUTION_RETRIES`, v4/v6, `[v6]:port`).
- `wg show`/`dump`/`endpoints` render `endpoint` as `ip:port` (parity; what `wg-quick` host-routes). `pretty_print` additionally shows `transport:` (when not udp) and `ws url:` (when set). `WSBearer`/`WSServerBearer` are secrets: emitted by `showconf` for round-trip (like `PresharedKey`) but **never** by `show`/`dump`/`endpoints`.
- `showconf` reconstructs `Endpoint = <ws_url>` + `WSMode` + the `WS*` keys so a shown config round-trips.
- `wg-quick` forces the userspace implementation whenever the config names a WS peer/`WSListen` (the kernel cannot carry WS; `ipc.c` also guards it). **No routing changes.** OpenBSD (no userspace path) dies with a clear message on any WS config.
- **Interop model — `transport=` vs device `ws_*`.** `transport=` is the **mandatory, fork-coordinated** per-peer key: `wireguard-go` v1.3.0 rejects a newly-created peer that omits it ("peer missing mandatory transport"). So `setconf`/`addconf` (which create peers) emit `transport=` for **every** peer, including `udp`. This ties the **userspace UAPI path** to `wireguard-go ≥ 1.3.0` (per project.md, new keys are additive **and coordinated with the sibling fork**); the Linux **kernel/netlink** path is unaffected (it never emits `transport=`). By contrast the **device** `ws_*` keys (`ws_listen`/`ws_server_*`/`ws_trusted_proxies`) are emitted **only when configured** — their `WGDEVICE_HAS_*` flags are NOT default-set in `config_read_init` — so a plain-UDP config adds `transport=udp` and nothing else, never the device WS keys.
- No new dependencies. No wire/format contract changes beyond the additive, v1.3.0-coordinated UAPI keys above.

### Cross-repo boundary (NOT in this plan)

`wireguard-go` (v1.3.0, separate repo) owns `transport=`/`ws_*` semantics, socket dialing, off-tun `SO_MARK`, and the darwin pin removal. This plan MUST NOT modify it. Manual macOS QA is coordinated but the daemon is a given.

### Testing strategy

Unit tests (Unity, in `src/tests/`) for config parsing (inference/validation/resolution seam), the UAPI round-trip over `test_uapi_seam.h`, the kernel guard, and show/showconf. Fuzz coverage for every new parser. The final task runs all quality gates and the **full-tunnel e2e** — Linux container (real `wireguard-go` v1.3.0) + the **macOS manual-QA gate** from `WGQUICK_INTEGRATION.md`.

### Sequential execution order (no item depends on a later item)

US1 model → US2 config parse → US3 UAPI → US4 kernel guard → US5 show/showconf/set → US6 wg-quick → US7 man/completion → US8 unit tests → US9 fuzz → US10 docs → US11 ground-up verification + e2e.

---

## User Story 1 — Device model carries the parity WebSocket fields `[x]`

**Why:** the model must hold a per-peer transport, the resolved endpoint (already present), the
`ws_url`, and the per-peer/device WS settings, with a single free path.

**Acceptance criteria:**
- [x] `struct wgpeer` gains `transport` + WS fields + `WGPEER_HAS_TRANSPORT` flag; `struct wgdevice` gains the device WS fields + `WGDEVICE_HAS_WS_LISTEN` flag.
- [x] `free_wgdevice()` frees every new owned string on every path.

### Task 1.1 — Add fields to `containers.h` `[x]`

- [x] **modify** `src/containers.h` — add the peer transport enum + fields and device fields:
  ```c
  enum wgpeer_transport { WGPEER_TRANSPORT_UDP = 0, WGPEER_TRANSPORT_WEBSOCKET, WGPEER_TRANSPORT_WSTUNNEL };

  /* in struct wgpeer, after persistent_keepalive_interval: */
  	enum wgpeer_transport transport;      /* emitted as transport=udp|websocket|wstunnel */
  	char *ws_url;                         /* ws(s)://host:port/path; NULL for udp/inbound */
  	char *wstunnel_target;                /* host:port; wstunnel dialing only */
  	char *ws_bearer;                      /* secret; never shown by show/dump */
  	char *ws_tls_ca, *ws_tls_cert, *ws_tls_key;  /* file paths */
  	uint32_t ws_ping_interval_ms, ws_backoff_min_ms, ws_backoff_max_ms; /* 0 ⇒ default */
  	bool ws_mask;
  	bool ws_tls_insecure;

  /* peer flags: extend the enum.
     WGPEER_HAS_TRANSPORT gates the transport= UAPI line (an incremental `wg set` that
     does not set it omits transport=, so the daemon keeps the peer's persisted value).
     WGPEER_HAS_WS_SETTINGS is set by any per-peer ws-setting key parse (below), so
     validate_ws_peer can reject a WS key on a UDP peer even when its value is
     false/0 (WSMask=false, WSPingInterval=0, …). */
  	WGPEER_HAS_TRANSPORT   = 1U << 5,
  	WGPEER_HAS_WS_SETTINGS = 1U << 6

  /* device flags: extend the enum. Each device WS key gets a HAS_* flag set ONLY when the
     key is present in the config (or read back by get). Emission is gated on the flag, so a
     plain UDP setconf emits NO ws_* keys (interop-safe with stock/older userspace daemons —
     the UAPI is a compatibility contract). To CLEAR a device WS key, set it explicitly empty
     (e.g. `WSListen=`), which emits `ws_listen=` (empty) — the daemon accepts empty as a
     clear. These flags are NOT default-set in config_read_init (unlike listen_port/fwmark),
     precisely to avoid emitting fork-only keys on a non-WS setconf. */
  	WGDEVICE_HAS_WS_LISTEN            = 1U << 5,
  	WGDEVICE_HAS_WS_SERVER_TLS_CERT   = 1U << 6,
  	WGDEVICE_HAS_WS_SERVER_TLS_KEY    = 1U << 7,
  	WGDEVICE_HAS_WS_SERVER_BEARER     = 1U << 8,
  	WGDEVICE_HAS_WS_TRUSTED_PROXIES   = 1U << 9

  /* in struct wgdevice, after listen_port: */
  	char *ws_listen;                      /* ws(s)://host:port/path; gated by WGDEVICE_HAS_WS_LISTEN */
  	char *ws_server_tls_cert, *ws_server_tls_key;
  	char *ws_server_bearer;               /* secret; never shown by show/dump */
  	char *ws_trusted_proxies;             /* comma-separated CIDRs, passed through verbatim */
  ```
  Add `#include <stdbool.h>`. In `free_wgdevice()`, before `free(peer)` free `peer->ws_url`,
  `wstunnel_target`, `ws_bearer`, `ws_tls_ca`, `ws_tls_cert`, `ws_tls_key`; before `free(dev)` free
  `dev->ws_listen`, `ws_server_tls_cert`, `ws_server_tls_key`, `ws_server_bearer`,
  `ws_trusted_proxies`.

**US1 DoD:** `struct wgpeer`/`struct wgdevice` carry the parity fields and flags; `free_wgdevice`
frees every new owned string on every path. (URL-parsing helpers live in `config.c`, not the
key-encoding header — see US2 Task 2.1, per `c.md` header-cohesion.)

---

## User Story 2 — Parse the WebSocket keys from config files and CLI `[x]`

**Why:** `wg setconf`/`addconf`/`set` must accept the config-file + CLI surface, infer the
transport, resolve the URL to `endpoint=ip:port`, and validate per the inference rules.

**Acceptance criteria:**
- [x] `[Peer]`/`[Interface]` WS keys parse from config files and the equivalent CLI tokens.
- [x] Inference + validation exactly as specified (dialing/inbound/udp, `WSMode` required, wstunnel needs target, ws-on-udp rejected).
- [x] The URL host resolves through `parse_endpoint()`; the whole `Endpoint` URL is stored as `ws_url`.
- [x] Every parse failure prints the offending value (quoted) and expectation; secrets are never printed.

### Task 2.1 — Parse helpers + validation in `config.c` `[x]`

- [x] **modify** `src/config.c` — add the URL helpers as `static` functions here (NOT in the key-encoding `encoding.h`; libc-only, bounds-safe):
  ```c
  static bool is_ws_url(const char *v)
  {
  	return !strncasecmp(v, "ws://", 5) || !strncasecmp(v, "wss://", 6);
  }

  /* Split ws(s)://host:port/path -> host (IPv6 literal unwrapped from [...]) + port.
   * Rejects missing scheme/host/:port and oversize. No allocation. */
  static bool ws_url_split(const char *url, char *host, size_t hostsz, char *port, size_t portsz)
  {
  	const char *p, *hstart, *hend, *pstart, *pend;
  	if (!strncasecmp(url, "wss://", 6)) p = url + 6;
  	else if (!strncasecmp(url, "ws://", 5)) p = url + 5;
  	else return false;
  	if (*p == '[') {                                  /* [v6]:port */
  		hstart = p + 1; hend = strchr(hstart, ']');
  		if (!hend || hend[1] != ':') return false;
  		pstart = hend + 2;
  	} else {
  		const char *colon = NULL;
  		for (const char *q = p; *q && *q != '/'; ++q) if (*q == ':') colon = q;
  		if (!colon) return false;
  		hstart = p; hend = colon; pstart = colon + 1;
  	}
  	for (pend = pstart; *pend && *pend != '/'; ++pend) ;
  	if (hend == hstart || pend == pstart) return false;                 /* empty host/port */
  	if ((size_t)(hend - hstart) >= hostsz || (size_t)(pend - pstart) >= portsz) return false;
  	memcpy(host, hstart, hend - hstart); host[hend - hstart] = '\0';
  	memcpy(port, pstart, pend - pstart); port[pend - pstart] = '\0';
  	return true;
  }
  ```
- [x] **modify** `src/config.c` — add the value helpers. Non-secret helpers quote the offending value on error; the **bearer** helpers NEVER include the value in any message (resolves the quote-vs-secret conflict):
  ```c
  static bool parse_ws_endpoint(struct wgpeer *peer, const char *url)
  {
  	char host[256], port[16], hostport[300];
  	if (!ws_url_split(url, host, sizeof host, port, sizeof port)) {
  		fprintf(stderr, "Endpoint is not a valid ws(s):// URL: `%s'\n", url);
  		return false;
  	}
  	if (strchr(host, ':'))                                  /* re-bracket IPv6 literal */
  		snprintf(hostport, sizeof hostport, "[%s]:%s", host, port);
  	else
  		snprintf(hostport, sizeof hostport, "%s:%s", host, port);
  	if (!parse_endpoint(&peer->endpoint.addr, hostport))
  		return false;
  	free(peer->ws_url);
  	if (!(peer->ws_url = strdup(url))) { perror("strdup"); return false; }
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

  static bool parse_ws_bool(bool *dst, const char *value)
  {
  	if (!strcmp(value, "true")) *dst = true;
  	else if (!strcmp(value, "false")) *dst = false;
  	else { fprintf(stderr, "Expected true or false, got: `%s'\n", value); return false; }
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
  	if (!*value) { fprintf(stderr, "Value is empty\n"); return false; }
  	free(*dst);
  	if (!(*dst = strdup(value))) { perror("strdup"); return false; }
  	return true;
  }

  /* WSListen: empty value CLEARS the listener (emits `ws_listen=` empty, daemon-supported);
   * otherwise must be a ws(s):// URL. Always sets WGDEVICE_HAS_WS_LISTEN so the key is emitted. */
  static bool parse_ws_listen(char **dst, uint32_t *flags, const char *value)
  {
  	if (*value && !is_ws_url(value)) {
  		fprintf(stderr, "WSListen is neither empty nor a ws(s):// URL: `%s'\n", value);
  		return false;
  	}
  	free(*dst);
  	*dst = NULL;
  	if (*value && !(*dst = strdup(value))) { perror("strdup"); return false; }
  	*flags |= WGDEVICE_HAS_WS_LISTEN;
  	return true;
  }

  /* Device WS string keys (WSServerTLSCert/Key/Bearer, WSTrustedProxies): empty value CLEARS the
   * field (free+NULL) and still sets its HAS_* flag so `key=` (empty) is emitted (the daemon accepts
   * empty as a clear); non-empty is stored verbatim. Never prints the value (safe for the secret
   * WSServerBearer). WSTrustedProxies is passed verbatim; the daemon validates the CIDR list. */
  static bool parse_ws_device_str(char **dst, uint32_t *flags, uint32_t bit, const char *value)
  {
  	free(*dst);
  	*dst = NULL;
  	if (*value && !(*dst = strdup(value))) { perror("strdup"); return false; }
  	*flags |= bit;
  	return true;
  }

  /* WSTunnelTarget: validate host:port shape and store it VERBATIM — the tools MUST NOT
   * DNS-resolve it (the wstunnel relay resolves the inner target). NOT parse_endpoint. */
  static bool parse_ws_target(char **dst, const char *value)
  {
  	const char *colon = strrchr(value, ':');
  	bool bad = !colon || colon == value || !colon[1];
  	for (const char *p = colon ? colon + 1 : ""; !bad && *p; ++p)
  		bad = *p < '0' || *p > '9';
  	if (bad) { fprintf(stderr, "WSTunnelTarget is not in host:port form: `%s'\n", value); return false; }
  	free(*dst);
  	if (!(*dst = strdup(value))) { perror("strdup"); return false; }
  	return true;
  }

  /* Inline bearer (config file) — validates non-empty; NEVER prints the value. */
  static bool parse_ws_secret(char **dst, const char *value)
  {
  	if (!*value) { fprintf(stderr, "A WebSocket bearer value is empty\n"); return false; }
  	free(*dst);
  	if (!(*dst = strdup(value))) { perror("strdup"); return false; }
  	return true;
  }

  /* CLI bearer — read the token from a FILE so the secret never sits in argv (parity with
   * private-key/preshared-key). Bounded read; strips one trailing newline; the value is
   * NEVER printed (errors reference the PATH); the stack copy is wiped. */
  static bool parse_ws_secret_file(char **dst, const char *path)
  {
  	char buf[8192];
  	FILE *f = fopen(path, "r");
  	size_t len;
  	if (!f) { fprintf(stderr, "Unable to open `%s': %s\n", path, strerror(errno)); return false; }
  	if (!fgets(buf, sizeof buf, f)) { fclose(f); fprintf(stderr, "Unable to read bearer from `%s'\n", path); return false; }
  	len = strlen(buf);
  	if (len && buf[len - 1] == '\n')
  		buf[--len] = '\0';
  	else if (len == sizeof buf - 1 && fgetc(f) != EOF) {   /* buffer full + more pending -> truncated */
  		fclose(f); memset(buf, 0, sizeof buf);
  		fprintf(stderr, "Bearer in `%s' is too long (max %zu bytes)\n", path, sizeof buf - 1);
  		return false;
  	}
  	fclose(f);
  	if (!len) { fprintf(stderr, "Bearer file `%s' is empty\n", path); return false; }
  	free(*dst);
  	*dst = strdup(buf);
  	memset(buf, 0, sizeof buf);
  	if (!*dst) { perror("strdup"); return false; }
  	return true;
  }
  ```
- [x] **modify** `src/config.c` — add `validate_ws_peer`, called per peer at `config_read_finish` and at `config_read_cmd` completion:
  ```c
  static bool validate_ws_peer(const struct wgpeer *peer)
  {
  	bool has_ep = peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6;
  	/* Incremental CLI update (transport NOT declared this command): the peer's real transport
  	   is whatever the daemon persisted, unknown here — defer to the daemon, do not local-reject
  	   a lone `wg set … ws-bearer …`. (Config-file peers always set WGPEER_HAS_TRANSPORT.) */
  	if (!(peer->flags & WGPEER_HAS_TRANSPORT))
  		return true;
  	if (peer->transport == WGPEER_TRANSPORT_UDP) {
  		if (peer->ws_url) { fprintf(stderr, "A ws(s):// Endpoint requires WSMode\n"); return false; }
  		/* presence flag catches false/0-valued ws keys (WSMask=false, WSPingInterval=0, …) too */
  		if (peer->flags & WGPEER_HAS_WS_SETTINGS) { fprintf(stderr, "A UDP peer has WebSocket settings\n"); return false; }
  		return true;
  	}
  	if (peer->ws_url) {                                  /* dialing peer */
  		if (!has_ep) { fprintf(stderr, "A dialing WebSocket peer has no resolved endpoint\n"); return false; }
  		if (peer->transport == WGPEER_TRANSPORT_WSTUNNEL && !peer->wstunnel_target) {
  			fprintf(stderr, "WSMode=wstunnel requires WSTunnelTarget\n"); return false;
  		}
  	} else {                                            /* inbound peer (server side): WS carrier only */
  		if (has_ep) { fprintf(stderr, "An inbound WebSocket peer must not set an Endpoint\n"); return false; }
  		/* wstunnel is a client-only dialing mode; an inbound peer cannot be wstunnel */
  		if (peer->transport == WGPEER_TRANSPORT_WSTUNNEL) {
  			fprintf(stderr, "WSMode=wstunnel requires a ws(s):// Endpoint (wstunnel is a dialing mode)\n"); return false;
  		}
  		if (peer->wstunnel_target) { fprintf(stderr, "An inbound WebSocket peer must not set WSTunnelTarget\n"); return false; }
  	}
  	if (peer->transport == WGPEER_TRANSPORT_WEBSOCKET && peer->wstunnel_target) {
  		fprintf(stderr, "WSTunnelTarget requires WSMode=wstunnel\n"); return false;
  	}
  	return true;
  }
  ```

### Task 2.2 — Wire keys into `process_line` + `config_read_cmd` `[x]`

Transport-emission gating (`WGPEER_HAS_TRANSPORT`) — the rule that keeps incremental `wg set` from
clobbering a persisted transport (US3):
- **Config file** (`setconf`/`addconf`, `process_line`): a config file fully declares each peer, so set `WGPEER_HAS_TRANSPORT` for **every** `[Peer]` (transport is mandatory at creation). The transport value is inferred: default `UDP`; `WSMode` (or a `ws(s)://` Endpoint) selects websocket/wstunnel.
- **CLI** (`config_read_cmd`): set `WGPEER_HAS_TRANSPORT` for a peer only when the transport is (re)declared in this command — i.e. a `ws-mode`/`ws(s)://` endpoint token, **or** any `endpoint` token (a bare `ip:port` declares `udp`). A pure incremental update (e.g. `wg set wg0 peer <X> persistent-keepalive 25`) sets no endpoint/ws token → `WGPEER_HAS_TRANSPORT` stays clear → `transport=` is omitted → the daemon keeps the persisted transport.

- [x] **modify** `src/config.c` `process_line` — dispatch the keys. The `key_match(key)` macro assigns `value = get_value(line, key "=")`, but `get_value` returns NULL for an EMPTY value (`keylen >= linelen`), so every device WS key that must accept an empty value (to clear) is dispatched with the `strncasecmp` form instead (like the existing empty-tolerant keys). Set `WGPEER_HAS_TRANSPORT` when a `[Peer]` `PublicKey` line starts a peer (config-file peers always emit transport):
  ```c
  /* [Interface] section — device is ctx->device. Each device WS key is dispatched by strncasecmp
     (so an empty value CLEARS it via parse_ws_device_str / parse_ws_listen) and sets its HAS_* flag
     ONLY when present. The flags are NOT default-set in config_read_init, so an OMITTED device WS key
     is never emitted and does not clear (interop-safe); to clear one, set it explicitly empty. */
  } else if (!strncasecmp(line, "WSListen=", sizeof("WSListen=") - 1)) {
  	ret = parse_ws_listen(&ctx->device->ws_listen, &ctx->device->flags, line + sizeof("WSListen=") - 1);
  } else if (!strncasecmp(line, "WSServerTLSCert=", sizeof("WSServerTLSCert=") - 1)) {
  	ret = parse_ws_device_str(&ctx->device->ws_server_tls_cert, &ctx->device->flags, WGDEVICE_HAS_WS_SERVER_TLS_CERT, line + sizeof("WSServerTLSCert=") - 1);
  } else if (!strncasecmp(line, "WSServerTLSKey=", sizeof("WSServerTLSKey=") - 1)) {
  	ret = parse_ws_device_str(&ctx->device->ws_server_tls_key, &ctx->device->flags, WGDEVICE_HAS_WS_SERVER_TLS_KEY, line + sizeof("WSServerTLSKey=") - 1);
  } else if (!strncasecmp(line, "WSServerBearer=", sizeof("WSServerBearer=") - 1)) {
  	ret = parse_ws_device_str(&ctx->device->ws_server_bearer, &ctx->device->flags, WGDEVICE_HAS_WS_SERVER_BEARER, line + sizeof("WSServerBearer=") - 1); /* never prints the value */
  } else if (!strncasecmp(line, "WSTrustedProxies=", sizeof("WSTrustedProxies=") - 1)) {
  	ret = parse_ws_device_str(&ctx->device->ws_trusted_proxies, &ctx->device->flags, WGDEVICE_HAS_WS_TRUSTED_PROXIES, line + sizeof("WSTrustedProxies=") - 1);
  }
  /* MetricsListen is consumed by wg-quick, NOT a wg key — no case here. */

  /* [Peer] section — the existing branch operates on ctx->last_peer; alias it: */
  struct wgpeer *peer = ctx->last_peer;
  peer->flags |= WGPEER_HAS_TRANSPORT;   /* set (idempotently) on every config-file peer line — a config file fully declares each peer, so transport= is always emitted for it */
  if (key_match("Endpoint")) {
  	ret = is_ws_url(value) ? parse_ws_endpoint(peer, value)
  	                       : parse_endpoint(&peer->endpoint.addr, value);
  } else if (key_match("WSMode")) {
  	ret = parse_ws_mode(&peer->transport, value);   /* sets transport, NOT a "ws setting" */
  } else if (key_match("WSTunnelTarget")) {
  	if ((ret = parse_ws_target(&peer->wstunnel_target, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSBearer")) {
  	if ((ret = parse_ws_secret(&peer->ws_bearer, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSMask")) {
  	if ((ret = parse_ws_bool(&peer->ws_mask, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSTLSCA")) {
  	if ((ret = parse_ws_str(&peer->ws_tls_ca, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSTLSCert")) {
  	if ((ret = parse_ws_str(&peer->ws_tls_cert, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSTLSKey")) {
  	if ((ret = parse_ws_str(&peer->ws_tls_key, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSTLSInsecure")) {
  	if ((ret = parse_ws_bool(&peer->ws_tls_insecure, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSPingInterval")) {
  	if ((ret = parse_ws_millis(&peer->ws_ping_interval_ms, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSBackoffMin")) {
  	if ((ret = parse_ws_millis(&peer->ws_backoff_min_ms, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  } else if (key_match("WSBackoffMax")) {
  	if ((ret = parse_ws_millis(&peer->ws_backoff_max_ms, value))) peer->flags |= WGPEER_HAS_WS_SETTINGS;
  }
  ```
- [x] **modify** `src/config.c` `config_read_cmd` — add the CLI tokens (peer: `ws-mode`, `wstunnel-target`, `ws-bearer <file>`, `ws-mask`, `ws-tls-ca`, `ws-tls-cert`, `ws-tls-key`, `ws-tls-insecure`, `ws-ping-interval`, `ws-backoff-min`, `ws-backoff-max`; interface: `ws-listen`, `ws-server-tls-cert`, `ws-server-tls-key`, `ws-server-bearer <file>`, `ws-trusted-proxies`), each mapping to the same helper as its config key. The device string tokens (`ws-listen`/`ws-server-tls-cert`/`ws-server-tls-key`/`ws-trusted-proxies`) route through `parse_ws_listen`/`parse_ws_device_str` and accept an **empty** value to clear (setting the `WGDEVICE_HAS_*` flag). Secrets (`ws-bearer`/`ws-server-bearer`) take a **file path** read via `parse_ws_secret_file` (never inline in `argv` — parity with `private-key`/`preshared-key`); `ws-server-bearer` sets `WGDEVICE_HAS_WS_SERVER_BEARER` (the server bearer is cleared via a config-file `WSServerBearer=`). `endpoint <ws-url>` routes through `parse_ws_endpoint`. Set the transport gate only when the transport is (re)declared:
  ```c
  /* on `endpoint <val>` for the current peer: */
  peer->flags |= WGPEER_HAS_TRANSPORT;   /* an endpoint (udp ip:port OR ws url) declares transport */
  ret = is_ws_url(value) ? parse_ws_endpoint(peer, value) : parse_endpoint(&peer->endpoint.addr, value);
  /* on `ws-mode <val>`: */
  peer->flags |= WGPEER_HAS_TRANSPORT;
  ret = parse_ws_mode(&peer->transport, value);
  /* a pure incremental token (e.g. persistent-keepalive) sets NO endpoint/ws-mode, so
     WGPEER_HAS_TRANSPORT stays clear and transport= is omitted (daemon keeps persisted). */
  ```
  Run `validate_ws_peer` on each peer before returning.
- [x] **modify** `src/config.c` `config_read_finish` — WS keys span multiple lines, so validate at finish (the `setconf`/`addconf`/`syncconf` path). Extend the existing `for_each_wgpeer` loop:
  ```c
  for_each_wgpeer(ctx->device, peer) {
  	if (!(peer->flags & WGPEER_HAS_PUBLIC_KEY)) {
  		fprintf(stderr, "A peer is missing a public key\n");
  		goto err;
  	}
  	if (!validate_ws_peer(peer))     /* NEW: reject invalid WS combinations */
  		goto err;
  }
  ```
- [x] **do NOT modify** `src/config.c` `config_read_init` — the device WS flags MUST NOT be added to the `!append` default set. Default-setting them would emit `ws_listen=`/`ws_server_*=` (empty) on **every** plain-UDP `setconf`/`syncconf`, which (a) breaks UAPI interop with stock/older userspace daemons that reject unknown `set=1` keys, and (b) would make the kernel-guard trip on every UDP config. Device WS keys are therefore emitted ONLY when actually present (flag set by parse). To clear one, the user sets it explicitly empty (e.g. `WSListen=`); clearing by omission is deliberately not supported (interop-safe).

**US2 DoD:** config files and CLI produce a correct model; invalid combinations are rejected with actionable, secret-free messages.

---

## User Story 3 — Serialize/parse the WebSocket keys over the UAPI socket `[x]`

**Why:** `set=1` must emit the v1.3.0 keys; `get=1` must read them back so `showconf`/`show`
round-trip.

**Acceptance criteria:**
- [x] `userspace_set_device` emits `transport=` for a peer **only when `WGPEER_HAS_TRANSPORT` is set** (so an incremental `set` that omits it keeps the daemon's persisted transport), `endpoint=ip:port` (existing block), and the WS peer/device keys when present.
- [x] `userspace_get_device` parses `transport=`, `ws_url=`, all `ws_*`, and the device keys back into the model (free-before-strdup, ENOMEM-checked); a parsed `transport=` sets `WGPEER_HAS_TRANSPORT`.

### Task 3.1 — Emit in `userspace_set_device` (`ipc-uapi.h`) `[x]`

Emitting `ws_mask`/`ws_tls_insecure` only when **true** (never `=false`) exactly mirrors the daemon's
own `get=1` (`WSPeerKVs()` in wireguard-go v1.3.0 emits `ws_mask=true`/`ws_tls_insecure=true` only
when set) — so `false` is represented by absence on both sides and there is no round-trip
asymmetry; no `HAS_*` bool flag is needed.

- [x] **modify** `src/ipc-uapi.h` `userspace_set_device`:
  ```c
  /* device block, after the existing fwmark line. Each key is gated on its HAS_* flag (set ONLY
     when the key was present in config or read by get — NOT default-set in config_read_init) and
     emitted value-or-empty. An OMITTED device WS key is not emitted and does not clear (unlike
     listen_port); to clear one, set it explicitly empty (e.g. WSListen=), which emits `ws_listen=`. */
  if (dev->flags & WGDEVICE_HAS_WS_LISTEN)          fprintf(f, "ws_listen=%s\n", dev->ws_listen ? dev->ws_listen : "");
  if (dev->flags & WGDEVICE_HAS_WS_SERVER_TLS_CERT) fprintf(f, "ws_server_tls_cert=%s\n", dev->ws_server_tls_cert ? dev->ws_server_tls_cert : "");
  if (dev->flags & WGDEVICE_HAS_WS_SERVER_TLS_KEY)  fprintf(f, "ws_server_tls_key=%s\n", dev->ws_server_tls_key ? dev->ws_server_tls_key : "");
  if (dev->flags & WGDEVICE_HAS_WS_SERVER_BEARER)   fprintf(f, "ws_server_bearer=%s\n", dev->ws_server_bearer ? dev->ws_server_bearer : "");
  if (dev->flags & WGDEVICE_HAS_WS_TRUSTED_PROXIES) fprintf(f, "ws_trusted_proxies=%s\n", dev->ws_trusted_proxies ? dev->ws_trusted_proxies : "");

  /* peer block: transport gated on the flag (incremental-set safety) */
  if (peer->flags & WGPEER_HAS_TRANSPORT)
  	fprintf(f, "transport=%s\n",
  	        peer->transport == WGPEER_TRANSPORT_WSTUNNEL  ? "wstunnel"  :
  	        peer->transport == WGPEER_TRANSPORT_WEBSOCKET ? "websocket" : "udp");
  /* ... the EXISTING endpoint=ip:port getnameinfo block is kept unchanged ... */
  /* Emit the ws_* keys for a WS peer, OR for an incremental `wg set … ws-<key>` that sets a
     ws setting without (re)declaring transport (HAS_WS_SETTINGS set, HAS_TRANSPORT clear) —
     the daemon validates it against the peer's persisted transport. */
  if (peer->transport != WGPEER_TRANSPORT_UDP ||
      ((peer->flags & WGPEER_HAS_WS_SETTINGS) && !(peer->flags & WGPEER_HAS_TRANSPORT))) {
  	if (peer->ws_url)          fprintf(f, "ws_url=%s\n", peer->ws_url);
  	if (peer->wstunnel_target) fprintf(f, "wstunnel_target=%s\n", peer->wstunnel_target);
  	if (peer->ws_bearer)       fprintf(f, "ws_bearer=%s\n", peer->ws_bearer);
  	if (peer->ws_mask)         fprintf(f, "ws_mask=true\n");
  	if (peer->ws_tls_ca)       fprintf(f, "ws_tls_ca=%s\n", peer->ws_tls_ca);
  	if (peer->ws_tls_cert)     fprintf(f, "ws_tls_cert=%s\n", peer->ws_tls_cert);
  	if (peer->ws_tls_key)      fprintf(f, "ws_tls_key=%s\n", peer->ws_tls_key);
  	if (peer->ws_tls_insecure) fprintf(f, "ws_tls_insecure=true\n");
  	if (peer->ws_ping_interval_ms) fprintf(f, "ws_ping_interval=%u\n", peer->ws_ping_interval_ms);
  	if (peer->ws_backoff_min_ms)   fprintf(f, "ws_backoff_min=%u\n", peer->ws_backoff_min_ms);
  	if (peer->ws_backoff_max_ms)   fprintf(f, "ws_backoff_max=%u\n", peer->ws_backoff_max_ms);
  }
  ```

### Task 3.2 — Parse in `userspace_get_device` (`ipc-uapi.h`) `[x]`

- [x] **modify** `src/ipc-uapi.h` `userspace_get_device` — add key branches alongside the existing `endpoint=` sockaddr parse. String fields use the file's existing owned-string idiom (free-before-strdup; on `strdup` failure `ret = -ENOMEM; goto err;` — match the surrounding code's exact error label):
  ```c
  else if (peer && !strcmp(key, "transport")) {
  	peer->transport = !strcmp(value, "wstunnel")  ? WGPEER_TRANSPORT_WSTUNNEL  :
  	                  !strcmp(value, "websocket") ? WGPEER_TRANSPORT_WEBSOCKET : WGPEER_TRANSPORT_UDP;
  	peer->flags |= WGPEER_HAS_TRANSPORT;
  } else if (peer && !strcmp(key, "ws_url")) {
  	free(peer->ws_url); peer->ws_url = strdup(value); if (!peer->ws_url) { ret = -ENOMEM; goto err; }
  } else if (peer && !strcmp(key, "wstunnel_target")) { /* free-before-strdup … */ }
  else if (peer && !strcmp(key, "ws_bearer"))    { /* free-before-strdup … */ }
  else if (peer && !strcmp(key, "ws_tls_ca"))    { /* … */ }
  else if (peer && !strcmp(key, "ws_tls_cert"))  { /* … */ }
  else if (peer && !strcmp(key, "ws_tls_key"))   { /* … */ }
  else if (peer && !strcmp(key, "ws_mask"))          peer->ws_mask = !strcmp(value, "true");
  else if (peer && !strcmp(key, "ws_tls_insecure"))  peer->ws_tls_insecure = !strcmp(value, "true");
  /* millis: use the file's bounded NUM() macro (rejects trailing garbage / overflow -> -EPROTO), NOT bare strtoul */
  else if (peer && !strcmp(key, "ws_ping_interval")) peer->ws_ping_interval_ms = NUM(0xffffffffU);
  else if (peer && !strcmp(key, "ws_backoff_min"))   peer->ws_backoff_min_ms   = NUM(0xffffffffU);
  else if (peer && !strcmp(key, "ws_backoff_max"))   peer->ws_backoff_max_ms   = NUM(0xffffffffU);
  else if (!peer && !strcmp(key, "ws_listen")) {
  	free(dev->ws_listen); dev->ws_listen = strdup(value);
  	if (!dev->ws_listen) { ret = -ENOMEM; goto err; }
  	dev->flags |= WGDEVICE_HAS_WS_LISTEN;
  } else if (!peer && !strcmp(key, "ws_server_tls_cert")) { /* free-before-strdup; set WGDEVICE_HAS_WS_SERVER_TLS_CERT */ }
  else if (!peer && !strcmp(key, "ws_server_tls_key"))    { /* free-before-strdup; set WGDEVICE_HAS_WS_SERVER_TLS_KEY */ }
  else if (!peer && !strcmp(key, "ws_server_bearer"))     { /* free-before-strdup; set WGDEVICE_HAS_WS_SERVER_BEARER */ }
  else if (!peer && !strcmp(key, "ws_trusted_proxies"))   { /* free-before-strdup; set WGDEVICE_HAS_WS_TRUSTED_PROXIES */ }
  ```
  Each device WS branch sets its `WGDEVICE_HAS_*` flag (like `ws_listen`) so a device read back via `get=1` round-trips these keys on a subsequent `setconf`.

**US3 DoD:** a set→get round-trip over the seam reproduces every field.

---

## User Story 4 — Reject WebSocket settings on a kernel interface `[x]`

**Why:** the kernel backend cannot carry WS; a WS config on a kernel interface must fail fast, not
silently drop settings.

**Acceptance criteria:**
- [x] Inside `#ifdef IPC_SUPPORTS_KERNEL_INTERFACE`, in the kernel branch of **`ipc_set_device` only**, before `kernel_set_device`, a WS config (any peer transport≠udp, or any device WS field/flag) returns `EOPNOTSUPP`. (`ipc_get_device` needs no guard: it *reads* a kernel device, whose out-param is unpopulated, and a kernel interface can never hold WS settings.)

### Task 4.1 — Guard in `ipc.c` `[x]`

- [x] **modify** `src/ipc.c` — add the helper and guard **inside** the existing `#ifdef IPC_SUPPORTS_KERNEL_INTERFACE` (so it is not compiled — and does not warn as unused — where there is no kernel backend). Guard **only** `ipc_set_device`'s kernel branch (NOT `ipc_get_device`, which would dereference the unpopulated out-param):
  ```c
  static bool device_has_ws_settings(const struct wgdevice *dev)
  {
  	/* Test VALUES, not flags: an empty/cleared device WS key (flag set, value "") is NOT a
  	   WS config, and the flags are not default-set — so this fires only on real WS content. */
  	if ((dev->ws_listen && *dev->ws_listen) || dev->ws_server_tls_cert || dev->ws_server_tls_key ||
  	    dev->ws_server_bearer || dev->ws_trusted_proxies)
  		return true;
  	for (struct wgpeer *peer = dev->first_peer; peer; peer = peer->next_peer)
  		if (peer->transport != WGPEER_TRANSPORT_UDP)
  			return true;
  	return false;
  }
  /* in ipc_set_device, kernel branch, before kernel_set_device(dev): */
  if (device_has_ws_settings(dev)) {
  	errno = EOPNOTSUPP;
  	return -EOPNOTSUPP;
  }
  ```

**US4 DoD:** `wg setconf` of a WS config on a kernel interface fails with a clear error; userspace path is unaffected.

---

## User Story 5 — Render WebSocket settings in `show`, `showconf`, and `set` usage `[x]`

**Why:** humans and `wg-quick`/`showconf` must see the right thing: routable `ip:port` for
machines, a round-trippable config from `showconf`, secrets never leaked.

**Acceptance criteria:**
- [x] `wg show` `endpoints`/`dump` keep `endpoint=ip:port`; `pretty_print` adds `transport:` (non-udp) and `ws url:` (when set); bearers never printed.
- [x] `showconf` emits `WSMode` + the `WS*` keys for **every** WS peer — including an **inbound** peer (`transport != udp` with no `ws_url`) so it round-trips as WS, not UDP; `Endpoint = <ws_url>` only when `ws_url` is set; device `WSListen`/`WSServer*`/`WSTrustedProxies`.
- [x] `wg set` usage lists the new tokens.

### Task 5.1 — `show.c` `[x]`
- [x] **modify** `src/show.c` `pretty_print` — after the existing `endpoint` line (`dump_print`/`endpoints` unchanged: `ip:port` only; never print `ws_bearer`):
  ```c
  if (peer->transport != WGPEER_TRANSPORT_UDP)
  	terminal_printf("  " TERMINAL_BOLD "transport" TERMINAL_RESET ": %s\n",
  	                peer->transport == WGPEER_TRANSPORT_WSTUNNEL ? "wstunnel" : "websocket");
  if (peer->ws_url)
  	terminal_printf("  " TERMINAL_BOLD "ws url" TERMINAL_RESET ": %s\n", peer->ws_url);
  ```

### Task 5.2 — `showconf.c` `[x]`
- [x] **modify** `src/showconf.c`:
  ```c
  /* device block, after ListenPort/FwMark. showconf_main's variable is `device`; gate on a
     NON-EMPTY value so a cleared key (flag set, value "") is omitted from the regenerated config. */
  if (device->ws_listen && *device->ws_listen)                 printf("WSListen = %s\n", device->ws_listen);
  if (device->ws_server_tls_cert && *device->ws_server_tls_cert) printf("WSServerTLSCert = %s\n", device->ws_server_tls_cert);
  if (device->ws_server_tls_key && *device->ws_server_tls_key)   printf("WSServerTLSKey = %s\n", device->ws_server_tls_key);
  if (device->ws_server_bearer && *device->ws_server_bearer)     printf("WSServerBearer = %s\n", device->ws_server_bearer); /* round-trip, like PresharedKey */
  if (device->ws_trusted_proxies && *device->ws_trusted_proxies) printf("WSTrustedProxies = %s\n", device->ws_trusted_proxies);

  /* peer: gate on transport (covers dialing AND inbound); Endpoint line only when ws_url set. */
  if (peer->transport != WGPEER_TRANSPORT_UDP) {
  	printf("WSMode = %s\n", peer->transport == WGPEER_TRANSPORT_WSTUNNEL ? "wstunnel" : "websocket");
  	if (peer->ws_url)          printf("Endpoint = %s\n", peer->ws_url);   /* dialing; inbound omits it */
  	if (peer->wstunnel_target) printf("WSTunnelTarget = %s\n", peer->wstunnel_target);
  	if (peer->ws_bearer)       printf("WSBearer = %s\n", peer->ws_bearer);
  	if (peer->ws_mask)         printf("WSMask = true\n");
  	if (peer->ws_tls_ca)       printf("WSTLSCA = %s\n", peer->ws_tls_ca);
  	if (peer->ws_tls_cert)     printf("WSTLSCert = %s\n", peer->ws_tls_cert);
  	if (peer->ws_tls_key)      printf("WSTLSKey = %s\n", peer->ws_tls_key);
  	if (peer->ws_tls_insecure) printf("WSTLSInsecure = true\n");
  	if (peer->ws_ping_interval_ms) printf("WSPingInterval = %u\n", peer->ws_ping_interval_ms);
  	if (peer->ws_backoff_min_ms)   printf("WSBackoffMin = %u\n", peer->ws_backoff_min_ms);
  	if (peer->ws_backoff_max_ms)   printf("WSBackoffMax = %u\n", peer->ws_backoff_max_ms);
  } else if (peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6) {
  	/* ... the EXISTING `Endpoint = ip:port` block for UDP peers, unchanged ... */
  }
  ```
  An inbound peer (`transport != udp`, no `ws_url`) thus round-trips as `WSMode = websocket` with no `Endpoint` — transport preserved.

### Task 5.3 — `set.c` usage `[x]`
- [x] **modify** `src/set.c` — extend the usage string with the peer tokens (`[ws-mode <websocket|wstunnel>] [wstunnel-target <host:port>] [ws-bearer <file>] [ws-mask <true|false>] [ws-tls-ca <file>] [ws-tls-cert <file>] [ws-tls-key <file>] [ws-tls-insecure <true|false>] [ws-ping-interval <ms>] [ws-backoff-min <ms>] [ws-backoff-max <ms>]`) and the interface tokens (`[ws-listen <ws-url>] [ws-server-tls-cert <file>] [ws-server-tls-key <file>] [ws-server-bearer <file>] [ws-trusted-proxies <cidr,...>]`), noting `endpoint` accepts a `ws(s)://` URL.

**US5 DoD:** `showconf | setconf` round-trips a WS config; machine outputs stay `ip:port`; no secret leaks in `show`/`dump`.

---

## User Story 6 — `wg-quick`: select userspace for WS + export metrics; NO routing change `[x]`

**Why:** the kernel backend cannot carry WS. On **linux/freebsd** `add_if` creates a **kernel**
interface first (userspace only as a fallback on kernel failure), so a WS config must select
userspace **before** creation. **darwin** already always runs `wireguard-go` (`add_if` line
124-127), so it needs no forcing change. **openbsd**'s `add_if` is **kernel-only** (no
`WG_QUICK_USERSPACE_IMPLEMENTATION` path). `MetricsListen` maps to the sole env var; routing is
untouched on every platform (darwin/freebsd host-route the `ip:port` endpoint via
`set_endpoint_direct_route`; linux marks via `fwmark` — all work unchanged now that `endpoint=`
is `ip:port`).

**Acceptance criteria:**
- [x] linux/freebsd: on a WS config (`WSListen`, any `WSMode`, or an `Endpoint = ws(s)://`), `add_if` runs `"${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" "$INTERFACE"` directly and skips the kernel `ip link add`/`ifconfig wg create`.
- [x] darwin: NO `add_if` change (already userspace).
- [x] linux/freebsd/darwin: strip `MetricsListen` from the config `wg` sees and `export WG_METRICS_LISTEN`; `save_config` re-emits the WS keys (automatic via `showconf` once US5 lands).
- [x] openbsd: any WS config `die`s with a clear "WebSocket transport is not supported by wg-quick on OpenBSD" message (kernel-only wg-quick; the `ipc.c` guard is the backstop).
- [x] No `fwmark`/`route`/`rule`/`nft`/`iptables`/`set_endpoint_direct_route` logic is modified on any platform.

### Task 6.1 — linux + freebsd: select userspace on WS `[x]`
- [x] **modify** `src/wg-quick/{linux,freebsd}.bash` — `WSListen`/`WSMode`/a `ws(s)://` `Endpoint` all land in `WG_CONFIG` (they are `wg` keys, not wg-quick-consumed), so detect after `parse_options`:
  ```bash
  # parse_options ends with `shopt -u nocasematch`, but `wg` keys are case-insensitive, so
  # match the WS indicators case-insensitively too (WSMODE=, wslisten=, ENDPOINT=WSS://, …).
  WG_HAS_WS=0
  shopt -s nocasematch
  if [[ $WG_CONFIG =~ (^|$'\n')[[:space:]]*(WSMode|WSListen)[[:space:]]*= ]] ||
     [[ $WG_CONFIG =~ (^|$'\n')[[:space:]]*Endpoint[[:space:]]*=[[:space:]]*wss?:// ]]; then
  	WG_HAS_WS=1
  fi
  shopt -u nocasematch
  ```
  In `add_if`, select userspace up front for a WS config (linux shown; freebsd is the same idea — skip `ifconfig wg create`, run the userspace impl with `$INTERFACE`):
  ```bash
  add_if() {
  	local ret
  	if [[ $WG_HAS_WS -eq 1 ]]; then
  		cmd "${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" "$INTERFACE"
  		return 0
  	fi
  	# ... existing kernel-create + userspace-fallback path, unchanged ...
  }
  ```
  Any new helper that ends on a possibly-false test MUST `return 0` (set -e safety).

### Task 6.2 — MetricsListen env + save_config (linux/freebsd/darwin) `[x]`
- [x] **modify** `src/wg-quick/{linux,freebsd,darwin}.bash` — add a global `METRICS_LISTEN=""`; add a `[Interface]` parse case that captures + strips it (like `MTU`/`Table`), then export it:
  ```bash
  # in parse_options, [Interface] case block:
  MetricsListen) METRICS_LISTEN="$value"; continue ;;
  # after parse_options (before add_if/wg): export only when set
  [[ -z $METRICS_LISTEN ]] || export WG_METRICS_LISTEN="$METRICS_LISTEN"
  ```
- [x] **modify** `save_config()` in the same scripts — re-emit it in the rebuilt `[Interface]`, mirroring `Table`/`MTU` (it is wg-quick-consumed, so `wg showconf` cannot produce it — without this it is lost on `SaveConfig`):
  ```bash
  [[ -z $METRICS_LISTEN ]] || new_config+="MetricsListen = $METRICS_LISTEN"$'\n'
  ```
  The per-peer/interface `WS*` **`wg` keys** (`WSMode`/`WSListen`/`WSServer*`/`WSTrustedProxies`) come from `wg showconf` automatically once US5 lands.

### Task 6.3 — openbsd: die on WS `[x]`
- [x] **modify** `src/wg-quick/openbsd.bash` — after `parse_options`, before `add_if`, detect a WS config and fail fast (OpenBSD wg-quick is kernel-only; the `ipc.c` guard is the backstop):
  ```bash
  shopt -s nocasematch
  if [[ $WG_CONFIG =~ (^|$'\n')[[:space:]]*(WSMode|WSListen)[[:space:]]*= ]] ||
     [[ $WG_CONFIG =~ (^|$'\n')[[:space:]]*Endpoint[[:space:]]*=[[:space:]]*wss?:// ]]; then
  	shopt -u nocasematch
  	die "WebSocket transport is not supported by wg-quick on OpenBSD"
  fi
  shopt -u nocasematch
  ```

### Task 6.4 — MetricsListen must not leak on openbsd + android `[x]`
`MetricsListen` is a wg-quick-consumed `[Interface]` key (not a `wg` UAPI key); a config carrying it
(even with no other WS token) MUST be stripped everywhere the config reaches `wg addconf`, or `wg`
errors with "Line unrecognized". openbsd/android have no userspace daemon, so they consume+ignore it
(no `WG_METRICS_LISTEN` export).
- [x] **modify** `src/wg-quick/openbsd.bash` — in `parse_options`, add the `[Interface]` case `MetricsListen) continue ;;` (strip + ignore) and, when it was present, `echo "[!] MetricsListen is ignored on OpenBSD (kernel backend, no userspace daemon)" >&2`.
- [x] **modify** `src/wg-quick/android.c` — in the `in_interface_section` key chain of `cmd_up`'s config parse (which strips `Address=`/`DNS=`/`MTU=` via `strncasecmp(clean, "KEY=", n)` cases, mirroring the `MTU=` case), drop `MetricsListen` before `*config` is built so it never reaches `set_config`'s `wg addconf` (inert on Android — metrics are the embedding app's concern):
  ```c
  } else if (!strncasecmp(clean, "MetricsListen=", 14)) {
  	continue;
  }
  ```

**US6 DoD:** a WS `wg-quick up` selects userspace (linux/freebsd/darwin) and brings the interface up; a UDP config is byte-for-byte unaffected; `MetricsListen` reaches the daemon as `WG_METRICS_LISTEN` on linux/freebsd/darwin and is stripped (not leaked to `wg`) on openbsd/android; OpenBSD WS fails fast with a clear message.

---

## User Story 7 — Document the keys in man pages and completion `[x]`

**Acceptance criteria:**
- [x] `wg.8` documents the `[Peer]`/`[Interface]` `WS*` keys and the `wg set` tokens; `wg-quick.8` documents `MetricsListen` + the userspace-forcing behavior.
- [x] bash completion offers the new `wg set` tokens.

### Task 7.1 — man `[x]`
- [x] **modify** `src/man/wg.8` — add the `WS*` config keys (peer + interface) and the `wg set` WS tokens, noting `Endpoint` accepts a `ws(s)://` URL and `WSMode` selects websocket/wstunnel. For `WSBearer`, describe it as the authentication token passed to the server and note its exact HTTP scheme is transport-dependent and applied by the daemon (see the `wireguard-go` documentation) — do NOT assert the precise header format as this tool's contract.
- [x] **modify** `src/man/wg-quick.8` — document `MetricsListen` (→ `WG_METRICS_LISTEN`) and that a WS config forces the userspace implementation (and is unsupported on OpenBSD).

### Task 7.2 — completion `[x]`
- [x] **modify** `src/completion/wg.bash-completion` — add the new `wg set` tokens to the peer/interface keyword lists: `ws-mode wstunnel-target ws-bearer ws-mask ws-tls-ca ws-tls-cert ws-tls-key ws-tls-insecure ws-ping-interval ws-backoff-min ws-backoff-max` (peer) and `ws-listen ws-server-tls-cert ws-server-tls-key ws-server-bearer ws-trusted-proxies` (interface), following the file's existing keyword-completion pattern.

**US7 DoD:** man pages and completion list every new key/token.

---

## User Story 8 — Unit tests (Unity) `[x]`

**Why:** cover parsing/inference/validation, the UAPI round-trip, the kernel guard, and show/showconf.

**Acceptance criteria:**
- [x] New/updated `*_test.c` under `src/tests/` pass plain + ASan+LSan+UBSan + MSan.
- [x] The `ipc_uapi` test drives the real UAPI via `test_uapi_seam.h`.

### Task 8.1 — Test files `[x]`
- [x] **create** `src/tests/config_test.c` — table-driven parse/inference/validation cases.
- [x] **create** `src/tests/containers_test.c` — model + `free_wgdevice` (LSan-clean) cases.
- [x] **create** `src/tests/ipc_uapi_test.c` — set→get round-trip over the seam.
- [x] **create** `src/tests/ipc_guard_test.c` — kernel-guard EOPNOTSUPP cases.
- [x] **create** `src/tests/show_test.c` / `src/tests/showconf_test.c` — rendering + round-trip + no-secret-leak cases.

Compressed test matrix (Arrange-Act-Assert; case `name` in every assertion):

| Test | Verifies |
|---|---|
| `config_parse_endpoint_ws_dialing_resolves` | `Endpoint=wss://h:port` + `WSMode=websocket` → transport=WEBSOCKET, `ws_url` set, endpoint resolved to `ip:port` |
| `config_parse_wstunnel_requires_target` | `WSMode=wstunnel` without `WSTunnelTarget` rejected; with target accepted (transport=WSTUNNEL) |
| `config_parse_wsmode_without_endpoint_inbound` | `WSMode=websocket`, no `Endpoint` → inbound (transport=websocket, no ws_url/endpoint) |
| `config_parse_wstunnel_inbound_rejected` | `WSMode=wstunnel` with no `Endpoint` → rejected (wstunnel is a dialing mode; inbound peers are websocket-only) |
| `config_parse_wsmode_required_for_ws_endpoint` | `Endpoint=wss://…` without `WSMode` rejected |
| `config_parse_ws_keys_rejected_on_udp` | any `WS*` peer key with a `host:port` Endpoint rejected — **including false/zero values** (`WSMask=false`, `WSTLSInsecure=false`, `WSPingInterval=0`), via the `WGPEER_HAS_WS_SETTINGS` presence flag |
| `config_parse_endpoint_and_wsmode_conflict` | `Endpoint=host:port` + `WSMode` rejected |
| `config_parse_ws_bool_millis_invalid` | non-`true/false` `WSMask`/`WSTLSInsecure`, non-numeric `WSPingInterval` rejected |
| `config_parse_bearer_error_hides_value` | a rejected peer/interface carrying `WSBearer`/`WSServerBearer` produces an error string that does NOT contain the bearer value (secret-safe diagnostics) |
| `config_parse_wslisten_empty_clears` | empty `WSListen=` clears + keeps `WGDEVICE_HAS_WS_LISTEN` |
| `config_parse_ws_endpoint_ipv6_literal` | `Endpoint=wss://[::1]:443/wg` resolves (re-bracketed) to the `[::1]:443` sockaddr; `ws_url` keeps the full URL |
| `config_cmd_ws_tokens_parity` | CLI `endpoint wss://…`, `ws-mode`, `wstunnel-target`, `ws-bearer <file>`, … match config-file results (incl. rejections); CLI bearer read from a file |
| `config_cmd_incremental_omits_transport` | a CLI peer with only `persistent-keepalive` (no endpoint/ws token) leaves `WGPEER_HAS_TRANSPORT` clear; a config-file peer sets it |
| `config_cmd_incremental_ws_setting_allowed` | a CLI peer with only `ws-bearer <file>` (no `ws-mode`/endpoint) passes validation (no `WGPEER_HAS_TRANSPORT` → deferred to the daemon), emitting the ws key without `transport=` |
| `containers_free_ws_graph` | `free_wgdevice` frees all WS strings (ASan/LSan clean) |
| `ipc_uapi_roundtrip_websocket` | set emits `transport/endpoint/ws_url/ws_*`; get reads them back identically |
| `ipc_uapi_roundtrip_wstunnel` | set/get incl. `wstunnel_target`; `ws_bearer` echoed |
| `ipc_uapi_device_server_keys` | `ws_listen`/`ws_server_*`/`ws_trusted_proxies` set+get (each sets its `WGDEVICE_HAS_*` on get) |
| `ipc_uapi_omits_transport_without_flag` | a peer without `WGPEER_HAS_TRANSPORT` emits **no** `transport=` line (incremental-set safety) |
| `ipc_uapi_udp_setconf_emits_transport_no_ws` | a plain-UDP `setconf`/`addconf` emits `transport=udp` for each created peer (v1.3.0 requires it at creation) but **no** `ws_*` peer keys and **no** device `ws_*` keys (device WS flags are NOT default-set) |
| `ipc_uapi_explicit_device_ws_empty_clears` | an explicit empty value for **any** device WS key (`WSListen=`, `WSServerTLSCert=`, `WSServerBearer=`, `WSTrustedProxies=`) sets its `WGDEVICE_HAS_*` flag and emits `key=` (empty) → daemon clears; omitting the key emits nothing (no clear-by-omission) |
| `config_parse_device_ws_empty_accepted` | each device WS key parses an empty value (clears the field, sets the flag) rather than erroring "Line unrecognized"/"Value is empty" |
| `ipc_guard_rejects_ws_on_kernel` | `device_has_ws_settings` → EOPNOTSUPP before kernel dispatch |
| `showconf_roundtrip_ws` | `showconf` output re-parses to the same model; `WSBearer` present, machine `endpoints` stays `ip:port` |
| `showconf_roundtrip_inbound_ws` | an inbound peer (`transport=websocket`, no `ws_url`) round-trips as `WSMode=websocket` with no `Endpoint` — transport preserved, not downgraded to udp |
| `show_hides_bearer` | `pretty_print`/`dump` never contain the bearer; `transport`/`ws url` shown by pretty only |

### Task 8.2 — tests `Makefile` `[x]`
- [x] **modify** `src/tests/Makefile` — the auto-discover already picks up new `*_test.c`; the existing `ipc_uapi` explicit rule (pthread + writable `RUNSTATEDIR`) now has its consumer. (The `.gitignore` already lists every produced binary name — no change needed there.)

**US8 DoD:** the unit tests under `src/tests/` are complete and pass under all three tiers (plain, ASan+LSan+UBSan, MSan); the tiers are actually executed in US11.

---

## User Story 9 — Fuzz coverage for the new parsers `[x]`

**Why:** every parser of external input must be fuzzed (project rule). The new WS parsing lives
**inside** already-fuzzed entry points, so coverage is automatic — this US verifies that, with no
new harness or dictionary (the fuzz `Makefile` consumes no `.dict`, so adding one would be an
orphan).

**Acceptance criteria:**
- [x] The new WS parsers are reached by existing harnesses: `config_read_line` (config-file WS keys) and `config_read_cmd` (CLI WS tokens) via `src/fuzz/config.c`, `src/fuzz/set.c` (`set_main`), `src/fuzz/setconf.c`, `src/fuzz/cmd.c`; `userspace_get_device` (UAPI WS keys) via `src/fuzz/uapi.c`.
- [x] No new orphan artifacts (no unused `.dict`).

### Task 9.1 — confirm harness reach `[x]`
- [x] **verify** (no code change expected) that `src/fuzz/config.c` fuzzes both `config_read_line` and `config_read_cmd`, and `src/fuzz/uapi.c` fuzzes `userspace_get_device`, so the WS config/CLI/UAPI parsers are on the fuzzed path. If any parser is NOT reachable, extend the relevant harness input generation (NOT a new dict) to reach it, in this same change.

**US9 DoD:** all new WS parsers are demonstrably on an existing harness's fuzzed path; the fuzz build + smoke run is executed in US11.

---

## User Story 10 — Documentation `[x]`

**Acceptance criteria:**
- [x] `docs/PROJECT.md` + `docs/ARCHITECTURE.md` describe the parity model (per-peer transport, resolved `endpoint`, `ws_url`, per-peer/device keys, no env except `WG_METRICS_LISTEN`), and pin `wireguard-go ≥ 1.3.0`.
- [x] `.claude/rules/project.md` reflects the delivered surface (concise, references the canonical docs).
- [x] Any modified Mermaid chart validates per `development_pipeline.md` §9.

### Task 10.1 — docs `[x]`
- [x] **modify** `docs/PROJECT.md`, `docs/ARCHITECTURE.md`, `.claude/rules/project.md` — replace any prior env-based/URL-in-endpoint description with the parity model; state the `wireguard-go ≥ 1.3.0` dependency and the config-file → UAPI key mapping.

**US10 DoD:** canonical docs match the implementation; charts (if touched) validate.

---

## User Story 11 — Ground-up verification + end-to-end `[x]`

**Why:** the final gate — everything builds/lints/tests clean on the touched platforms, and the
**full-tunnel VPN works end-to-end** with `wireguard-go` v1.3.0.

**Acceptance criteria:**
- [x] `CFLAGS="-O2 -Werror" make -C src` clean on linux (gcc+clang) and darwin; cross `PLATFORM=freebsd`/`openbsd`/`windows` build not broken.
- [x] `make -C src check` (clang-tidy gate) zero findings.
- [x] `make -C src/tests` green plain + `SANITIZE=address` + `SANITIZE=memory` (Linux container for the Linux-only sanitizers).
- [x] `make -C src/fuzz` builds; touched-harness smoke clean.
- [x] Mermaid validation (only if charts touched).
- [x] **Full-tunnel e2e (Linux container):** with real `wireguard-go` v1.3.0, `wg-quick up` a `0.0.0.0/0` websocket/wstunnel config; assert handshake + the through-VPN egress IP differs from the baseline; every network op hard-timeout-bounded.
- [x] **Full-tunnel e2e (macOS Manual QA):** the `WGQUICK_INTEGRATION.md` gate — real Mac, `0.0.0.0/0` WS peer, traffic flows, encrypted socket egresses the physical interface. STOP and hand off to the user for this step; open the PR only after the user confirms it passes.

### Task 11.1 — Re-verify from the ground up `[x]`
- [x] Re-read this plan from the top; confirm every action landed and every checkbox is ticked; run all quality gates above; record any deviation in `## Deviations`; run the e2e (container) and prepare the macOS manual-QA hand-off.

**US11 DoD:** all gates green, container full-tunnel e2e proven, macOS manual-QA hand-off prepared; PR only after the user confirms macOS.

---

## Deviations

- **US2 (`process_line`)** — the `[Peer]` WS dispatch uses `ctx->last_peer` directly rather than the
  `struct wgpeer *peer = ctx->last_peer;` alias shown in the plan, matching the existing peer-branch
  style (functionally identical). `WGPEER_HAS_TRANSPORT` is set once when the `[Peer]` section is
  created (`config.c` `[Peer]` handler) rather than on every peer line — same effect, no redundant work.
- **US3 (`userspace_get_device`)** — the owned-string branches use a small `static bool uapi_dup(char
  **dst, const char *value)` helper (free-before-strdup, returns false on ENOMEM), implementing the
  plan's per-branch free-before-strdup contract without repeating it in every branch.
- **US8 (`ipc_uapi_test.c`)** — includes `../ipc.c` (not `../ipc-uapi.h`) because `ipc-uapi-unix.h`
  references `string_list_add`, which is defined in `ipc.c` before it includes `ipc-uapi.h`; including
  `ipc.c` is the only way to compile the userspace UAPI path in a standalone test TU.
- **US8 (`show_test.c`/`showconf_test.c`)** — render-tested directly (calling the static
  `pretty_print`/`dump_print`, and `showconf_main` with a stubbed `ipc_get_device`) with captured
  stdout, rather than via the socket seam; this needs no pthread and keeps the tests hermetic. The
  plan's US8 did not prescribe the mechanism.
