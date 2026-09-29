# Implementation status

This checklist records only phases that have compiled and passed their relevant tests.

- [x] Phase 0 — repository bootstrap and build/test foundation
- [x] Phase 1 — RESP2 codec and parser fuzz target
- [x] Phase 2 — epoll server and socket integration
- [x] Phase 3 — typed storage and core commands
- [x] Phase 4 — lazy and active expiration
- [x] Phase 5 — transactions and optimistic concurrency
- [x] Phase 6 — append-only persistence and recovery
- [x] Phase 7 — pub/sub
- [x] Phase 8 — dependency-aware invalidation
- [x] Phase 9 — hardening, packaging, and documentation

## Environment notes

The initial execution environment provided CMake 3.22.1 and GCC 11.4.0. The project
keeps compatibility with those tools for local verification while documenting the
supported portfolio baseline of CMake 3.24+ and GCC 12+.

The libFuzzer harness is present, but the initial environment did not provide Clang,
so it was not executed during Phase 1 verification.

## Final verification — 2026-08-30

- Pristine Debug configure/build (`build/pristine-debug`, Unix Makefiles, GCC 11.4): passed
  with the strict project warning set and no project warnings.
- Fresh Debug CTest: 27/27 passed, including child-process loopback integration and
  recovery tests.
- ASan/UBSan configure/build and CTest (`build/asan-local`): 27/27 passed with no
  sanitizer findings.
- Release configure/build (`build/acceptance-release`): `keyforge_core`,
  `keyforge-server`, and `keyforge-cli` passed; both version commands executed.
- Release manual CLI smoke: SET preserved the `hello world` argv item, GET returned it,
  KFG.INVALIDATE removed it, and Ctrl+C produced a clean server shutdown.
- Benchmark shell syntax check: passed.

Ninja, Clang, clang-format, clang-tidy, and Redis CLI/benchmark tools were unavailable.
The Docker executable exposed by Windows reported that WSL integration was disabled.
Therefore the Ninja presets, libFuzzer run, formatting/static-analysis tools, Docker
build, redis-cli compatibility smoke, and benchmark measurements were not executed in
this environment. Their exact commands are documented in the README and CI covers GCC,
Clang, Ninja, sanitizers, and a short fuzz run.
