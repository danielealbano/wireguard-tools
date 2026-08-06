# wireguard-tools — Project Rules

This repo is **wireguard-tools**: the canonical **userspace tooling for configuring WireGuard
tunnels, written in C** (bash for `wg-quick`). It builds `wg(8)` (show/configure/key-manage
WireGuard interfaces over per-platform kernel backends or the cross-platform UAPI protocol)
and ships `wg-quick(8)`, man pages, completions, and systemd units. It runs on Linux, macOS,
FreeBSD, OpenBSD, Windows, and Android.

> **This is a fork of upstream `wireguard-tools`** — sibling of the `wireguard-go` fork that
> adds a WebSocket transport. Fork goals, in order: sanitizer hardening (ASan+LSan+UBSan+MSan),
> a vendored-Unity test suite (unit, integration, e2e), a clang-tidy zero-findings gate, GitHub
> Actions CI (Linux + macOS, parallel jobs), config-file/CLI support for the sibling fork's
> per-peer WebSocket/wstunnel settings (UDP-parity UAPI; targets `wireguard-go` >= 1.3.0; see the
> WebSocket surface in `docs/ARCHITECTURE.md`), and CI-built release artifacts for Linux and macOS
> (GitHub releases + the `danielealbano/homebrew-wireguard` tap). Non-trivial work proceeds via the development
> pipeline per `development_pipeline.md`. The canonical docs MUST be kept current as changes
> land.

## MANDATORY: Read These First

You MUST ALWAYS read these before ANY work, in this order:

1. **`docs/PROJECT.md`** — what the project is, tech stack, repository layout, configuration
   and runtime surfaces (config keys, env vars, IPC backends), platforms, build/commands,
   testing, and the fork roadmap.
2. **`docs/ARCHITECTURE.md`** — components, configuration model, IPC backend selection, the
   UAPI text protocol, wg-quick orchestration, crypto, portability boundaries, and security
   posture (with Mermaid charts).

You MUST ALSO follow `c.md`, `github.md`, `agent.md`, and `development_pipeline.md`. It is
ABSOLUTELY MANDATORY to pass ALL quality gates before any work is considered done.

This rule file MUST stay accurate but CONCISE — it references the canonical docs, it does NOT
duplicate them.

---

## Tech Stack (current)

Authoritative detail lives in `docs/PROJECT.md`.

| Concern | Choice | Notes |
|---|---|---|
| Language | **C** (`gnu99`, `-Wall -Wextra`) | Zero third-party dependencies: libc + system interfaces only. See `c.md`. |
| Scripting | **bash** | `wg-quick` per-platform scripts (`android.c` for Android). |
| Build | **GNU make** (`src/Makefile`) | Authoritative command surface; `PLATFORM` from `uname -s`. |
| Crypto | in-repo Curve25519 (fiat32 / hacl64) | Key derivation + clamping only. Constant-time encoders in `encoding.c`/`ctype.h`. |
| Netlink | vendored mini-libmnl (`src/netlink.h`) | No external libmnl. |
| Static analysis | **clang-tidy** (`src/.clang-tidy`) gate + scan-build (`make -C src check`) | clang-tidy (`clang-analyzer-*` + curated bugprone/perf/portability) is the CI gate; scan-build is informational/local. clang-format deliberately NOT used. |
| CI / Release | **GitHub Actions** (`.github/workflows/`) | `ci.yml` (build gcc+clang/macOS, clang-tidy, fuzz+MSan smoke, parallel); `release.yml` (`v*` tags → Linux amd64/arm64 + macOS universal tarballs + SHA256SUMS). |
| Fuzzing | libFuzzer + ASan (`src/fuzz/`, clang) | Six harnesses: config/CLI parsers, UAPI response parsing, interface-list handling, command dispatch. |
| Unit tests | **Unity**, vendored (planned) | Lands with the fork's test-suite pass; policy in `c.md` §3. |
| Windows | llvm-mingw + `src/wincompat/` | Windows ≥ 10. |
| Versioning | `git describe` → `WIREGUARD_TOOLS_VERSION` | Fork releases: upstream base + suffix (e.g. `1.0.20260223+ws1`). |

---

## Hard Project Invariants — ABSOLUTE RULES

- **THE UAPI PROTOCOL IS SACRED.** The `get=1`/`set=1` cross-platform configuration protocol
  is the compatibility contract with `wireguard-go` and every userspace implementation. You
  MUST NOT change existing keys or semantics; new keys MUST be additive and coordinated with
  the sibling `wireguard-go` fork.
- **KERNEL ABIS ARE UPSTREAM CONTRACTS.** The Linux netlink family, BSD ioctls, and
  WireGuardNT interfaces MUST remain exactly compatible with their kernels.
- **CONFIG FILE FORMAT IS A STABLE SURFACE.** Existing `wg(8)`/`wg-quick(8)` keys and
  semantics MUST NOT change; new keys MUST be additive, CamelCase, documented in the man
  pages and completions, and — for wg-quick-level keys — consumed and stripped by `wg-quick`
  per the established convention. Tunnel configuration lives ONLY in config files/CLI args;
  environment variables only tune tool behavior.
- **ZERO DEPENDENCIES.** libc and system interfaces only. No third-party libraries without
  EXPLICIT user approval; prefer minimal, license-compatible vendoring (as with mini-libmnl
  and Unity).
- **CROSS-PLATFORM BUILDS MUST NOT BREAK.** Changes MUST keep linux, darwin, freebsd,
  openbsd, and windows building; per-platform variants (ipc backends, wg-quick scripts) MUST
  stay consistent when shared behavior changes.
- **SECRETS HYGIENE.** Keys are printed ONLY where that is the documented purpose (genkey,
  showconf, `WG_HIDE_KEYS=never`); diagnostics MUST NOT echo valid key material (for the
  precise current behavior on malformed input, see `docs/ARCHITECTURE.md` §8). Key files use
  umask-077 patterns. Constant-time primitives MUST NOT be weakened.
- **MINIMAL UPSTREAM DIFF.** No reformatting, no drive-by refactors of upstream code; match
  the existing style so future upstream rebases stay tractable.
- **SINGLE-THREADED BY DESIGN.** `wg` has no threads, signals, or event loops; do not
  introduce any without explicit approval (see `c.md` §1).
- Keep it SIMPLE.

---

## Non-goals (MUST NOT build unless the user explicitly asks)

- No WireGuard protocol/tunnel implementation in the tools (that is the kernel's /
  `wireguard-go`'s job — the tools only speak configuration protocols).
- No new configuration file formats; no daemons, no metrics servers, no web UI.
- No Debian/Ubuntu packaging (explicitly deferred by decision — distribution is via GitHub
  release artifacts and the Homebrew tap).
- No testcontainers / external test services — there is no external infrastructure.

---

## Commit Scopes

All commits MUST use `<type>(<scope>): <description>` per `agent.md`, with one of the scopes
below. A commit spanning multiple scopes uses `core`.

| Scope | Applies to |
|---|---|
| `core` | Cross-cutting changes and anything without its own scope |
| `wg` | `wg` CLI sources (`wg.c`, `show*`, `set*`, `genkey`, `pubkey`, `terminal`) |
| `config` | `config.c`/`config.h` parsing and the `containers.h` model |
| `ipc` | `ipc*.{c,h}`, `netlink.h`, `src/uapi/` per-OS headers |
| `crypto` | `curve25519*`, `encoding.*`, `ctype.h` |
| `wg-quick` | `src/wg-quick/` scripts and `android.c` |
| `wincompat` | `src/wincompat/` |
| `fuzz` | `src/fuzz/` harnesses |
| `tests` | Unit-test suite and vendored Unity (once landed) |
| `man` | `src/man/` |
| `completion` | `src/completion/` |
| `systemd` | `src/systemd/` |
| `contrib` | `contrib/` |
| `ci` | GitHub Actions workflows |
| `docs` | `docs/` (PROJECT, ARCHITECTURE, plans) |
| `make` | Makefiles |

```
feat(config): parse websocket endpoint URLs
```

---

## Standard Commands

The **`src/Makefile`** is the authoritative command surface (details in `docs/PROJECT.md`):

- `make -C src` — build `wg`.
- `make -C src check` — clean rebuild under scan-build (Clang Static Analyzer).
- `make -C src install` — install binary, man pages, completions, wg-quick, systemd units.
- `make -C src clean`.
- `make -C src/fuzz` — build the libFuzzer harnesses (clang required).
- `make -C src check` — scan-build (Clang Static Analyzer, HTML; local deep-dive).
- Cross/variants: `PLATFORM=<os>`, `DEBUG=yes`, `V=1`.
- CI (`.github/workflows/ci.yml`) enforces: `CFLAGS="-O2 -Werror" make -C src CC=<gcc|clang>`,
  clang-tidy per file (config `src/.clang-tidy`), fuzz ASan+UBSan smoke, MSan parser smoke —
  parallel, Linux (`ubuntu:26.04` container) + macOS. `release.yml` builds tagged artifacts.

**Quality gates (current)**: warnings-clean build on the touched platforms + `make -C src
check` clean + fuzz harnesses still building. **Quality gates (target, per `c.md` §4 — wired
in as the roadmap lands)**: unit + integration + e2e tests, clang-tidy zero findings,
ASan+LSan+UBSan and MSan builds green, fuzz smoke — run as independent parallel CI jobs.
Mermaid validation per `development_pipeline.md` §9 whenever docs charts are touched.

---

## Testing — ABSOLUTE (project-specific)

- Framework: **Unity, vendored in-repo** — the ONLY test framework (see `c.md` §3). No
  external test dependencies of any kind. Suite location: `src/tests/` (Unity vendored under
  `src/tests/unity/`), following the `src/fuzz/` harness pattern. (Test suite is roadmap
  item 2; until it lands, the verification surface is warnings + scan-build + fuzzers.)
- Tests MUST NEVER hit real networks or require privileges in the default run; IPC is
  exercised via test-owned local sockets/fds.
- The e2e tier drives `wg`/`wg-quick` against real backends — both the kernel netlink path
  and the userspace UAPI path (sibling `wireguard-go`) — via a dedicated target (privileges
  allowed there, never in the default run), per `c.md` §3.
- **Every parser MUST have fuzz coverage** — extend `src/fuzz/` in the same change.
- **Sanitizers are mandatory** on the test suite: ASan+LSan+UBSan build and a separate MSan
  build. On macOS hosts, the Linux-only sanitizers run in Linux containers (Docker); CI runs
  the sanitizer jobs on Linux runners. UBSan extended integer checks are NOT enabled on the
  vendored Curve25519 code (intentional unsigned wraparound).
- **Testcontainers are NOT used** — no external service infrastructure exists. This is the
  documented exception to the generic testing rules.

---

## Key Conventions

- **Per-platform boundaries**: OS differences live in dedicated files (`ipc-<os>.h`,
  `ipc-uapi-{unix,windows}.h`, per-platform wg-quick scripts) selected by preprocessor/make —
  NEVER `#ifdef` forests in shared logic. Keep ALL variants in sync.
- **Single ownership + `goto` cleanup**: one owner per allocation, single destructor for the
  device graph (`free_wgdevice`), `goto out/cleanup` exit paths, errors as
  `bool`/`-errno`/`NULL` per surrounding convention (see `c.md`).
- **stdout is product, stderr is diagnostics**; ANSI output only via `terminal.c`.
- **Validate everything at the boundary**: config lines, argv, env vars, and every UAPI
  response byte, with bounded numeric parsing.
- Commit scopes above; branches `<type>/<short-desc>` per `agent.md`/`github.md`.

---

## Rule Map

| Concern | Rule file |
|---|---|
| Agnostic agent behavior, git, plans, reviews, subagents | `agent.md` |
| Plan-driven development pipeline (write → review → implement → PR) + Mermaid validation | `development_pipeline.md` |
| C language, memory/string safety, testing tiers, sanitizers, quality gates | `c.md` |
| GitHub (`gh` CLI, branches, PRs) (tooling) | `github.md` |
| Project context (this file) | `project.md` |
