# ADR 0002: System packages locally, vcpkg manifest as canonical list

**Status:** Accepted

## Context
Development happens on Linux where all dependencies are available via apt.
Windows/macOS support is planned and needs reproducible dependency resolution.
Building Qt and Boost from source through vcpkg is slow and unnecessary on Linux.

## Decision
- `vcpkg.json` is the canonical manifest of dependencies.
- `cmake/Dependencies.cmake` uses plain `find_package()` for everything, so it
  works unchanged with system packages and with the vcpkg toolchain file
  (`vcpkg` preset).
- No local vcpkg bootstrap is required on Linux.
- All required dependencies use `REQUIRED`; nothing is silently skipped.
  Boost and OpenSSL are resolved now even though they are first used in Phase 2.

## Consequences
- Version drift between apt and vcpkg is possible. Minimum versions are declared
  in `find_package()` and CI on the vcpkg preset must be added before releases.
- The vcpkg manifest has not been exercised on this machine (not installed); it
  is unverified until a vcpkg build is run.
