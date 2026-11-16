# ADR 0003: `Result<T>` for errors, spdlog for logging

**Status:** Accepted

## Decision
- Fallible operations return `lansync::Result<T>` (`Error{code, message}`),
  a minimal C++20 stand-in for `std::expected`. Exceptions are not used across
  module boundaries; they remain acceptable for programmer errors and for
  third-party calls (e.g. spdlog sink creation), caught at the entry points.
  When the project moves to C++23 this can be replaced by `std::expected`.
- `Result` is `[[nodiscard]]`, so ignoring an error is a compiler warning.
- Logging uses one named spdlog logger (`lansync`) with a rotating file sink
  (5 MB x 3) and an optional console sink, configured by the entry point.
  Modules log through `LS_*` macros; there is no per-module global state besides
  spdlog's registry.
- Time is injected through `IClock` so time-dependent logic is testable.

## Consequences
- Call sites are explicit about failure, at the cost of some verbosity.
- Log level is changeable at runtime (`log::setLevel`) for the Settings page.
