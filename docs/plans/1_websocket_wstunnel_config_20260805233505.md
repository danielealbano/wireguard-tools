<!-- SACRED DOCUMENT — Edit ONLY per agent.md §2 plan-file rules: plan-review fixes, checkmarks, recorded implementation deviations, and code-review re-alignment. -->
<!-- You MUST NEVER delete this file or alter files outside this plan's scope. -->
<!-- Plans in docs/plans/ are PERMANENT artifacts. There are ZERO exceptions. -->

# Plan 1 — WebSocket / wstunnel configuration surface for `wg` + `wg-quick`

Teach `wg(8)` and `wg-quick(8)` the WebSocket/wstunnel settings that the sibling `wireguard-go`
fork consumes, so tunnels are configured from config files/CLI instead of hand-written to the UAPI
socket. Read `docs/PROJECT.md`, `docs/ARCHITECTURE.md`, and `.claude/rules/{c,project,agent,development_pipeline}.md`
first — this plan does not repeat them.

## Design record (agreed with the user; the contract this plan implements)

**Two buckets, split by how `wireguard-go` consumes each setting:**

- **Bucket A — daemon-level, read from ENV at daemon startup.** New `[Interface]` config keys that
  `wg-quick` **captures, strips, and re-exports as env vars** when it launches the userspace daemon.
  `wg` never sees them.

  | Config key (`[Interface]`) | → env | role |
  |---|---|---|
  | `Transport = udp\|ws` | `WG_TRANSPORT` | both (explicit selector) |
  | `WSRole = client\|server` | `WG_WS_ROLE` | both |
  | `WSMask = true\|false` | `WG_WS_MASK` | client |
  | `WSTLSCert` / `WSTLSKey` | `WG_WS_TLS_CERT` / `WG_WS_TLS_KEY` | server |
  | `WSTLSCA` / `WSTLSServerName` / `WSTLSInsecure` | `WG_WS_TLS_CA` / `WG_WS_TLS_SERVERNAME` / `WG_WS_TLS_INSECURE` | client |
  | `WSBearer` | `WG_WS_BEARER` | server (inbound gate) |
  | `WSPingInterval` | `WG_WS_PING_INTERVAL` | client |
  | `WSTrustedProxies` | `WG_WS_TRUSTED_PROXIES` | server |
  | `MetricsListen` | `WG_METRICS_LISTEN` | both |

- **Bucket B — per-tunnel, carried over the UAPI socket.** New keys `wg` learns to parse (config-file
  **and** CLI, matching every existing parameter) and serialize to `set=1` / read back from `get=1`.

  | Config key | CLI token | section | UAPI key |
  |---|---|---|---|
  | `WSListen = ws(s)://host:port/path` | `ws-listen` | `[Interface]` | `ws_listen` (device) |
  | `Endpoint = ws(s)://host:port[/path]` | `endpoint` | `[Peer]` | `endpoint` (URL form) |
  | `WSMode = standard\|wstunnel` | `ws-mode` | `[Peer]` | `ws_mode` |
  | `WSTunnelTarget = host:port` | `wstunnel-target` | `[Peer]` | `wstunnel_target` |
  | `WSPeerBearer = <token>` | `ws-bearer` | `[Peer]` | `ws_bearer` |

**`wireguard-go` `get=1` contract (verified against `main` / release `v1.2.0`, `device/uapi.go`).**
The additive-emit change landed in PR #9 (`474d228`) as `ws_target`; PR #10 (`refactor/rename-ws-target-to-wstunnel-target`,
commit `3b01b0d`, merge `f17ccdc`) renamed it to `wstunnel_target` — which is what `main`/`v1.2.0` emit.
For a WS bind/endpoint only (UDP peers byte-identical), `get=1` emits, additively:
```
ws_listen=<url>          # device section, when a listen URL is set
endpoint=<ws(s)://…>     # peer (already round-trips upstream)
ws_mode=standard|wstunnel# peer — ALWAYS emitted for a WS endpoint
wstunnel_target=<host:port>    # peer — only when non-empty
ws_bearer=<token>        # peer — only when non-empty (parity with preshared_key)
```

**Naming / version dependency:** the peer key is `WSTunnelTarget` (config) / `wstunnel-target` (CLI) /
`wstunnel_target` (UAPI). The sibling `wireguard-go` renames `ws_target`→`wstunnel_target` in lockstep;
this `wg` therefore requires a `wireguard-go` that speaks `wstunnel_target` — **release ≥ 1.2.0** (1.1.0
and earlier use `ws_target`). The end-to-end wstunnel test is gated on that build being available.

**Locked decisions:**
- **Explicit transport**: `Transport` is mandatory; a `ws(s)://` URL cannot distinguish native vs
  wstunnel, so transport (device) and `WSMode` (peer) are always explicit.
- **Kernel guard**: `wg` rejects a WS-carrying config on the kernel backend with a clear message.
- **Force userspace**: `wg-quick` must NOT attempt the kernel path when `Transport=ws`
  (linux/freebsd); darwin is already userspace-only; openbsd has no userspace path → fail fast.
- **`WSListen` semantics** (verified `conn/ws_server_test.go:210`): a persistent device scalar like
  `listen_port`, but — unlike `listen_port` — **NOT** added to the full-`setconf` clear-on-absent
  preset (`config.c` `config_read_init`). Gated on a new `WGDEVICE_HAS_WS_LISTEN` flag: absent ⇒ not
  emitted ⇒ daemon preserves the listener; explicit empty `WSListen=` ⇒ emitted empty ⇒ deliberate
  disable. `wg` never synthesizes the empty form from absence.
- **Secrets**: `WSPeerBearer` is printed only by `showconf` (documented key-export path), never by
  `wg show` / `dump` (which would break the dump contract and expose a secret).
- **Presence** of the new string fields = non-NULL pointer (device `ws_listen` additionally carries
  `WGDEVICE_HAS_WS_LISTEN` to distinguish explicit-empty from absent).
- **Tests**: vendor Unity v2.7.0 now (bootstraps `src/tests/`); every C change carries unit tests;
  existing fuzz harnesses already cover the extended parsers (a WS dictionary is added).
- **Out of scope**: `openbsd.bash`/`android.c` have no userspace-daemon launch (openbsd = kernel-only;
  android = separate C orchestrator) — no env injection there.

**Key-detection helper (shared rule):** a value is a WS URL iff it has the **case-sensitive** prefix
`ws://` or `wss://` (mirrors `wireguard-go` `device/uapi.go`).

---

## [x] US1 — Test infrastructure: vendor Unity, build target, UAPI test seam, CI

**Why:** Every subsequent story must ship unit tests, but no test suite exists yet. Establish it
first. Depends on: none.

**Acceptance criteria:**
- [x] Unity v2.7.0 vendored under `src/tests/unity/` (`unity.c`, `unity.h`, `unity_internals.h`) with
      its MIT license intact.
- [x] The `src/tests/Makefile` provides `all`/`clean`, a `SANITIZE=address` (ASan+LSan+UBSan) and a
      `SANITIZE=memory` (MSan, clang) variant, and a `TESTS` list that auto-discovers existing
      `*_test.c` (at US1: only `sanity`).
- [x] A shared `test_uapi_seam.h` lets a test drive `userspace_set_device`/`userspace_get_device`
      against a test-owned UNIX socket (no privileges; build-local `RUNSTATEDIR`).
- [x] `ci.yml` gains parallel `unit-tests` (gcc+clang) and `unit-tests-msan` jobs.
- [x] A smoke `sanity_test.c` exists to prove the harness wiring.
      (All gate *runs* — plain/ASan/MSan builds+tests — execute in US11, per pipeline §6.)

### [x] Task 1.1 — Vendor Unity v2.7.0
- [x] **Action 1.1.1** — create `src/tests/unity/{unity.c,unity.h,unity_internals.h}` from the v2.7.0
  release `src/` tree, unmodified, license header retained. Add `src/tests/unity/README` recording the
  pinned tag `v2.7.0` and upstream URL.

### [x] Task 1.2 — Test build system
- [x] **Action 1.2.1** — create `src/tests/Makefile`. Mirrors `src/fuzz/Makefile` conventions
  (`PLATFORM`, `-isystem ../uapi/$(PLATFORM)`, `-std=gnu11 -D_GNU_SOURCE`, `-DRUNSTATEDIR='"/var/empty"'`).
  Each `*_test.c` links `unity.c` and the unit(s) under test. `SANITIZE` selects the sanitizer set:

```make
# SPDX-License-Identifier: GPL-2.0
PLATFORM ?= $(shell uname -s | tr '[:upper:]' '[:lower:]')
CC ?= cc
# Auto-discover: TESTS is whatever *_test.c files currently exist, so each user
# story's test is picked up as it lands and US1 is green with only sanity_test.c.
TESTS := $(patsubst %_test.c,%,$(wildcard *_test.c))
CFLAGS ?= -O1 -g
CFLAGS += -std=gnu11 -D_GNU_SOURCE -I.. -Iunity -Wall -Wextra
ifneq ($(wildcard ../uapi/$(PLATFORM)/.),)
CFLAGS += -isystem ../uapi/$(PLATFORM)
endif
ifeq ($(SANITIZE),address)
CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
endif
ifeq ($(SANITIZE),memory)
CC := clang
CFLAGS += -fsanitize=memory -fsanitize-memory-track-origins
endif
DEFRUN := -DRUNSTATEDIR='"/var/empty"'
UAPI_RUNDIR := $(CURDIR)/.rundir

all: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t =="; ./$$t; done

# Generic recipe (fuzz-style): each *_test.c #includes the unit under test + every
# transitive .c it references, so the recipe compiles the TU + unity.c only.
%: %_test.c unity/unity.c
	$(CC) $(CFLAGS) $(DEFRUN) -o $@ $< unity/unity.c
# ipc_uapi exercises the REAL userspace UAPI path over a test-owned UNIX socket
# (test_uapi_seam.h): needs a writable RUNSTATEDIR and pthreads. This explicit rule
# overrides the generic pattern rule for the ipc_uapi target.
ipc_uapi: ipc_uapi_test.c unity/unity.c
	$(CC) $(CFLAGS) -DRUNSTATEDIR='"$(UAPI_RUNDIR)"' -o $@ $< unity/unity.c -lpthread

clean:
	$(RM) $(TESTS); $(RM) -r .rundir
.PHONY: all clean
```
  - Context: `containers_test.c`/`sanity_test.c` are self-contained (headers only). The other white-box
    tests `#include` the unit(s) under test **and every transitive implementation `.c`** they reference
    (the `src/fuzz` pattern — units are `#include`d, NOT separately compiled, so no symbol is defined
    twice): `config_test.c` → `../config.c` + `../encoding.c`; `ipc_guard_test.c` → `../ipc.c` +
    `../curve25519.c` + `../encoding.c`; `showconf_test.c` → `../showconf.c` + `../encoding.c`;
    `show_test.c` → `../show.c` + `../terminal.c` + `../encoding.c`; `ipc_uapi_test.c` → `../encoding.c`
    + `../curve25519.c` and drives the real `userspace_*` functions through the socket seam.

### [x] Task 1.3 — Shared UAPI test seam
- [x] **Action 1.3.1** — create `src/tests/test_uapi_seam.h`: an in-process fake UAPI endpoint (c.md's
  endorsed "UNIX-socket server the test controls") — NO macro surgery on the production code. It binds
  the real socket at `RUNSTATEDIR/wireguard/<iface>.sock` (the tests Makefile points `RUNSTATEDIR` at a
  writable build-local dir for this test), so the real `userspace_interface_file()` connects to it. A
  helper thread accepts one connection, captures everything the SUT writes, and replies with a
  caller-supplied response — driving the real `userspace_set_device`/`userspace_get_device`. Shared
  infra, included in full:

```c
/* SPDX-License-Identifier: GPL-2.0 */
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
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, mkdir(RUNSTATEDIR, 0700) == 0 || errno == EEXIST ? 0 : -1, "mkdir runstatedir");
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
```
  - Context: the header is self-contained (includes `<errno.h>` for the `EEXIST`/`errno` idempotent
    `mkdir`), so include order cannot break it. `sun_path` is 108 bytes — the build-local `RUNSTATEDIR`
    keeps the full socket path within that bound.

### [x] Task 1.4 — Smoke test + CI wiring
- [x] **Action 1.4.1** — create `src/tests/sanity_test.c` (one passing + Unity wiring assertion).
- [x] **Action 1.4.2** — modify `.github/workflows/ci.yml`: add jobs `unit-tests` (matrix gcc+clang,
  `ubuntu:26.04` container, `make -C src/tests` and `make -C src/tests SANITIZE=address`) and
  `unit-tests-msan` (clang, `make -C src/tests SANITIZE=memory`). Same apt install pattern (with retry
  loop + `libc6-dev`) as the existing jobs.

### [x] Task 1.5 — DoD
- [x] Unity v2.7.0 vendored; `src/tests/Makefile` (with the ASan/UBSan and MSan variants) and
      `test_uapi_seam.h` created; `sanity_test.c` created; CI `unit-tests`/`unit-tests-msan` jobs added.
      All test *runs* happen in US11.

---

## [x] US2 — Device model: WS fields + ownership

**Why:** All parse/serialize code needs the storage first. Depends on: US1.

**Acceptance criteria:**
- [x] `struct wgpeer` carries `endpoint_url`, `ws_mode`, `wstunnel_target`, `ws_bearer` (`char *`).
- [x] `struct wgdevice` carries `ws_listen` (`char *`) + `WGDEVICE_HAS_WS_LISTEN` flag.
- [x] `free_wgdevice` frees all five; no leaks under LSan.
- [x] `is_ws_url` (shared, libc-free) added to `encoding.h`; the `-Werror` build stays clean in every TU
      including `encoding.h`.

### [x] Task 2.1 — Extend containers.h
- [x] **Action 2.1.1** — modify `src/containers.h`:

```c
enum {
	WGDEVICE_REPLACE_PEERS = 1U << 0,
	WGDEVICE_HAS_PRIVATE_KEY = 1U << 1,
	WGDEVICE_HAS_PUBLIC_KEY = 1U << 2,
	WGDEVICE_HAS_LISTEN_PORT = 1U << 3,
	WGDEVICE_HAS_FWMARK = 1U << 4,
	WGDEVICE_HAS_WS_LISTEN = 1U << 5
};
```
  add to `struct wgpeer` (after `endpoint`): `char *endpoint_url, *ws_mode, *wstunnel_target, *ws_bearer;`
  and to `struct wgdevice` (after `listen_port`): `char *ws_listen;`
  - Context: `endpoint_url` is mutually exclusive with `endpoint.addr` — the parser routes exactly one.

- [x] **Action 2.1.2** — modify `free_wgdevice`: before `free(peer)`, free `peer->endpoint_url`,
  `ws_mode`, `wstunnel_target`, `ws_bearer`; before `free(dev)`, free `dev->ws_listen`. (`free(NULL)` is safe.)

- [x] **Action 2.1.3** — add the shared WS-URL detector to `src/encoding.h` (already included by both
  `config.c` and `ipc-uapi.h`, so both use one definition), implemented with explicit character
  comparisons — **no `strncmp`**, so `encoding.h` needs no `<string.h>` and cannot break the `-Werror`
  build in a TU (e.g. `pubkey.c`) that includes `encoding.h` without `<string.h>`:

```c
/* Case-sensitive ws://  or  wss://  prefix (mirrors wireguard-go device/uapi.go). */
static inline bool is_ws_url(const char *v)
{
	return (v[0] == 'w' && v[1] == 's' && v[2] == ':' && v[3] == '/' && v[4] == '/') ||
	       (v[0] == 'w' && v[1] == 's' && v[2] == 's' && v[3] == ':' && v[4] == '/' && v[5] == '/');
}
```
  - Context: `&&` short-circuits at the first mismatch/NUL, so every read is in-bounds for any
    NUL-terminated string. `encoding.h` already declares `bool`-returning functions, so `<stdbool.h>`
    is in scope.

### [x] Task 2.2 — Tests + DoD
- [x] **Action 2.2.1** — create `src/tests/containers_test.c` (black-box via public struct + `free_wgdevice`).

| Test | Verifies |
|---|---|
| `test_containers_free_wgdevice_frees_ws_fields` | A device with all five WS strings set frees cleanly (LSan/ASan clean). |
| `test_containers_free_wgdevice_null_ws_fields` | NULL WS fields do not crash free. |

- [x] DoD: fields + flag present; `free_wgdevice` frees all five; `is_ws_url` added to `encoding.h`;
      `containers_test.c` created covering the free cases. (All test runs happen in US11.)

---

## [x] US3 — `wg` config + CLI parsing and validation

**Why:** Parse the Bucket B keys from config files and the CLI, matching existing conventions exactly.
Depends on: US2.

**Acceptance criteria:**
- [x] Config-file `[Interface] WSListen`, `[Peer] Endpoint=ws(s)://`, `WSMode`, `WSTunnelTarget`,
      `WSPeerBearer` parse into the model.
- [x] CLI `ws-listen` (device, gated on `!peer`), `endpoint ws(s)://`, `ws-mode`, `wstunnel-target`,
      `ws-bearer` parse identically.
- [x] Validation: non-`ws/wss` scheme rejected; invalid `WSMode` rejected; a WS `Endpoint` (either
      path) requires `WSMode`, and a wstunnel WS `Endpoint` requires `WSTunnelTarget`; a full config
      (`config_read_finish`) additionally rejects WS peer keys with no WS `Endpoint`, while an
      incremental `wg set` (`config_read_cmd`, `full=false`) accepts standalone WS-attribute updates.
      Scheme/format parse errors quote the offending value; the `WSPeerBearer` error (a secret) and the
      structural `validate_ws_peer` errors do NOT echo a value.
- [x] `WSListen` is NOT added to the `config_read_init` full-`setconf` preset.
- [x] Explicit empty `WSListen=` sets `WGDEVICE_HAS_WS_LISTEN` with an empty string.

### [x] Task 3.1 — Parse helpers + detection
- [x] **Action 3.1.1** — modify `src/config.c` (uses the shared `is_ws_url` from `encoding.h`, Action
  2.1.3) — add these leaf helpers. Each frees any prior `*out` before `strdup` (duplicate-key safety),
  checks `strdup`, quotes the offending value on error (never a secret), and `value` has no spaces
  (stripped). `parse_wstunnel_target` pins the `host:port` rule (last `:` with non-empty host and port —
  accepts `h:1234` and `[::1]:1234`, rejects `hostonly`/`:p`/`h:`); strict numeric/IPv6 checks are the
  daemon's job:

```c
static bool parse_ws_listen(char **out, uint32_t *flags, const char *value)
{
	if (value[0] && !is_ws_url(value)) {
		(void) fprintf(stderr, "WSListen is neither empty nor a ws(s):// URL: `%s'\n", value);
		return false;
	}
	free(*out);                       /* "" (empty) is a valid explicit-disable value */
	*out = strdup(value);
	if (!*out) { perror("strdup"); return false; }
	*flags |= WGDEVICE_HAS_WS_LISTEN;
	return true;
}

static bool parse_ws_endpoint(char **out, const char *value)
{
	if (!is_ws_url(value)) {
		(void) fprintf(stderr, "WebSocket endpoint is not a ws(s):// URL: `%s'\n", value);
		return false;
	}
	free(*out);
	*out = strdup(value);
	if (!*out) { perror("strdup"); return false; }
	return true;
}

static bool parse_ws_mode(char **out, const char *value)
{
	if (strcmp(value, "standard") && strcmp(value, "wstunnel")) {
		(void) fprintf(stderr, "WSMode is neither standard nor wstunnel: `%s'\n", value);
		return false;
	}
	free(*out);
	*out = strdup(value);
	if (!*out) { perror("strdup"); return false; }
	return true;
}

static bool parse_wstunnel_target(char **out, const char *value)
{
	const char *sep = strrchr(value, ':');
	if (!sep || sep == value || !sep[1]) {
		(void) fprintf(stderr, "WSTunnelTarget is not in host:port form: `%s'\n", value);
		return false;
	}
	free(*out);
	*out = strdup(value);
	if (!*out) { perror("strdup"); return false; }
	return true;
}

static bool parse_ws_bearer(char **out, const char *value)
{
	if (!value[0]) {                  /* never echo the token itself */
		(void) fprintf(stderr, "WSPeerBearer is empty\n");
		return false;
	}
	free(*out);
	*out = strdup(value);
	if (!*out) { perror("strdup"); return false; }
	return true;
}
```

- [x] **Action 3.1.2** — add the shared per-peer validator to `src/config.c`, defined HERE (in the
  helpers task) so both Task 3.3's CLI pass and Task 3.4's config-file pass can call it without a
  forward dependency:

The `full` parameter distinguishes a complete config-file peer (`config_read_finish`) from an
incremental `wg set` peer (`config_read_cmd`): only a full definition requires that WS attributes be
accompanied by a WS endpoint — incremental `wg set wg0 peer K ws-bearer <tok>` (a standalone WS-attr
update the daemon accepts) MUST NOT be rejected, matching how every other peer attribute is
independently settable. The endpoint-anchored checks (no UDP+WS conflict; a WS endpoint requires
`WSMode`; a wstunnel WS endpoint requires `WSTunnelTarget`) apply on BOTH paths, preserving the
explicit-`WSMode` decision whenever an endpoint is actually set.

```c
static bool validate_ws_peer(const struct wgpeer *peer, bool full)
{
	bool has_ws_key = peer->ws_mode || peer->wstunnel_target || peer->ws_bearer;
	bool has_udp = peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6;

	if (full && has_ws_key && !peer->endpoint_url) {     /* complete config only; not incremental wg set */
		(void) fprintf(stderr, "A peer has WebSocket settings but no ws(s):// Endpoint\n");
		return false;
	}
	if (peer->endpoint_url && has_udp) {
		(void) fprintf(stderr, "A peer has both a UDP and a WebSocket Endpoint\n");
		return false;
	}
	if (peer->endpoint_url && !peer->ws_mode) {          /* WSMode mandatory for a WS endpoint */
		(void) fprintf(stderr, "A WebSocket Endpoint peer is missing WSMode\n");
		return false;
	}
	if (peer->endpoint_url && peer->ws_mode && !strcmp(peer->ws_mode, "wstunnel") && !peer->wstunnel_target) {
		(void) fprintf(stderr, "WSMode=wstunnel requires WSTunnelTarget\n");
		return false;
	}
	return true;
}
```

### [x] Task 3.2 — Config-file dispatch (`process_line`)
- [x] **Action 3.2.1** — modify the device branch: `else if (key_match("WSListen")) ret =
  parse_ws_listen(&ctx->device->ws_listen, &ctx->device->flags, value);` before the `goto error`.
- [x] **Action 3.2.2** — modify the peer branch: change the `Endpoint` arm to route on `is_ws_url(value)`
  → `parse_ws_endpoint(&ctx->last_peer->endpoint_url, value)` else `parse_endpoint(...)`; add arms for
  `WSMode` → `parse_ws_mode(&ctx->last_peer->ws_mode, value)`, `WSTunnelTarget` → `parse_wstunnel_target(...)`,
  `WSPeerBearer` → `parse_ws_bearer(...)`.

### [x] Task 3.3 — CLI dispatch (`config_read_cmd`)
- [x] **Action 3.3.1** — modify `config_read_cmd` in `src/config.c`: route the existing `endpoint` arm
  on `is_ws_url`, and add the four new arms (device `ws-listen` gated on `!peer` like `listen-port`;
  peer `ws-mode`/`wstunnel-target`/`ws-bearer`), each a two-token consume matching the surrounding arms:

```c
} else if (!strcmp(argv[0], "endpoint") && argc >= 2 && peer) {
	if (is_ws_url(argv[1])) {
		if (!parse_ws_endpoint(&peer->endpoint_url, argv[1]))
			goto error;
	} else if (!parse_endpoint(&peer->endpoint.addr, argv[1]))
		goto error;
	argv += 2; argc -= 2;
} else if (!strcmp(argv[0], "ws-listen") && argc >= 2 && !peer) {
	if (!parse_ws_listen(&device->ws_listen, &device->flags, argv[1]))
		goto error;
	argv += 2; argc -= 2;
} else if (!strcmp(argv[0], "ws-mode") && argc >= 2 && peer) {
	if (!parse_ws_mode(&peer->ws_mode, argv[1]))
		goto error;
	argv += 2; argc -= 2;
} else if (!strcmp(argv[0], "wstunnel-target") && argc >= 2 && peer) {
	if (!parse_wstunnel_target(&peer->wstunnel_target, argv[1]))
		goto error;
	argv += 2; argc -= 2;
} else if (!strcmp(argv[0], "ws-bearer") && argc >= 2 && peer) {
	if (!parse_ws_bearer(&peer->ws_bearer, argv[1]))
		goto error;
	argv += 2; argc -= 2;
}
```
  and before `return device;` add the validation pass (declare `struct wgpeer *vp;`) with `full=false`
  (incremental `wg set` — standalone WS-attribute updates allowed):
  `for_each_wgpeer(device, vp) if (!validate_ws_peer(vp, false)) goto error;`

### [x] Task 3.4 — Cross-key validation (`config_read_finish`)
- [x] **Action 3.4.1** — modify `config_read_finish` in `src/config.c`: in its existing
  `for_each_wgpeer` loop, after the public-key check, call `validate_ws_peer(peer, true)` (the full
  completeness check; defined in Action 3.1.2) for each peer and take the existing `err` path (returns
  `NULL`) when it returns false.
  (The CLI path's invocation is wired in Action 3.3.1, since `config_read_cmd` never calls
  `config_read_finish` — together they give full config-file/CLI validation parity.)

### [x] Task 3.5 — Tests + DoD
- [x] **Action 3.5.1** — create `src/tests/config_test.c`. White-box TU: `#include "../config.c"` (for
  `static` reach) **and** `#include "../encoding.c"` (`config.c` calls `key_from_base64`/`key_to_hex`).
  Drive `config_read_line`/`config_read_finish` and `config_read_cmd`. Table-driven:

| Test | Verifies |
|---|---|
| `test_config_parse_wslisten_valid` | `WSListen = wss://h:443/p` sets field + `WGDEVICE_HAS_WS_LISTEN`. |
| `test_config_parse_wslisten_empty_disables` | `WSListen=` sets flag + empty string. |
| `test_config_parse_wslisten_bad_scheme` | `WSListen = http://x` rejected. |
| `test_config_parse_wslisten_absent_leaves_flag_unset` | A full (`append=false`) `[Interface]` with no `WSListen` → `!(flags & WGDEVICE_HAS_WS_LISTEN)` (preset-exclusion / persistence contract). |
| `test_config_parse_endpoint_ws_url` | `Endpoint = wss://h:443/p` → `endpoint_url`, not sockaddr. |
| `test_config_parse_endpoint_udp_unchanged` | `Endpoint = 1.2.3.4:51820` still resolves to sockaddr. |
| `test_config_parse_wsmode_enum` | `standard`/`wstunnel` accepted; other rejected. |
| `test_is_ws_url_variants` | `ws://`,`wss://` → true; `WS://`,`WSS://`,`ws:/`,`wss:`,`""`,`http://` → false (case-sensitive, prefix-boundary). |
| `test_config_parse_wstunnel_target_form` | `h:1234`,`[::1]:1234` accepted; `hostonly`,`:p`,`h:` rejected. |
| `test_config_parse_ws_bearer_empty_rejected` | Empty `WSPeerBearer=` / CLI `ws-bearer ""` rejected (error does not echo a value). |
| `test_config_finish_wstunnel_requires_target` | `WSMode=wstunnel` w/o `WSTunnelTarget` → finish fails. |
| `test_config_finish_wskeys_require_ws_endpoint` | `WSMode` on a UDP-endpoint peer → finish fails. |
| `test_config_finish_ws_endpoint_requires_mode` | `Endpoint = wss://…` with no `WSMode` → finish fails. |
| `test_config_finish_rejects_double_endpoint` | A peer with both a UDP `Endpoint` and a `wss://` `Endpoint` → rejected (config-file + CLI). |
| `test_config_cmd_ws_tokens_parity` | CLI `ws-listen`/`endpoint wss://`/`ws-mode`/`wstunnel-target`/`ws-bearer` produce the same model as config-file. |
| `test_config_cmd_wslisten_rejected_after_peer` | `ws-listen` after a `peer` token rejected (device-level gate). |
| `test_config_cmd_ws_endpoint_requires_mode` | CLI `endpoint wss://…` without `ws-mode` → `config_read_cmd` rejects (validation parity, no `config_read_finish`). |
| `test_config_cmd_incremental_ws_attr_ok` | CLI `peer K ws-bearer <tok>` (or `ws-mode …`) with NO endpoint → accepted (incremental update; `full=false`). |

- [x] DoD: config-file + CLI parse the WS keys identically; validation (both paths) rejects the bad
      cases; `config_test.c` created covering the table above. (All test runs happen in US11.)

---

## [x] US4 — UAPI serialization: `set=1` emit + `get=1` read-back

**Why:** Carry Bucket B over the socket and round-trip it. Depends on: US2 (US3 for realistic inputs).

**Acceptance criteria:**
- [x] `userspace_set_device` emits `ws_listen` (gated on `WGDEVICE_HAS_WS_LISTEN`), a URL `endpoint`,
      and per-peer `ws_mode`/`wstunnel_target`/`ws_bearer` (each when its field is set); UDP peers unchanged.
- [x] `userspace_get_device` parses `ws_listen`, a `ws(s)://` `endpoint` (as URL), `ws_mode`,
      `wstunnel_target`, `ws_bearer`; unknown keys still ignored; UDP responses unchanged.
- [x] Internal robustness (not a test-mapped criterion — no `malloc` fault-injection seam exists):
      `strdup` failures follow the existing `err` path and propagate `-ENOMEM`.

### [x] Task 4.1 — `set=1` emission
- [x] **Action 4.1.1** — modify `src/ipc-uapi.h` `userspace_set_device`: after the `fwmark` emit, add
  `if (dev->flags & WGDEVICE_HAS_WS_LISTEN) (void) fprintf(f, "ws_listen=%s\n", dev->ws_listen);`. In
  the per-peer block, wrap the endpoint emit: `if (peer->endpoint_url) (void) fprintf(f,
  "endpoint=%s\n", peer->endpoint_url); else { <existing sockaddr getnameinfo block> }`, then emit
  `ws_mode`/`wstunnel_target`/`ws_bearer` each guarded by non-NULL. Emit order: endpoint, ws_mode, wstunnel_target,
  ws_bearer.

### [x] Task 4.2 — `get=1` read-back
- [x] **Action 4.2.1** — modify `userspace_get_device`: in the device-scope branches add `else if
  (!peer && !strcmp(key, "ws_listen")) { free(dev->ws_listen); dev->ws_listen = strdup(value); if
  (!dev->ws_listen) { ret = -ENOMEM; goto err; } dev->flags |= WGDEVICE_HAS_WS_LISTEN; }`. In the
  `endpoint` branch, if `is_ws_url(value)` `free(peer->endpoint_url)` then store `peer->endpoint_url =
  strdup(value)` (ENOMEM-checked) and skip `getaddrinfo`; else the existing path. Add
  `ws_mode`/`wstunnel_target`/`ws_bearer` peer branches, each `free`-ing the prior value before `strdup`
  (ENOMEM-checked) — a `get=1` response is external input read in a `getline` loop with no dedup, so a
  duplicated key MUST NOT leak the earlier allocation (mirrors the `ws_listen` free-before-`strdup`
  above and the config-side `parse_ws_*`). Uses the shared `is_ws_url` from `encoding.h` (Action 2.1.3) — the same
  detector the parser uses, so the WS-prefix rule cannot diverge between set-parse and get-read-back.

### [x] Task 4.3 — Tests + DoD
- [x] **Action 4.3.1** — create `src/tests/ipc_uapi_test.c`. White-box TU mirroring the proven
  `src/fuzz/uapi.c` include order (do NOT include `ipc-uapi.h` standalone — it pulls
  `ipc-uapi-unix.h`, whose `userspace_get_wireguard_interfaces` references `struct string_list` /
  `string_list_add` that live in `ipc.c`, so a standalone include fails to compile):

```c
#include "../curve25519.c"
#undef __linux__          /* skip the kernel backend; test the userspace UAPI path only (as fuzz/uapi.c) */
#include "../ipc.c"       /* provides string_list + string_list_add and the static userspace_* funcs */
#include "../encoding.c"
#include "test_uapi_seam.h"
```
  The seam binds a real `AF_UNIX` socket at `RUNSTATEDIR/wireguard/<iface>.sock`, so the stock
  `userspace_interface_file()` `stat`/`connect` succeed (no libc hack needed). Each test:
  `seam_start(&s, iface, reply)` → call `userspace_set_device`/`userspace_get_device` (reachable as
  `static` in the included `ipc.c`; they connect to the seam socket) → `seam_stop(&s)` → assert on
  `s.captured` (set) or the returned `struct wgdevice` (get). The `ipc_uapi` Makefile recipe already
  compiles `ipc_uapi_test.c` + `unity.c` with `-lpthread` and the writable `RUNSTATEDIR`.

| Test | Verifies |
|---|---|
| `test_uapi_set_emits_ws_listen_when_flag` | `ws_listen=` line present iff `WGDEVICE_HAS_WS_LISTEN`. |
| `test_uapi_set_omits_ws_listen_when_absent` | No `ws_listen` line when flag unset (persistence). |
| `test_uapi_set_emits_url_endpoint_and_ws_keys` | URL `endpoint=`, `ws_mode=`, `wstunnel_target=`, `ws_bearer=` lines emitted for a WS peer. |
| `test_uapi_set_udp_peer_byte_identical` | A UDP peer's `set=1` block is unchanged vs baseline. |
| `test_uapi_get_reads_back_ws_fields` | A crafted `get=1` response populates all WS fields. |
| `test_uapi_get_ws_endpoint_is_url` | `endpoint=wss://…` in a response → `endpoint_url`, not sockaddr. |
| `test_uapi_get_unknown_key_ignored` | Unknown `ws_future=` ignored, parse continues. |

- [x] DoD: `set=1` emits the WS lines (gated) and UDP peers stay byte-identical; `get=1` reads all WS
      fields back (free-before-reassign); `ipc_uapi_test.c` created. (All test runs happen in US11.)

---

## [x] US5 — Kernel-backend guard

**Why:** WS keys are meaningful only on the userspace backend; block silent partial-apply on a kernel
interface. Depends on: US2.

**Acceptance criteria:**
- [x] A device carrying any WS setting, dispatched to `kernel_set_device`, is rejected with a clear
      stderr message and a non-zero return.
- [x] Non-WS configs are unaffected on every backend; the `#else` (userspace-only) build is unaffected.

### [x] Task 5.1 — Guard in `ipc_set_device`
- [x] **Action 5.1.1** — modify `src/ipc.c`: add `static bool device_has_ws_settings(const struct
  wgdevice *dev)` (true if `WGDEVICE_HAS_WS_LISTEN`, or any peer has `endpoint_url`/`ws_mode`/`wstunnel_target`/`ws_bearer`).
  The helper MUST be enclosed in a NEW file-scope `#ifdef IPC_SUPPORTS_KERNEL_INTERFACE` … `#endif`
  block (added after the platform-backend includes ~line 52, before `ipc_set_device`) — it is
  referenced only on the kernel-dispatch path, so guarding its definition avoids a `-Wunused-function`
  `-Werror` failure on userspace-only builds (macOS/darwin, which has no kernel backend). The call site
  stays inside `ipc_set_device`'s existing `IPC_SUPPORTS_KERNEL_INTERFACE` branch, before `return
  kernel_set_device(dev);`:

```c
if (device_has_ws_settings(dev)) {
	(void) fprintf(stderr, "WebSocket settings require a userspace WireGuard implementation; `%s' is a kernel interface\n", dev->name);
	errno = EOPNOTSUPP;
	return -errno;
}
```
  - Context: `<stdio.h>` (`fprintf`) and `<errno.h>` (`EOPNOTSUPP`) are already in scope in `ipc.c` (the
    latter directly, the former transitively via the `ipc-uapi.h` include that precedes the functions).
    The caller (`set.c`/`setconf.c`) additionally `perror`s; the specific line above is the actionable
    diagnostic. `EOPNOTSUPP` matches "backend cannot do this".

### [x] Task 5.2 — Tests + DoD
- [x] **Action 5.2.1** — create `src/tests/ipc_guard_test.c`. White-box TU: `#include "../curve25519.c"`,
  `#include "../ipc.c"` (keeps the platform kernel backend so `IPC_SUPPORTS_KERNEL_INTERFACE` and
  `device_has_ws_settings` exist; `ipc.c` transitively pulls the vendored netlink header — the same
  set the real `wg` build compiles), and `#include "../encoding.c"`. The whole test body is wrapped in
  `#ifdef IPC_SUPPORTS_KERNEL_INTERFACE` (a no-op assertion otherwise) since the helper only exists on
  kernel-capable platforms; the CI unit-test job runs on Linux, where it is defined. Besides the helper,
  the test drives the observable guard via `ipc_set_device` on a WS device whose interface name has no
  socket (so `userspace_has_wireguard_interface` returns false and the kernel path is selected), and
  asserts a non-zero return plus the stderr message (captured via `freopen` to a temp file — no
  privileges, no real kernel call, since the guard returns before `kernel_set_device`).

| Test | Verifies |
|---|---|
| `test_ipc_device_has_ws_settings_detects_each` | True for each WS field individually (device + peer). |
| `test_ipc_device_has_ws_settings_false_for_udp` | False for a fully-populated UDP device. |
| `test_ipc_set_device_rejects_ws_on_kernel` | `ipc_set_device` with a WS device + no-socket iface → non-zero return + stderr message; returns before `kernel_set_device`. |

- [x] DoD: helper detects all WS carriers; guard emits the message and returns non-zero;
      `ipc_guard_test.c` created (gated on `IPC_SUPPORTS_KERNEL_INTERFACE`). (All test runs happen in US11.)

---

## [x] US6 — Config export + status display

**Why:** Round-trip `showconf` and render URL endpoints in `wg show`. Depends on: US4.

**Acceptance criteria:**
- [x] `showconf` emits `WSListen`, URL `Endpoint`, `WSMode`, `WSTunnelTarget`, `WSPeerBearer` when present.
- [x] `wg show` (pretty, dump, `endpoints`) renders a URL endpoint in the endpoint slot; dump column
      count is unchanged.
- [x] `WSPeerBearer` never appears in `wg show`/`dump`.

### [x] Task 6.1 — showconf
- [x] **Action 6.1.1** — modify `src/showconf.c`: after the `PrivateKey` block, `if (device->flags &
  WGDEVICE_HAS_WS_LISTEN) printf("WSListen = %s\n", device->ws_listen);`. In the peer loop, before the
  sockaddr endpoint block, `if (peer->endpoint_url) printf("Endpoint = %s\n", peer->endpoint_url); else
  { <existing sockaddr block> }`; then `if (peer->ws_mode) printf("WSMode = %s\n", peer->ws_mode);`,
  `WSTunnelTarget`, `WSPeerBearer` (each non-NULL-gated).

### [x] Task 6.2 — show
- [x] **Action 6.2.1** — modify `src/show.c`: in each of `pretty_print`, `dump_print`, and
  `ugly_print`'s `endpoints` branch, prepend a `peer->endpoint_url` case to the existing `sa_family`
  gate so a URL renders in the endpoint slot. No `ws_mode`/`wstunnel_target`/bearer output here (keeps the
  dump column count and hides the secret):

```c
/* pretty_print */
if (peer->endpoint_url)
	terminal_printf("  " TERMINAL_BOLD "endpoint" TERMINAL_RESET ": %s\n", peer->endpoint_url);
else if (peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6)
	terminal_printf("  " TERMINAL_BOLD "endpoint" TERMINAL_RESET ": %s\n", endpoint(&peer->endpoint.addr));

/* dump_print (endpoint column) */
if (peer->endpoint_url)
	printf("%s\t", peer->endpoint_url);
else if (peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6)
	printf("%s\t", endpoint(&peer->endpoint.addr));
else
	printf("(none)\t");

/* ugly_print, "endpoints" */
if (peer->endpoint_url)
	printf("%s\n", peer->endpoint_url);
else if (peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6)
	printf("%s\n", endpoint(&peer->endpoint.addr));
else
	printf("(none)\n");
```

### [x] Task 6.3 — Tests + DoD
- [x] **Action 6.3.1** — create `src/tests/showconf_test.c` and `src/tests/show_test.c`. Both keep the
  production `*_main` unchanged and drive it with a crafted device via **link-time stubs** (no upstream
  refactor): the test defines `int ipc_get_device(struct wgdevice **d, const char *i)` (returns a
  test-owned device) — and `show_test.c` additionally defines `char *ipc_list_devices(void)` — plus
  `const char *PROG_NAME = "wg";`, then captures `stdout` via `freopen` to a temp file and asserts.
  White-box TUs (each `#include`s only what it needs so no symbol is defined twice):
  `showconf_test.c` → `#include "../showconf.c"` + `#include "../encoding.c"`;
  `show_test.c` → `#include "../show.c"` + `#include "../terminal.c"` + `#include "../encoding.c"`.
  Set `WG_COLOR_MODE=never` so `terminal_printf` output is deterministic plain text.

| Test | Verifies |
|---|---|
| `test_showconf_emits_ws_listen_and_peer_keys` | Device+peer WS keys rendered in INI form. |
| `test_showconf_url_endpoint` | URL endpoint rendered as `Endpoint = wss://…`. |
| `test_show_dump_url_endpoint_same_columns` | `dump` shows URL in the endpoint column; column count unchanged vs a UDP peer. |
| `test_show_pretty_url_endpoint` | Pretty output shows the URL endpoint line. |
| `test_show_endpoints_url` | `wg show <if> endpoints` prints the `ws(s)://` URL in the endpoint slot for a WS peer. |
| `test_show_bearer_absent` | A peer with `ws_bearer` set → the token appears in neither `dump` nor pretty output. |

- [x] DoD: `showconf` round-trips all WS keys; `wg show`/`dump` render URL endpoints with the dump
      column count intact; `WSPeerBearer` absent from `wg show`; `showconf_test.c`/`show_test.c` created
      (incl. the bearer-absence case). (All test runs happen in US11.)

---

## [x] US7 — `wg-quick` translation (Bucket A → env; force userspace)

**Why:** Capture/strip daemon-level keys, export them as env for the daemon, and force the userspace
path when `Transport=ws`. Depends on: none functionally (parallel to `wg`), sequenced here so `wg`
already understands Bucket B pass-through.

**Acceptance criteria:**
- [x] `linux.bash`, `darwin.bash`, `freebsd.bash` capture+strip all Bucket A keys and `export` the
      corresponding `WG_*` env vars (only for keys actually set) before launching the daemon.
- [x] `Transport=ws` forces the userspace path on linux/freebsd (kernel path skipped).
- [x] `openbsd.bash` fails fast if `Transport=ws`.
- [x] WS keys present with `Transport≠ws` → `die` with a clear message.
- [x] `SaveConfig=true` round-trips the Bucket A keys (re-emitted by `save_config` on the WS-capable
      scripts), so a down/reload does not lose `Transport=ws` or the daemon-level WS settings.
- [x] Bucket B keys pass through unchanged to `wg addconf` (no parser change needed).
- [x] Every touched script is syntactically well-formed (validated by `bash -n` in US11).

### [x] Task 7.1 — Shared translator logic (linux.bash, then mirrored)
The following blocks are **identical** across `linux.bash`, `darwin.bash`, `freebsd.bash`, and
`openbsd.bash` (Tasks 7.2–7.4 reuse them verbatim and change only `add_if`).
- [x] **Action 7.1.1** — declare the Bucket A capture vars at **script scope** (near the top, alongside
  `ADDRESSES`/`MTU`/…; NOT `local` in `parse_options` — bash `local` is dynamically scoped and gone by
  the time `add_if` runs from `cmd_up`), and add `case` arms in `parse_options`'s `[Interface]` block
  (`nocasematch` is already on) that capture + `continue`:

```bash
# script scope (with the other captured-setting globals)
WS_TRANSPORT="" WS_ROLE="" WS_MASK="" WS_TLS_CERT="" WS_TLS_KEY="" WS_TLS_CA=""
WS_TLS_SERVERNAME="" WS_TLS_INSECURE="" WS_BEARER="" WS_PING_INTERVAL="" WS_TRUSTED_PROXIES="" WS_METRICS_LISTEN=""

# inside parse_options()'s  case "$key" in … esac  (Interface section):
Transport) WS_TRANSPORT="$value"; continue ;;
WSRole) WS_ROLE="$value"; continue ;;
WSMask) WS_MASK="$value"; continue ;;
WSTLSCert) WS_TLS_CERT="$value"; continue ;;
WSTLSKey) WS_TLS_KEY="$value"; continue ;;
WSTLSCA) WS_TLS_CA="$value"; continue ;;
WSTLSServerName) WS_TLS_SERVERNAME="$value"; continue ;;
WSTLSInsecure) WS_TLS_INSECURE="$value"; continue ;;
WSBearer) WS_BEARER="$value"; continue ;;
WSPingInterval) WS_PING_INTERVAL="$value"; continue ;;
WSTrustedProxies) WS_TRUSTED_PROXIES="$value"; continue ;;
MetricsListen) WS_METRICS_LISTEN="$value"; continue ;;
```
- [x] **Action 7.1.2** — add `validate_ws_config` + `export_ws_env`, and call `validate_ws_config`
  UNCONDITIONALLY at the very end of `parse_options` (so the `die` fires regardless of which backend
  `add_if` later chooses). The WS-only die-set excludes `WS_TRANSPORT` (the selector) and
  `WS_METRICS_LISTEN` (transport-independent):

```bash
validate_ws_config() {
	[[ ${WS_TRANSPORT,,} == ws ]] && return 0
	local v
	for v in "$WS_ROLE" "$WS_MASK" "$WS_TLS_CERT" "$WS_TLS_KEY" "$WS_TLS_CA" "$WS_TLS_SERVERNAME" \
	         "$WS_TLS_INSECURE" "$WS_BEARER" "$WS_PING_INTERVAL" "$WS_TRUSTED_PROXIES"; do
		[[ -n $v ]] && die "WebSocket settings require \`Transport = ws'"
	done
	return 0   # the trailing [[ … ]] is false for a UDP config; without this, set -e aborts
}

export_ws_env() {
	[[ -n $WS_TRANSPORT ]] && export WG_TRANSPORT="$WS_TRANSPORT"
	[[ -n $WS_ROLE ]] && export WG_WS_ROLE="$WS_ROLE"
	[[ -n $WS_MASK ]] && export WG_WS_MASK="$WS_MASK"
	[[ -n $WS_TLS_CERT ]] && export WG_WS_TLS_CERT="$WS_TLS_CERT"
	[[ -n $WS_TLS_KEY ]] && export WG_WS_TLS_KEY="$WS_TLS_KEY"
	[[ -n $WS_TLS_CA ]] && export WG_WS_TLS_CA="$WS_TLS_CA"
	[[ -n $WS_TLS_SERVERNAME ]] && export WG_WS_TLS_SERVERNAME="$WS_TLS_SERVERNAME"
	[[ -n $WS_TLS_INSECURE ]] && export WG_WS_TLS_INSECURE="$WS_TLS_INSECURE"
	[[ -n $WS_BEARER ]] && export WG_WS_BEARER="$WS_BEARER"
	[[ -n $WS_PING_INTERVAL ]] && export WG_WS_PING_INTERVAL="$WS_PING_INTERVAL"
	[[ -n $WS_TRUSTED_PROXIES ]] && export WG_WS_TRUSTED_PROXIES="$WS_TRUSTED_PROXIES"
	[[ -n $WS_METRICS_LISTEN ]] && export WG_METRICS_LISTEN="$WS_METRICS_LISTEN"
	return 0   # the trailing [[ … ]] is false when WS_METRICS_LISTEN is empty; without this, set -e aborts
}
```
- [x] **Action 7.1.3** — rewrite `linux.bash` `add_if` to force userspace when `Transport=ws` (skip the
  kernel attempt), and `export_ws_env` before any userspace launch:

```bash
add_if() {
	local ret
	if [[ ${WS_TRANSPORT,,} == ws ]]; then
		export_ws_env
		cmd "${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" "$INTERFACE"
		return 0
	fi
	if ! cmd ip link add dev "$INTERFACE" type wireguard; then
		ret=$?
		[[ -e /sys/module/wireguard ]] || ! command -v "${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" >/dev/null && exit $ret
		echo "[!] Missing WireGuard kernel module. Falling back to slow userspace implementation." >&2
		export_ws_env
		cmd "${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" "$INTERFACE"
	fi
}
```

### [x] Task 7.2 — darwin.bash
- [x] **Action 7.2.1** — add the Task 7.1.1 globals/case-arms and 7.1.2 helpers (identical);
  darwin is userspace-only, so `add_if` gains only `export_ws_env` before the existing launch:

```bash
add_if() {
	export_ws_env
	export WG_TUN_NAME_FILE="/var/run/wireguard/$INTERFACE.name"
	mkdir -p "/var/run/wireguard/"
	cmd "${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" utun
	get_real_interface
}
```

### [x] Task 7.3 — freebsd.bash
- [x] **Action 7.3.1** — add the Task 7.1.1/7.1.2 blocks (identical); `add_if` forces userspace when
  `Transport=ws` and `export_ws_env` before any userspace launch:

```bash
add_if() {
	local ret rc
	if [[ ${WS_TRANSPORT,,} == ws ]]; then
		export_ws_env
		cmd "${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" "$INTERFACE"
		return 0
	fi
	if ret="$(cmd ifconfig wg create name "$INTERFACE" 2>&1 >/dev/null)"; then
		return 0
	fi
	rc=$?
	if [[ $ret == *"ifconfig: ioctl SIOCSIFNAME (set name): File exists"* ]]; then
		echo "$ret" >&3
		return $rc
	fi
	echo "[!] Missing WireGuard kernel support ($ret). Falling back to slow userspace implementation." >&3
	export_ws_env
	cmd "${WG_QUICK_USERSPACE_IMPLEMENTATION:-wireguard-go}" "$INTERFACE"
}
```

### [x] Task 7.4 — openbsd.bash
- [x] **Action 7.4.1** — add the Task 7.1.1 globals/case-arms and the 7.1.2 `validate_ws_config`
  (openbsd has no userspace path, so `export_ws_env` is unused there and need not be added); `add_if`
  `die`s if `Transport=ws`:

```bash
add_if() {
	[[ ${WS_TRANSPORT,,} == ws ]] && die "WebSocket transport requires a userspace implementation, unsupported on OpenBSD"
	# … existing OpenBSD kernel add_if body unchanged …
}
```

### [x] Task 7.5 — Persist Bucket A across `SaveConfig`
- [x] **Action 7.5.1** — `save_config()` reconstructs `[Interface]` from the captured wg-quick vars and
  merges with `wg showconf`, then atomically rewrites the config. Because Bucket A keys are stripped
  from `WG_CONFIG` and are NOT in `showconf` output, a `SaveConfig=true` down/reload would drop them —
  losing `Transport=ws` (so the next `up` builds a kernel interface and the US5 guard rejects the
  surviving Bucket B keys) and the daemon-level WS/TLS settings. Add the captured Bucket A keys to
  `new_config` in `save_config()` of `linux.bash`, `darwin.bash`, and `freebsd.bash` (the WS-capable
  scripts), immediately after the existing `[[ $SAVE_CONFIG … ]]` line:

```bash
[[ -n $WS_TRANSPORT ]] && new_config+="Transport = $WS_TRANSPORT"$'\n'
[[ -n $WS_ROLE ]] && new_config+="WSRole = $WS_ROLE"$'\n'
[[ -n $WS_MASK ]] && new_config+="WSMask = $WS_MASK"$'\n'
[[ -n $WS_TLS_CERT ]] && new_config+="WSTLSCert = $WS_TLS_CERT"$'\n'
[[ -n $WS_TLS_KEY ]] && new_config+="WSTLSKey = $WS_TLS_KEY"$'\n'
[[ -n $WS_TLS_CA ]] && new_config+="WSTLSCA = $WS_TLS_CA"$'\n'
[[ -n $WS_TLS_SERVERNAME ]] && new_config+="WSTLSServerName = $WS_TLS_SERVERNAME"$'\n'
[[ -n $WS_TLS_INSECURE ]] && new_config+="WSTLSInsecure = $WS_TLS_INSECURE"$'\n'
[[ -n $WS_BEARER ]] && new_config+="WSBearer = $WS_BEARER"$'\n'
[[ -n $WS_PING_INTERVAL ]] && new_config+="WSPingInterval = $WS_PING_INTERVAL"$'\n'
[[ -n $WS_TRUSTED_PROXIES ]] && new_config+="WSTrustedProxies = $WS_TRUSTED_PROXIES"$'\n'
[[ -n $WS_METRICS_LISTEN ]] && new_config+="MetricsListen = $WS_METRICS_LISTEN"$'\n'
```
  - Context: `WSListen` (Bucket B, `[Interface]`) is NOT re-emitted here — it round-trips via `wg
    showconf` already. `openbsd.bash` is kernel-only: `Transport=ws` `die`s before `save_config`, and a
    WS-only key with `Transport≠ws` `die`s in `parse_options`, so no WS key ever reaches its
    `save_config` — it is intentionally left unchanged.

### [x] Task 7.6 — DoD
- [x] DoD: the four scripts capture/strip Bucket A as globals, `validate_ws_config` + `export_ws_env`
      added, force-userspace/fail-fast wired per platform, and `SaveConfig` re-emits Bucket A on the
      WS-capable scripts. (`bash -n` runs in US11.)
- [x] **Manual QA Steps** (documented, not a substitute for automated tests; automated e2e is the
  deferred e2e-tier roadmap item): (1) linux with kernel module present + `Transport=ws` launches the
  userspace daemon, not a kernel netdev; (2) `WSBearer`/`WSTLS*` reach the daemon env; (3) a config with
  WS keys but no `Transport=ws` aborts.

---

## [x] US8 — Man pages + bash-completion

**Why:** Document the new keys/tokens and complete `wg set`. Depends on: US3, US7.

**Acceptance criteria:**
- [x] `wg.8`: `[Interface] WSListen`; `[Peer] Endpoint` URL note + `WSMode`/`WSTunnelTarget`/`WSPeerBearer`;
      `set` synopsis gains `ws-listen`/`ws-mode`/`wstunnel-target`/`ws-bearer`.
- [x] `wg-quick.8`: Bucket A keys added to the "handled by this tool" list.
- [x] `wg.bash-completion` offers the new `set` tokens with correct one-shot/`has_*` tracking.

### [x] Task 8.1 — man pages
- [x] **Action 8.1.1** — modify `src/man/wg.8`: add the Interface/Peer entries and extend the `set`
  synopsis line; note `Endpoint` accepts `ws(s)://` URLs for userspace WS interfaces.
- [x] **Action 8.1.2** — modify `src/man/wg-quick.8`: add `Transport`, `WSRole`, `WSMask`, `WSTLSCert`,
  `WSTLSKey`, `WSTLSCA`, `WSTLSServerName`, `WSTLSInsecure`, `WSBearer`, `WSPingInterval`,
  `WSTrustedProxies`, `MetricsListen` to the tool-handled `[Interface]` list, each one line.

### [x] Task 8.2 — completion + DoD
- [x] **Action 8.2.1** — modify `src/completion/wg.bash-completion`, matching the existing `has_*`
  idiom:

```bash
# declaration (~L38): add ALL four new trackers to the existing `local has_listen_port=0 …` line
#   (they MUST be `local` like every existing has_* tracker, or they leak into the user's shell):
#   … has_ws_listen=0 has_ws_mode=0 has_wstunnel_target=0 has_ws_bearer=0 …
#   in the device token loop (~L39-44):
[[ ${COMP_WORDS[i]} == ws-listen ]] && has_ws_listen=1
#   in the device word list (~L45-51), before `words+=( peer )`:
[[ $has_ws_listen -eq 1 ]] || words+=( ws-listen )

# peer block: in the reset-on-`peer` group (~L68-72):
has_ws_mode=0 has_wstunnel_target=0 has_ws_bearer=0
#   in the peer token loop (~L76-80):
[[ ${COMP_WORDS[i]} == ws-mode ]] && has_ws_mode=1
[[ ${COMP_WORDS[i]} == wstunnel-target ]] && has_wstunnel_target=1
[[ ${COMP_WORDS[i]} == ws-bearer ]] && has_ws_bearer=1
#   in the peer word list (~L88-94):
[[ $has_ws_mode -eq 1 ]] || words+=( ws-mode )
[[ $has_wstunnel_target -eq 1 ]] || words+=( wstunnel-target )
[[ $has_ws_bearer -eq 1 ]] || words+=( ws-bearer )
```
- [x] DoD: man entries + `set` synopsis updated; completion arms/tracking added for the new tokens.
      (`bash -n` on the completion and `man -l` rendering run in US11.)

---

## [x] US9 — Fuzz dictionary for WS keys

**Why:** The existing harnesses already reach the extended parsers; a dictionary accelerates WS-path
discovery (c.md: parsers get fuzz coverage in the same change). Depends on: US3, US4.

**Acceptance criteria:**
- [x] A `src/fuzz/ws.dict` lists WS config/CLI/UAPI tokens; the config/uapi/set/setconf harnesses still
      build and smoke-run clean.

### [x] Task 9.1 — dictionary + DoD
- [x] **Action 9.1.1** — create `src/fuzz/ws.dict` with entries (`"WSListen="`, `"WSMode="`,
  `"wss://"`, `"ws_mode="`, `"standard"`, `"wstunnel"`, `"wstunnel_target="`, `"ws_bearer="`,
  `"ws_listen="`, `"ws-mode"`, …). No harness `.c` change required (the extended functions are already
  under `config`/`uapi`/`set`/`setconf`).
- [x] DoD: `ws.dict` created with the WS tokens. (The `make -C src/fuzz` build + the `config`/`uapi`
      smoke with `-dict=ws.dict` run in US11.)

---

## [x] US10 — Documentation

**Why:** Keep the canonical docs current (agent.md mandate). Depends on: US1–US9.

**Acceptance criteria:**
- [x] `docs/PROJECT.md` + `docs/ARCHITECTURE.md` document the WS config surface (both buckets, the
      wg-quick translation, the kernel guard, the `get=1` round-trip) and move it Roadmap→Delivered.
- [x] `.claude/rules/project.md` config-surface + testing rows reflect Unity landed and the WS keys.

### [x] Task 10.1 — docs + DoD
- [x] **Action 10.1.1** — modify `docs/PROJECT.md`: WS config surface section; test suite (Unity)
  marked delivered.
- [x] **Action 10.1.2** — modify `docs/ARCHITECTURE.md`: describe the Bucket A/B split and the
  `wg-quick`→env / `wg`→UAPI flow in prose ONLY. This plan adds NO new Mermaid chart and modifies NO
  existing chart, so no `mmdc` validation is required.
- [x] **Action 10.1.3** — modify `.claude/rules/project.md`: Unity test suite now present; WS keys noted
  in the config-surface conventions.
- [x] DoD: docs accurate; no stale "planned" markers for delivered items.

---

## [x] US11 — Ground-up verification (final)

**Why:** Double-check the entire implementation from scratch. Depends on: US1–US10.

**Acceptance criteria + tasks:**
- [x] **Task 11.1** — Re-read this plan from disk; confirm every action's checkbox is `[x]` and each
      matches the code actually written; record any deviation in `## Deviations`.
- [x] **Task 11.2** — Quality gates (run once each, `tee` to `/tmp`): warnings-clean build on the
      touched platforms (`CFLAGS="-O2 -Werror" make -C src` for gcc and clang on linux; `make -C src`
      on darwin); `make -C src check` (scan-build) clean; `make -C src/tests` green plain +
      `SANITIZE=address` + `SANITIZE=memory`; `make -C src/fuzz` builds + 30s `config`/`uapi` smoke
      (`-dict=ws.dict`); clang-tidy gate clean (the CI file list) with zero findings.
- [x] **Task 11.3** — `bash -n` on all four `wg-quick` scripts; `man -l` renders `wg.8`/`wg-quick.8`
      without warnings.
- [x] **Task 11.4** — Confirm UDP-only configs are byte-identical through `set=1`/`get=1`/`showconf`/
      `dump` vs the pre-change baseline (no regression); confirm the `get=1` read-back format matches
      the merged `wireguard-go` `main` / `v1.2.0` `ws_listen`/`ws_mode`/`wstunnel_target`/`ws_bearer`
      emission in `device/uapi.go` (additive emit PR #9 `474d228`; `wstunnel_target` rename PR #10
      `3b01b0d`, merge `f17ccdc`).
- [x] **Task 11.5** — Re-confirm every acceptance criterion of US1–US10 is satisfied.

## Deviations

- **Action 3.2.1 (WSListen config-file dispatch)** — `get_value()` returns NULL for a zero-length
  value (`keylen >= linelen`), so `key_match("WSListen")` never matches the explicit-disable form
  `WSListen=` (empty). The device dispatch instead uses `!strncasecmp(line, "WSListen=", …)` and passes
  `line + strlen("WSListen=")`, which accepts both empty and non-empty values. The CLI path is
  unaffected (it does not go through `get_value`). Caught by `test_config_parse_wslisten_empty_disables`.
- **Action 3.3.1 (CLI validation pass)** — reuses `config_read_cmd`'s existing `peer` walker variable
  for the `for_each_wgpeer` validation loop rather than declaring a new `vp`; equivalent and smaller.
- **Action 3.1.1 (parse_ws_mode)** — uses `strcmp(...) != 0` (explicit) to satisfy the clang-tidy
  `bugprone-suspicious-string-compare` gate; the plan's illustrative `strcmp` was implicit.
- **Action 1.2.1 (tests Makefile)** — added `-DUNITY_EXCLUDE_MATH_H`: Unity's `<math.h>` brings glibc's
  C23 narrowing `fmul()`/`fadd()`/… which collide with the `static fmul()`/… in the vendored
  `curve25519-hacl64.h` when a test includes both (`ipc_guard_test`, `ipc_uapi_test`). The `src/fuzz`
  harnesses avoid this only because they never include Unity.
- **Action 6.3.1 (show_test dump case)** — the `dump` line legitimately prints `(none)` for a peer's
  absent preshared-key and allowed-ips columns, so the URL-in-endpoint-column assertion checks
  `"<url>\t"` (URL followed by the column tab) instead of asserting no `(none)` anywhere.
- **US7 (`set -e` in the new bash functions)** — found by the live e2e: `validate_ws_config` and
  `export_ws_env` each ended with a `[[ … ]] && …` whose left side is false for a UDP config / empty
  var, so the function returned non-zero, and under `wg-quick`'s `set -e` that aborts (`validate_ws_config`
  would have broken EVERY plain UDP tunnel). Added an explicit `return 0` at the end of both functions
  in `linux.bash`/`darwin.bash`/`freebsd.bash` and of `validate_ws_config` in `openbsd.bash`. (Inline
  `[[ … ]] && …` statements elsewhere are exempt from `set -e` via the `&&`-short-circuit rule; only a
  function's trailing statement determines its return status.)
- **US8 (set.c usage string)** — beyond the planned man + completion updates, the `set_main` usage
  string in `src/set.c` was extended with the four new CLI tokens (`ws-listen`, `ws-mode`,
  `wstunnel-target`, `ws-bearer`), matching the `wg.8` `set` grammar, to fully match the existing
  parameters' behavior (every other token already appears there).
