# wireguard-tools — Project

This repo is **wireguard-tools**: the canonical **userspace tooling for configuring
[WireGuard](https://www.wireguard.com/) tunnels**, written in **C** (with bash for `wg-quick`). It
builds the `wg(8)` binary — show, configure, and key-manage WireGuard interfaces — and ships the
`wg-quick(8)` script family, man pages, bash completions, and systemd units. It supports Linux,
macOS, FreeBSD, OpenBSD, Windows, and Android.

> **This build is a fork of upstream `wireguard-tools`** (sibling of the
> [`wireguard-go`](https://github.com/danielealbano/wireguard-go) fork, which adds a WebSocket
> transport in server and client mode). The fork's goals are: (1) a **sanitizer-hardened,
> unit-tested codebase** with CI quality gates; (2) **support for the WebSocket settings**
> introduced by the sibling `wireguard-go` fork, surfaced entirely through the standard
> configuration files; (3) **CI-built release artifacts** for Linux and macOS distributed via
> GitHub releases and a Homebrew tap. See [Roadmap](#roadmap). Licensed GPL-2.0 (`COPYING`).

---

## What It Does

- **`wg(8)`** (`src/*.c`) — the configuration tool. Subcommands: `show`, `showconf`, `set`,
  `setconf`, `addconf`, `syncconf`, `genkey`, `genpsk`, `pubkey`. It parses INI-style
  configuration (files or command-line grammar) into an in-memory device/peer model and applies
  it through a per-platform IPC backend — the kernel implementation where one exists, or any
  userspace implementation (e.g. `wireguard-go`) over the cross-platform UAPI text protocol.
- **`wg-quick(8)`** (`src/wg-quick/*.bash`, one script per platform, plus `android.c`) — an
  opinionated wrapper that reads an extended config file, creates the interface (falling back to
  a userspace implementation when no kernel module is present), applies the `wg` configuration,
  and manages addresses, MTU, DNS, routes (including fwmark-based default-route policy routing
  with nftables/iptables integration on Linux), and up/down hooks.
- **Support surfaces** — man pages (`src/man`), bash completions (`src/completion`), systemd
  units (`src/systemd`), Windows compatibility layer (`src/wincompat`), libFuzzer harnesses
  (`src/fuzz`), and example integrations under `contrib/` (including a single-file embeddable
  C library for configuring WireGuard from your own program, a JSON status dumper, DNS
  re-resolution, and a launchd setup for macOS).

```mermaid
flowchart LR
    CONF["wg.conf / CLI args"] --> WG["wg binary"]
    WGQ["wg-quick script"] -- "strip + addconf" --> WG
    WG --> DISPATCH{"IPC dispatch"}
    DISPATCH -- "kernel iface" --> KERNEL["Kernel backend<br/>netlink / ioctl / WireGuardNT"]
    DISPATCH -- "userspace socket" --> UAPI["UAPI text protocol<br/>UNIX socket / named pipe"]
    UAPI --> WGGO["wireguard-go daemon"]
```

---

## Tech Stack

| Concern | Choice | Notes |
|---|---|---|
| Language | **C** (`-std=gnu99 -D_GNU_SOURCE`) | `-Wall -Wextra`; zero third-party dependencies — libc and system interfaces only. |
| Scripting | **bash** | `wg-quick` per-platform scripts; Android uses a C implementation instead. |
| Build | **GNU make** (`src/Makefile`) | Plain hand-written Makefile; `PLATFORM` auto-detected from `uname -s`. |
| Crypto | in-repo **Curve25519** | `curve25519-fiat32.h` (32-bit) / `curve25519-hacl64.h` (64-bit) backends; used only for public-key derivation and key clamping. |
| Encoding | in-repo base64/hex (`encoding.c`) | Constant-time conversions; constant-time `ctype.h` replacements. |
| Netlink | in-repo **mini-libmnl** (`netlink.h`, LGPL-2.1+) | Vendored, minimized libmnl; no external libmnl dependency. |
| Kernel UAPI headers | `src/uapi/<os>/` | Fallback copies per OS: `linux/wireguard.h`, `dev/wg/if_wg.h`, `net/if_wg.h`, and `wireguard.h` (WireGuardNT). |
| Windows | **llvm-mingw** (`x86_64-w64-mingw32-clang`) | `src/wincompat/` shims, delay-loaded DLLs, Windows ≥ 10. |
| Static analysis | **scan-build** (`make check`) | Clang Static Analyzer over the full build. clang-tidy gate: see [Roadmap](#roadmap). |
| Fuzzing | **libFuzzer + ASan** (`src/fuzz/`) | Six harnesses: `config`, `uapi`, `stringlist`, `cmd`, `set`, `setconf`; clang required. |
| Unit tests | **Unity** (vendored, `src/tests/unity/`) | `make -C src/tests` (plain + `SANITIZE=address`/`memory`); a socket seam drives the real UAPI. See `.claude/rules/c.md`. |
| Versioning | `git describe` → `WIREGUARD_TOOLS_VERSION` | Injected by the Makefile from `git describe`; `version.h` provides the fallback version string and is the sole source for the Windows resource version. |

---

## Repository Layout

| Path | Contents |
|---|---|
| `src/wg.c`, `src/subcommands.h` | Entry point and subcommand dispatch table. |
| `src/show.c`, `src/showconf.c` | Pretty/scriptable status output; runtime config export. |
| `src/set.c`, `src/setconf.c` | `set` CLI grammar; `setconf`/`addconf`/`syncconf` file application. |
| `src/genkey.c`, `src/pubkey.c` | Key generation (getentropy/getrandom/urandom) and public-key derivation. |
| `src/config.c` / `config.h` | INI config-file and CLI-grammar parser → `wgdevice`/`wgpeer` model. |
| `src/containers.h` | `wgdevice`/`wgpeer`/`wgallowedip` linked-list model + flags. |
| `src/ipc.c` / `ipc.h`, `src/ipc-*.h` | IPC dispatch and per-platform backends (see `docs/ARCHITECTURE.md`). |
| `src/curve25519*`, `src/encoding.*`, `src/ctype.h` | Crypto and constant-time helpers. |
| `src/terminal.c` / `terminal.h` | ANSI color output with tty detection and filtering. |
| `src/wg-quick/` | Per-platform `wg-quick` implementations (`linux`, `darwin`, `freebsd`, `openbsd` bash; `android.c`). |
| `src/man/`, `src/completion/`, `src/systemd/` | Man pages, bash completions, `wg-quick@.service` + target. |
| `src/wincompat/` | Windows libc shims, delay-load loader, resources, manifest. |
| `src/fuzz/` | libFuzzer harnesses (clang, `-fsanitize=fuzzer,address`). |
| `src/uapi/` | Per-OS kernel header fallbacks. |
| `contrib/` | Examples and integrations (embeddable-wg-library, json, reresolve-dns, launchd, highlighter, …). |
| `docs/` | This document, `ARCHITECTURE.md`, and plan documents under `docs/plans/`. |

---

## Configuration & Runtime Surfaces

### Command line

`wg` alone defaults to `wg show`. `wg set` accepts an inline grammar (`listen-port`, `fwmark`,
`private-key <file>`, `peer <key>` with `remove`, `preshared-key <file>`, `endpoint`,
`persistent-keepalive`, `allowed-ips`). `wg setconf|addconf|syncconf <iface> <file>` apply a
config file (replace / append / minimal-diff sync). `genkey`/`genpsk` write a fresh key to
stdout (warning if the destination is a world-accessible regular file); `pubkey` derives the
public key on a stdin→stdout pipe.

### Configuration file format

INI-style, parsed case-insensitively, whitespace-stripped, `#` comments:

- **`[Interface]`** — `PrivateKey`, `ListenPort`, `FwMark` (`off`/decimal/`0x` hex).
- **`[Peer]`** — `PublicKey`, `PresharedKey`, `AllowedIPs` (comma-separated CIDRs; a `+`/`-`
  prefix switches to incremental add/remove instead of replace), `Endpoint` (`host:port` or
  `[v6]:port`, DNS-resolved with retries), `PersistentKeepalive` (`0`/`off`/1–65535, `0`
  meaning off).

`wg-quick` accepts the same file plus its own `[Interface]` keys — `Address`, `DNS` (servers
and search domains), `MTU`, `Table` (`auto`/`off`/table), `PreUp`/`PostUp`/`PreDown`/`PostDown`
(with `%i` interface expansion), `SaveConfig` — which it consumes itself and strips before
handing the remainder to `wg addconf` (`wg-quick strip` exposes the stripped form).

### Environment variables

| Variable | Effect |
|---|---|
| `WG_COLOR_MODE` | `always` / `never`; default: color iff stdout is a tty. |
| `WG_HIDE_KEYS` | `never` shows private/preshared keys in `wg show`; default: `(hidden)`. |
| `WG_ENDPOINT_RESOLUTION_RETRIES` | Endpoint DNS retry count (default 15) or `infinity`; the systemd unit sets `infinity`. |
| `WG_QUICK_USERSPACE_IMPLEMENTATION` | Userspace daemon binary `wg-quick` launches when there is no kernel module (default `wireguard-go`). |
| `WG_TUN_NAME_FILE` | Set *by* `wg-quick` (macOS) so the userspace daemon reports its kernel-chosen `utun` name. |

Environment variables only tune tool behavior — tunnel configuration lives EXCLUSIVELY in the
configuration file / CLI arguments.

### IPC backends

`wg` talks to whichever implementation owns the interface (see `docs/ARCHITECTURE.md` §3):

- **Userspace (all platforms)** — the cross-platform
  [UAPI text protocol](https://www.wireguard.com/xplatform/#configuration-protocol)
  (`get=1`/`set=1`) over `RUNSTATEDIR/wireguard/<iface>.sock` (UNIX socket) or the
  `\\.\pipe\ProtectedPrefix\Administrators\WireGuard\<iface>` named pipe on Windows. A live
  userspace socket takes precedence over a kernel interface of the same name.
- **Kernel** — Linux: generic netlink (`wireguard` family, vendored mini-libmnl); FreeBSD:
  nvlist `SIOCGWG`/`SIOCSWG` ioctls; OpenBSD: `SIOCGWG`/`SIOCSWG` ioctls; Windows: the
  WireGuardNT driver via SetupAPI device I/O.

---

## Platforms

| Platform | Status | Notes |
|---|---|---|
| Linux | Supported | Kernel module via netlink; userspace fallback in `wg-quick` when the module is absent. |
| macOS | Supported | Userspace only (`wireguard-go` + `utun`); `wg-quick` darwin script manages DNS via `networksetup` and re-applies state through a `route -n monitor` daemon. |
| FreeBSD | Supported | Kernel `if_wg` via nvlist ioctls; `-lnv`. |
| OpenBSD | Supported | Kernel `if_wg` via ioctls. |
| Windows | Supported | mingw build; WireGuardNT kernel driver or userspace named pipe; Windows ≥ 10. |
| Android | Supported | `wg-quick/android.c` C implementation (used with the Android app ecosystem). |

---

## Build & Commands

The **`src/Makefile` is the authoritative command surface**. Current targets:

- `make -C src` / `make -C src all` — build the `wg` binary (version injected from `git describe`).
- `make -C src install` — install `wg`, man pages, and (auto-detected) bash completions,
  `wg-quick`, and systemd units; honors `PREFIX`, `DESTDIR`, `BINDIR`, `MANDIR`, `BASHCOMPDIR`,
  `SYSCONFDIR`, `RUNSTATEDIR`, `WITH_BASHCOMPLETION`, `WITH_WGQUICK`, `WITH_SYSTEMDUNITS`.
- `make -C src check` — clean rebuild under **scan-build** (Clang Static Analyzer, HTML report).
- `make -C src clean`.
- `make -C src/fuzz` — build the six libFuzzer harnesses (clang required).
- Cross-compile: `make -C src PLATFORM=windows` (llvm-mingw); `DEBUG=yes` adds `-g`; `V=1`
  verbose.

CI-enforced commands (see `.github/workflows/`):

- Warnings-clean build: `CFLAGS="-O2 -Werror" make -C src CC=<gcc|clang>` (env-origin `CFLAGS`
  is appended to the Makefile's flags, preserving `-isystem uapi/$(PLATFORM)`).
- **clang-tidy** analysis gate (config in `src/.clang-tidy`): run per file with
  `clang-tidy <f.c> -- -std=gnu99 -D_GNU_SOURCE -isystem uapi/linux -DRUNSTATEDIR='"/var/run"' -I.`.
  `clang-analyzer-*` is the anchor and reports zero; `WarningsAsErrors` makes any new finding fail.
- Fuzz smoke: `CFLAGS="-O1 -g -fsanitize=undefined" make -C src/fuzz`, then each harness for a
  bounded `-max_total_time`; MSan smoke builds the parser harnesses with `-fsanitize=fuzzer,memory`.

> Unit-test and sanitizer *make targets* do not exist yet (sanitizers run via CI/container
> commands, not a Makefile target). Quality-gate policy for all changes is defined in
> `.claude/rules/c.md`.

---

## Testing

Current state — this is what exists today:

- **Unit tests (Unity)**: `src/tests/` holds vendored Unity (`src/tests/unity/`) and one
  `*_test.c` per unit, built by an auto-discovering Makefile (`make -C src/tests`, plus
  `SANITIZE=address` for ASan+LSan+UBSan and `SANITIZE=memory` for MSan). Tests `#include` the
  `.c` under test to reach `static` internals (the `src/fuzz` pattern); `test_uapi_seam.h`
  binds a test-owned UNIX socket so the real `userspace_set_device`/`get_device` run
  unprivileged. CI runs `unit-tests` (gcc+clang, plain+ASan) and `unit-tests-msan` in parallel.
  The **end-to-end** tier is still to come (see [Roadmap](#roadmap)).
- The build stays warnings-clean (`-Wall -Wextra -Werror` in CI) under the **clang-tidy**
  analysis gate, with informational scan-build.
- **Fuzzing**: `src/fuzz/` builds six libFuzzer harnesses with `-fsanitize=fuzzer,address`,
  covering the config-file parser, the `set` CLI grammar, UAPI response parsing, the
  interface-list string handling, and the full command dispatcher. Harnesses `#include` the
  `.c` files under test to reach internals, except the `cmd` dispatcher harness, which
  compiles and links all of `src/*.c` with `main` renamed to `wg_main`.
- **No external services, no live networks** — everything is a local binary plus local
  sockets. Testcontainers do not apply.

Standing policy (see `.claude/rules/c.md`): the vendored **Unity** unit-test suite, a mandatory
**ASan+LSan+UBSan / MSan** sanitizer gate (Linux-only sanitizers run in Linux containers on
macOS hosts; CI runs the sanitizer jobs on Linux runners), fuzz coverage required for every
parser, and a **clang-tidy** (including `clang-analyzer-*`) zero-findings policy. clang-format
is deliberately NOT used — the upstream code style is preserved to keep the fork's diff minimal.

---

## Roadmap

**Delivered**

- **Sanitizer investigation** — the codebase was exercised under ASan+LSan+UBSan and MSan
  (CLI, config, key, and UAPI-parser workloads) plus the fuzzers: **zero runtime findings,
  zero fuzzer crashes**. There was no hardening backlog; sanitizer coverage is now enforced in
  CI (fuzz smoke under ASan+UBSan and an MSan parser smoke) rather than as a one-off pass.
- **Static-analysis gate** — `src/.clang-tidy` (`clang-analyzer-*` + curated `bugprone-*` /
  `performance-*` / `portability-*`) at zero findings, `WarningsAsErrors`. scan-build runs
  informationally in CI (its broader default checkers include noisy ones) and remains the
  local HTML deep-dive via `make -C src check`.
- **CI (GitHub Actions)** — `.github/workflows/ci.yml`: warnings-clean build (`gcc`+`clang`
  on Linux via an `ubuntu:26.04` container, plus native macOS), the clang-tidy gate, and fuzz
  + MSan smoke — as **independent parallel jobs**. Test-suite jobs are added when the suite lands.
- **Release automation** — `.github/workflows/release.yml`: `v*` tags build prebuilt tarballs
  for Linux (amd64 + arm64 cross) and macOS (universal), with `SHA256SUMS`, published to a
  GitHub release. Versioning: upstream base + fork suffix (e.g. `1.0.20260223+ws1`).
- **Unit-test suite (Unity)** — vendored Unity (MIT) under `src/tests/unity/`, an
  auto-discovering `src/tests/Makefile` with **ASan+LSan+UBSan** and a separate **MSan**
  variant, and a test-owned UNIX-socket UAPI seam (`test_uapi_seam.h`) for driving the real
  `userspace_set_device`/`get_device` unprivileged. Run in CI as parallel `unit-tests`
  (gcc+clang, plain+ASan) and `unit-tests-msan` jobs. The integration tier is covered by the
  socket seam; the full **end-to-end** tier (driving `wg`/`wg-quick` against a real backend) is
  still planned (see below).
- **WebSocket/wstunnel configuration surface** — the sibling `wireguard-go` fork's WebSocket
  transport is configured entirely through config files/CLI (see `docs/ARCHITECTURE.md` §4,
  "WebSocket / wstunnel configuration surface"). **Bucket B** (per-tunnel, over the UAPI socket): `wg`
  parses `WSListen` (`[Interface]`), a `ws(s)://` `Endpoint`, `WSMode`, `WSTunnelTarget`, and
  `WSPeerBearer` (`[Peer]`) from both config files and the `wg set` CLI, serializes them to
  `set=1`, and reads them back from `get=1` (so `showconf` round-trips). **Bucket A**
  (daemon-level): `wg-quick` captures the `[Interface]` keys `Transport`, `WSRole`, `WSMask`,
  `WSTLS*`, `WSBearer`, `WSPingInterval`, `WSTrustedProxies`, `MetricsListen`, strips them, and
  exports them as `WG_TRANSPORT`/`WG_WS_*`/`WG_METRICS_LISTEN` when launching the userspace
  daemon (forcing the userspace path on `Transport=ws`). Requires **`wireguard-go` ≥ 1.2.0**
  (the `wstunnel_target` UAPI key). WS settings are rejected on a kernel interface.

**Planned**

1. **End-to-end test tier** — drive `wg`/`wg-quick` against a real backend (both the kernel
   netlink path and the userspace UAPI path via the sibling `wireguard-go`), via a dedicated
   target; e2e obtains `wireguard-go` from the sibling fork's **latest GitHub release** (local
   e2e uses a locally built one). The unit + integration tiers are delivered (above).
2. **Homebrew tap** — a `danielealbano/homebrew-wireguard` tap with formulas for this fork
   and the `wireguard-go` fork, installing the prebuilt release binaries. Debian/Ubuntu
   packaging is deliberately out of scope for now.
