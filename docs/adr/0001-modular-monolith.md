# ADR 0001: Modular monolith with a Qt-free core

**Status:** Accepted

## Context
LanSync needs a UI, networking, storage and a sync engine. The sync engine must
be reusable without the desktop interface and testable headlessly.

## Decision
One process, built from static libraries with one-way dependencies:

```
apps/{desktop,cli} -> lansync_app -> lansync_storage -> lansync_common
```

- `lansync_common`: `Result<T>`, `IClock`, logging, platform abstraction.
- `lansync_storage`: SQLite wrapper and migrations.
- `lansync_app`: `Application` composition root; future services are registered here.
- Qt is linked **only** by `apps/desktop`. `AppController` is the single adapter
  turning core state into QML properties.
- The `core-only` CMake preset builds core + CLI + tests with no Qt, so the
  boundary is enforced by the build rather than by convention.

Later phases add `network`, `filesystem`, `sync`, `transfer` libraries beneath
`lansync_app`, each depending only on lower layers.

## Consequences
- The CLI doubles as the system-test driver for two-instance tests.
- No microservices; IPC is only the LAN protocol between devices.
- Services are constructed in one place and injected (clock, config), which keeps
  unit tests deterministic.
