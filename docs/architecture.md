# Architecture

LanSync is a modular monolith: one process, static libraries with one-way dependencies, no Qt below the UI.

```
 apps/desktop (Qt/QML)        apps/cli (headless)
        │  AppController            │
        └───────────────┬───────────┘
                        ▼
                  core/application   Engine (one thread) ── Application (DB+migrations) ── Settings
                        │
      ┌─────────────────┼───────────────────────────┐
      ▼                 ▼                           ▼
 core/device        core/sync                 network/discovery
 DeviceManager      SyncManager               (UDP announce/listen)
 PeerSession        diff · version vectors
      │             state machine · local index
      │             ├─ core/transfer (ChunkWriter)
      │             ├─ core/conflict · core/versioning
      ▼             ▼
 network/transport (TLS 1.3, framing)   filesystem (scanner, watcher)   storage (SQLite, repositories)
 network/security (identity)            platform/{linux,macos,windows,posix}
                        └──────────────── core/common (Result, crypto, logging, paths, worker pool)
```

## Threading model

* **Engine thread** — a single-threaded `io_context` runs *everything stateful*: discovery, TLS server and
  connections, `DeviceManager`, `SyncManager`, and all SQLite access. Components therefore need no locks, and
  the database is confined to one thread.
* **Worker pool (2 threads)** — blocking work only: folder scans + hashing, hashing a part file prefix for
  resume, copying an identical local file. Results are posted back with `asio::post`; every handler re-validates
  state (folder removed? download cancelled? shutdown?) using a weak "alive" token.
* **Watcher thread** — one inotify thread per folder; it only posts "something changed" to the engine thread.
* **UI thread** — never blocks: commands go through `Engine::run/async`, state arrives as an immutable `Snapshot`
  via `snapshotAsync`, coalesced every 150 ms. `AppController` is the only Qt↔core translation.
* Lifetimes: connections and sessions are `shared_ptr`s captured by handlers; owners clear handlers on close to
  break cycles; the engine stops the io thread before components are destroyed (declaration order enforces it).
  Verified with ASan/UBSan and TSan.

## A file's journey (A creates `x.txt`)

1. inotify event → debounce 400 ms → scan on a worker (stat; hash only new/changed) → `applyScan` in one
   transaction: new row, version vector `{A:1}`.
2. A evaluates paths, then announces the entries in an `index` message.
3. B validates entries, `decide(local=∅, remote)` ⇒ **Fetch**; queued (bounded concurrency).
4. B sends `file_request`; A validates (shared folder, safe path, hash, file unchanged) and streams chunks.
5. B writes `.lansync/tmp/*.part` while hashing; at `file_end` it verifies size + SHA-256.
6. B re-checks the decision against current state, archives any replaced file, sets mtime, renames atomically,
   updates the row with A's version vector, sends `file_done`, and announces its new state.
7. Both sides mark the file synced; both UIs show history/transfer results.

## Per-folder state machine

`Idle → Scanning → Comparing → Queued → Transferring → Verifying → Completed | Conflict | Failed`,
`Paused` reachable from anywhere (resume goes through `Idle`). Transitions are validated
(`core/sync/sync_state.cpp`); the manager derives the target state from observable facts and moves there.

## Data

SQLite (WAL, foreign keys, STRICT tables). Tables: `devices`, `sync_folders`, `files` (local index incl. tombstones
and version vectors), `transfers`, `file_versions`, `conflicts`, `sync_history`, `settings`, `schema_migrations`.
Forward-only migrations are embedded into the binary; a database newer than the binary is refused.

## Where the safety properties live

| Property | Mechanism |
|---|---|
| No untrusted sync | `DeviceManager::onSessionHello` (paired + fingerprint pin), `PeerSession` pre-trust message allowlist |
| No path traversal | `pathsafety::validateRelativePath` + `resolveInside` on every peer path and every apply |
| No silent overwrite | re-decide at apply time; conflict copies; version archive before replace/delete |
| No partial files | part files in `.lansync/tmp`, verify then atomic rename |
| Missed events | periodic reconciliation scan; scanner never treats an unreadable/missing root as "all deleted" |
| Resource limits | frame/JSON/entry/path caps, connection cap, timeouts, bounded queues and concurrency, disk-space check |
