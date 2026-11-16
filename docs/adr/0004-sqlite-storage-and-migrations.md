# ADR 0004: SQLite storage, embedded forward-only migrations

**Status:** Accepted

## Decision
- SQLite via a small RAII wrapper (`Database`, `Statement`, `Transaction`).
  All values are bound as parameters; SQL is never built from user input.
- Connection pragmas: `foreign_keys=ON`, `journal_mode=WAL` (file DBs),
  `synchronous=NORMAL`, `busy_timeout=5000`.
- Tables use `STRICT` and `CHECK` constraints so invalid states (e.g. unknown
  pairing status) are rejected by the database. Timestamps are UTC milliseconds.
- Migrations live in `migrations/NNNN_name.sql`. CMake embeds them into a
  generated source file, so binaries are self-contained.
- Each migration runs in its own `BEGIN IMMEDIATE` transaction together with
  the `schema_migrations` bookkeeping row; failure leaves the previous version.
- A database whose version is newer than the binary is **refused**, not opened.
- Migrations are forward-only; shipped migrations are never edited.
- Beyond the spec's tables, the v1 schema adds `conflicts` and `settings`, and
  `files` gains `is_directory`, `deleted` (tombstones for delete propagation)
  and `version_vector` (JSON) for concurrent-edit detection (Phase 5).

## Threading
`Database` is single-thread-confined, and that thread is the Engine thread (ADR 0007): every repository and
transaction runs there, so no locking is needed and the UI never touches the database (it receives
immutable snapshots). Migration 0002 later reshaped `transfers` (identified by folder + path, nullable file
link) and added a unique index on `(display_name, remote_device_id)` because folders are matched across
devices by that pair.

## Consequences
- Adding a migration needs a CMake re-run (file glob uses `CONFIGURE_DEPENDS`).
- Hot-path writes (progress updates) must be batched to limit write volume.
