# C Rules — ABSOLUTE RULES

These rules apply to ANY C project where this file is present. They are **VERY STRICT and ABSOLUTELY NON-NEGOTIABLE**!
Project-specific details (structure, dependencies, Makefile targets) live in the project-specific rule file.

## 1) Architecture & Idioms — ABSOLUTE RULES

### C idioms first
- You MUST write standard, portable C (the project-specific rule file pins the exact dialect, e.g. `gnu99`) and you MUST match the idioms already established in the codebase.
- You MUST ALWAYS prefer simplicity over cleverness. Clear is better than clever.
- You MUST keep translation units, functions, and headers small and cohesive; you MUST NEVER create "util" or "common" mega-files.
- You MUST keep the responsibilities in the code narrow — one file, one concern.
- You MUST expose only what consumers need: functions private to a translation unit MUST be `static`; public headers MUST declare the minimal API surface.
- You MUST use `const` correctness on pointers and parameters that are not mutated.
- You MUST NEVER write functions that silently accept NULL for required parameters — it usually means too many things are being done in one place and a split is required. Preconditions MUST be explicit.
- You MUST ALWAYS write code that is testability friendly: parsing, formatting, and pure logic MUST be separable from I/O and syscalls so they can be exercised by unit tests and fuzzers.

### Boundaries and portability
- OS- and platform-specific behavior MUST live behind explicit boundaries: per-platform files or headers selected by the build system (e.g. `foo-linux.h`, `foo-windows.h`, a `PLATFORM` make variable, or preprocessor guards). You MUST NOT scatter `#ifdef` platform forests through shared logic.
- When you change shared behavior that has per-platform implementations, you MUST keep EVERY platform variant consistent — ALL of them, in the same change.
- Wire formats, IPC protocols, and file formats are CONTRACTS. You MUST NOT change them unless the user EXPLICITLY asks.

### Ownership and lifetimes
- Every allocation MUST have a single, obvious owner and a single, obvious free path. Document ownership transfer in the function contract when it is not obvious.
- You MUST use the established cleanup idiom of the codebase (e.g. `goto out`/`goto cleanup` with a single exit path) for functions that acquire multiple resources.
- You MUST free every resource on EVERY path — success and failure alike: memory, file descriptors, `FILE *`, sockets, directory handles.
- Structures that own linked data MUST provide a single destructor that frees the entire graph (e.g. a `free_x()` that walks the lists), and ALL code MUST use it.

### Global state and process model
- You MUST NOT introduce mutable global state beyond what the codebase already establishes; static function-local buffers and caches are acceptable ONLY in single-threaded code and MUST be flagged as such.
- If the project is single-threaded (see the project-specific rule file), you MUST NOT introduce threads, signal-async logic, or atomics without EXPLICIT user approval.
- If concurrency exists or is introduced, you MUST protect shared mutable state, give every thread a clear shutdown path, and treat data races as bugs — NEVER as warnings.
- Operations that may be retried or replayed (IPC requests, file writes) MUST be safe to repeat: write-to-temp-then-rename for files, idempotent request semantics for IPC.

## 2) Coding Standards — ABSOLUTE RULES

### Validation
- You MUST ALWAYS validate inputs at the boundary: command-line arguments, configuration lines, environment variables, and EVERY byte read from an IPC peer or file.
- Parsers MUST reject trailing garbage, out-of-range values, and truncated input explicitly — never "best effort".
- You MUST print error messages that include the offending value (quoted) and what was expected, so the caller can fix the issue. You MUST NEVER print secret material in error messages.

### Error handling
- You MUST ALWAYS check and handle errors. You MUST NEVER ignore a return value that can indicate failure unless there is a documented justification.
- Functions MUST report failure through their return value (`bool`, `int` with `0`/`-errno`, or `NULL`), following the convention already used by the surrounding code.
- Syscall wrappers MUST preserve `errno` for the caller when the convention requires it (e.g. return `-errno` and restore `errno` before returning).
- You MUST NEVER call `exit()` from library-like code paths; only top-level command entry points may terminate the process.
- You MUST NEVER use `assert()` as an input-validation mechanism in production code paths.

### Memory and string safety
- Every `malloc`/`calloc`/`realloc`/`strdup` result MUST be checked; on failure you MUST clean up and propagate the error.
- You MUST use bounded operations: `snprintf` over `sprintf`, explicit length tracking over `strcpy`/`strcat`. Fixed-size buffers MUST be provably large enough, and truncation MUST be either impossible or handled.
- All array indexing and pointer arithmetic MUST be provably in bounds; integer arithmetic on sizes and lengths MUST NOT overflow (check before, not after).
- String buffers built from external input MUST be NUL-terminated on every path.
- You MUST compile with ALL warnings enabled per the project's flags and fix EVERY warning — warnings are errors in spirit even when the build does not use `-Werror`.

### Secrets hygiene
- Key material and other secrets MUST NEVER be written to logs, error messages, or diagnostics. Printing secrets to stdout is allowed ONLY where it is the documented purpose of the command (e.g. key generation, explicit config export).
- Code handling secrets MUST follow the codebase's constant-time conventions where they exist (comparisons, encoders, character classification) and MUST NOT replace constant-time primitives with variable-time ones.
- Files created to hold secrets MUST be created with restrictive permissions (mode 0600 / umask 077 pattern), and world-readable destinations MUST produce a warning or an error, following the existing behavior.

### Output and diagnostics
- Diagnostics go to `stderr`; machine-readable/primary output goes to `stdout`. You MUST NOT mix them.
- Diagnostic messages MUST be actionable: what failed, on which input/interface, and the system error (`strerror(errno)`/`gai_strerror`) when applicable.
- Terminal escape sequences MUST go through the project's terminal abstraction (never raw ANSI writes scattered in logic code).

### Configuration
- You MUST NEVER hardcode secrets or environment-specific values.
- Environment variables MUST only tune tool behavior (colors, retries, hiding output) — NEVER carry primary configuration, which lives in configuration files or explicit arguments, per the project's conventions.
- All required configuration MUST be validated up front. Fail fast with a clear error message if anything is missing or invalid.

### Dependencies
- The standard C library and the platform's system interfaces are the ONLY default dependencies. You MUST NOT add third-party libraries unless the user EXPLICITLY approves; prefer vendoring minimal, license-compatible code over linking external packages, following the project's zero-dependency policy (see the project-specific rule file).
- Vendored code MUST keep its license header and MUST be tracked in the repository as-is, with local modifications clearly minimal.

## 3) Testing Rules — ABSOLUTE RULES

All references to "tests" in this document mean automated tests (unit, integration, and e2e) that run during development and in CI/CD pipelines.

### General principles
- Tests are MANDATORY for all changes. There are ZERO exceptions.
- Tests MUST be small, focused, and non-redundant while still covering: happy path, edge cases, failure modes.
- Tests MUST ALWAYS pass.
- Tests MUST NOT depend on execution order.
- Tests MUST clean up after themselves (temp files, sockets, spawned helpers).

### Frameworks — ABSOLUTE
- **Unity (ThrowTheSwitch) is THE unit-test framework**, vendored in-repo (single `unity.c` + headers, MIT license). You MUST NOT introduce any other test framework or any external test dependency.
- You MUST use **table-driven tests** as the default pattern for functions with multiple input/output cases: an array of case structs with a descriptive `name` field, iterated in a loop, with the case name included in every assertion message.
- You MUST follow the **Arrange-Act-Assert** pattern consistently.
- You MUST name test functions descriptively: `test_<unit>_<function>_<scenario>` (e.g. `test_config_parse_endpoint_rejects_missing_port`).
- Shared setup MUST be factored into helpers; copy-pasted setup across test files is FORBIDDEN.

```c
struct parse_port_case {
	const char *name;
	const char *input;
	bool want_ok;
	uint16_t want_port;
};

static void test_config_parse_port_variants(void)
{
	static const struct parse_port_case cases[] = {
		{ .name = "valid numeric port", .input = "51820", .want_ok = true, .want_port = 51820 },
		{ .name = "empty string", .input = "", .want_ok = false },
		{ .name = "out of range", .input = "70000", .want_ok = false },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		uint16_t port = 0;
		uint32_t flags = 0;
		bool ok = parse_port(&port, &flags, cases[i].input);

		TEST_ASSERT_EQUAL_MESSAGE(cases[i].want_ok, ok, cases[i].name);
		if (cases[i].want_ok)
			TEST_ASSERT_EQUAL_UINT16_MESSAGE(cases[i].want_port, port, cases[i].name);
	}
}
```

### Test organization
- Unit tests live in the project's dedicated test directory (see the project-specific rule file), one test file per unit under test: `config.c` → `config_test.c`.
- Test binaries are built by the project's standard Makefile targets — NEVER by ad-hoc compiler invocations committed to scripts.
- Tests MAY `#include` the `.c` file under test to reach `static` internals when the codebase uses that pattern (as the fuzz harnesses do); black-box testing through public headers is preferred when the API allows it.

### Unit tests
- Unit tests MUST be fast and MUST NOT hit real networks or require privileges.
- Filesystem-dependent tests MUST use temp files/directories created and removed by the test.
- External boundaries (syscalls, IPC endpoints) MUST be exercised through test seams: in-process fakes (e.g. a UNIX-socket server the test controls), injected file descriptors, or link-time substitution. You MUST NOT mock the C library wholesale.

### Fuzzing — MANDATORY for parsers
- EVERY parser of external input (config syntax, IPC responses, command-line grammars) MUST have a libFuzzer harness, following the project's existing fuzz-harness pattern.
- New parsing code MUST be added to an existing harness or get a new one IN THE SAME CHANGE.
- Fuzz harnesses MUST build with AddressSanitizer enabled (plus the project's sanitizer set).

### Sanitizers — MANDATORY
- The project's sanitizer gate (see the project-specific rule file; the standard set is **ASan + LSan + UBSan + MSan**) MUST pass on the FULL test suite.
- ASan+UBSan and MSan are SEPARATE builds; both MUST be run.
- Sanitizer findings are bugs. You MUST fix the root cause immediately. You MUST NEVER suppress a sanitizer report (no suppression files, no `__attribute__((no_sanitize))`) without EXPLICIT user approval for a documented, unavoidable conflict.

### Integration / end-to-end tests
- Integration tests MUST exercise real protocol surfaces in-process where possible (e.g. a test-owned UNIX socket speaking the real IPC wire format) — NEVER live networks.
- E2E flows that require privileges or kernel features MUST be clearly separated from the default test run and runnable via a dedicated target; the default `make`-driven test run MUST work unprivileged.
- **Testcontainers do NOT apply** to projects with no external service infrastructure; the project-specific rule file documents this.

### Manual testing documentation
- Manual tests are NOT a substitute for automated tests.
- If manual testing steps are necessary, they MUST be clearly labeled as "**Manual Test**" or "**Manual QA Steps**" and documented separately from automated test descriptions.

## 4) Quality Gates — ABSOLUTE RULES

### Definition of Done
A change MUST be considered DONE **ONLY AND ONLY** if ALL are true:

- All relevant automated tests are written AND passing (unit, integration, e2e, fuzz smoke as appropriate).
- **ZERO compiler warnings** with the project's warning flags, on EVERY supported platform target the change touches.
- **ZERO clang-tidy findings** (including the `clang-analyzer-*` checks) per the project's configuration.
- The sanitizer gate (ASan+LSan+UBSan and MSan builds) passes on the full test suite.
- The project builds without errors and without warnings via the standard Makefile targets.
- No TODOs, no commented-out dead code, no "temporary hacks".
- Changes are small, readable, and aligned with existing C patterns — you MUST NOT reformat code you are not otherwise changing.

### Fix broken tests — ABSOLUTE RULE
- You MUST fix ANY broken test, even if unrelated to your changes. Finish your current change first, then fix the broken test immediately.
- You MUST NEVER leave the test suite broken. There are ZERO exceptions.

### Fix broken linting — ABSOLUTE RULE
- You MUST fix ANY linting or static-analysis error, even if unrelated to your changes. Finish your current change first, then fix the violations immediately.
- You MUST NEVER leave the codebase with linting violations. There are ZERO exceptions.

### No linting suppression — ABSOLUTE RULE
- You MUST NEVER suppress, silence, or skip linting or analyzer rules (no `// NOLINT` comments, no check exclusions in the clang-tidy config, no baseline files) to make errors disappear.
- You MUST FIX the root cause of every finding by adjusting the implementation.
- The ONLY exception is when a rule GENUINELY and unavoidably conflicts with the project's documented design decisions. In that case, you MUST explain the conflict to the user and get EXPLICIT approval before adding any suppression. This is NON-NEGOTIABLE.

### Standard build/lint/test commands
- Build: the project's standard Makefile target (e.g. `make -C src`).
- Static analysis: `clang-tidy` per the project configuration; the Clang Static Analyzer additionally via the project's `check` target where wired.
- Unit tests: the project's standard test target.
- Fuzzers: the project's fuzz targets (clang required).
- Sanitizers: the project's sanitizer targets/builds (ASan+LSan+UBSan build and MSan build).

Project-specific Makefile targets and additional commands are defined in the project-specific rule file — the Makefile is the authoritative command surface.
