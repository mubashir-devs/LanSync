# LanSync

Secure, self-hosted file synchronization between computers on a local network.
No cloud service, no account, no central server.

LanSync keeps chosen folders identical on paired computers. Devices find each
other on the LAN, you approve pairing on **both** screens by comparing a short code,
and from then on changes flow over mutually authenticated TLS 1.3. When the same file
is edited on two machines at once, **both versions are kept**.

> **Status: MVP, verified on Linux only.** Everything in the acceptance workflow
> (discover → pair → share → sync → modify back → detect concurrent edits) is
> implemented and covered by automated tests, including two separate OS processes
> syncing real files. It has **not** been run on two physical machines, on Windows,
> or on macOS. See [Verification status](#verification-status) and
> [Known limitations](#known-limitations) before relying on it.

## Features

| Area | What works |
|---|---|
| Discovery | UDP multicast + subnet broadcast on every interface (+ unicast targets); announcements are unauthenticated hints; devices appear and disappear (goodbye + timeout) |
| Pairing | Explicit approval on both devices; commit-then-reveal 6-digit code; persistent device identity (ECDSA P-256 cert); pinned fingerprint; revoke any time |
| Transport | TLS 1.3, mutual authentication, length-prefixed framing, strict message/size validation, idle & handshake timeouts, connection limits, private-network-only by default |
| Change detection | inotify watcher (Linux) + periodic full reconciliation; metadata short-circuit so unchanged files are never re-hashed; SHA-256 content hashes |
| Sync | Per-file version vectors, bidirectional / send-only / receive-only folders, directories, deletions (tombstones), explicit state machine per folder, pause/resume |
| Transfers | Chunked streaming (configurable chunk size), bounded concurrency, progress / speed / ETA, pause / resume / cancel, resume from a verified part file, SHA-256 verification, atomic replace, partial files never visible |
| Conflicts | Deterministic winner keeps the name, loser kept as `name.sync-conflict-<time>-<id>.ext` on **both** devices; choose keep local / keep remote / keep both |
| Versions | Replaced and deleted files are archived; browse and restore; configurable retention (count and age) |
| UI | Qt 6 / QML: Dashboard, Devices, Sync Folders, Transfers, Conflicts, History, Settings; loading and empty states, toasts, confirmation dialogs, responsive layout |
| Headless | `lansync-cli daemon` runs the full engine without Qt |

## Quick start (two computers)

1. Install LanSync on both (see [Build](#build) or the `.deb`/`.tar.gz` from `cpack`).
2. Start the app on both. They appear under **Devices** within a few seconds.
3. On one, press **Pair**. Both screens show the same 6-digit code. If the digits match,
   press **Codes match — Approve** on both. If they differ, press **Reject**.
4. On each computer open **Sync Folders → Add folder**, choose the folder, give it the
   **same name** on both sides, and pick the other device.
5. Files now sync. Check **Transfers**, **History** and **Conflicts**.

Firewall: allow TCP 45455 (sync) and UDP 45454 (discovery); both are configurable in Settings.

## Build

Dependencies: C++20 compiler (tested with GCC 15.2), CMake ≥ 3.25, Ninja, Qt ≥ 6.5 (6.10.2 tested,
desktop only), Boost ≥ 1.80 (headers), OpenSSL ≥ 3, SQLite ≥ 3.37, spdlog, nlohmann-json, GoogleTest.

**Ubuntu/Debian (apt, no vcpkg needed):**

```bash
scripts/install_deps_ubuntu.sh          # apt-get install of everything needed (needs sudo)
cmake --preset dev && cmake --build --preset dev
```

`vcpkg.json` is the canonical dependency manifest for reproducible builds and future
Windows/macOS support (`export VCPKG_ROOT=...; cmake --preset vcpkg`). **The vcpkg path has not been
exercised yet.** Dependencies are resolved with plain `find_package`, so a missing one fails at
configure time.

| Preset | Purpose |
|---|---|
| `dev` | Debug, everything |
| `release` | RelWithDebInfo |
| `core-only` | Core + CLI + tests, **no Qt** (proves the core is UI-independent) |
| `asan` | AddressSanitizer + UBSan |
| `tsan` | ThreadSanitizer (no desktop) |
| `vcpkg` | Release through the vcpkg toolchain |

Packages: `cmake --preset release && cmake --build --preset release && (cd build/release && cpack)`
produces a `.tar.gz` and (on Debian-like systems) a `.deb`.

## Run

```bash
build/dev/bin/lansync                                    # desktop app
build/dev/bin/lansync-cli daemon                         # headless engine
build/dev/bin/lansync-cli --help
```

Data directory: `$XDG_DATA_HOME/lansync` or `~/.local/share/lansync` (database, identity key
`identity.key` mode 0600, versions, logs).

Two instances on one machine (no multicast needed):

```bash
# terminal 1
LANSYNC_DATA_DIR=/tmp/a LANSYNC_LISTEN_PORT=45455 LANSYNC_DISCOVERY_PORT=45470 \
  LANSYNC_DISCOVERY_TARGET=127.0.0.1:45471 build/dev/bin/lansync
# terminal 2
LANSYNC_DATA_DIR=/tmp/b LANSYNC_LISTEN_PORT=45555 LANSYNC_DISCOVERY_PORT=45471 \
  LANSYNC_DISCOVERY_TARGET=127.0.0.1:45470 build/dev/bin/lansync
```

## Testing

```bash
ctest --preset dev                 # 154 tests, ~1 minute
ctest --preset asan                # same suite under ASan + UBSan
ctest --preset tsan                # same suite under ThreadSanitizer
ctest --preset core-only           # without Qt
scripts/system_test.sh build/dev/bin/lansync-cli    # two separate processes (also run by ctest)
```

| Level | Covers |
|---|---|
| Unit | hashing, `Result`, SQLite wrapper, migrations, logging, settings, path validation, scanner + change detection, version vectors, diff decisions, conflict naming, sync state machine, chunk writer (resume, corruption), version store + retention, framing, message validation, identity, pairing code |
| Integration | real TLS on loopback (framing, limits, timeouts), UDP discovery, pairing flows (approve, reject, spoofed id, forged reveal, simultaneous), real inotify, **two full engines**: discover → pair → share → sync → modify back, nested dirs, rename, delete, concurrent edit + resolution, 24 MB file, resume + corrupt part file, reconnect, unpair, receive-only, forged file requests, settings persistence |
| System | two `lansync-cli` processes: pairing, sync, 200 MB file, deletion, process restart + catch-up, concurrent edits |

All tests use temporary directories and need no network beyond loopback. Sanitizer runs are clean
on the current code.

## Verification status

| Verified (automated) | Not verified |
|---|---|
| Linux x86-64, GCC 15.2, Ubuntu 26.04, Qt 6.10.2 | Two physical computers on a real LAN |
| Core: Debug, Qt-free, ASan/UBSan, TSan all pass | Multicast/broadcast discovery on a real network (loopback unicast is tested; the multicast path exists but is untested) |
| Two processes syncing over loopback TLS | Windows, macOS (platform code is written but **never compiled**) |
| Desktop UI renders every page (checked by screenshots, no QML warnings) | Interactive UI flows by clicking (add-folder, versions, settings dialogs were not driven or screenshotted) |
| `.deb` / `.tar.gz` contents | Installing the `.deb`; vcpkg builds |

## Performance (measured, not promised)

Release build, one machine, loopback, 2 processes, including SHA-256 at both ends and TLS 1.3:

| Transfer | Result |
|---|---|
| 1 GiB file | 4.9 s end-to-end, byte-identical |
| Peak RSS during that transfer | 16 MiB (sender), 15 MiB (receiver) — memory does not grow with file size |

Real LAN throughput depends on the network and disks and has not been measured.

## Security summary

* **Identity**: per-device ECDSA P-256 key + self-signed cert; device ID = SHA-256(public key).
* **Trust**: only a *paired* device whose certificate fingerprint matches the one pinned at pairing is accepted. Discovery grants nothing.
* **Pairing**: both users approve; the code uses commit-then-reveal nonces so a man-in-the-middle cannot grind certificates to force matching digits (success chance ~10⁻⁶ per visible attempt).
* **Transport**: TLS 1.3 only, mutual certs, bounded frames (8 MiB; JSON 1 MiB), timeouts, connection cap, non-private source addresses refused unless enabled.
* **Filesystem**: every peer path is validated (no `..`, absolute, drive, backslash, control chars, reserved `.lansync`), resolved inside the folder, and symlinked ancestors are refused. Only registered folders are served. Temp files are 0600; replacements are atomic; replaced content is archived first.
* **Integrity**: SHA-256 over the whole file is verified before anything is placed; mismatching part files are deleted.

Details: [docs/architecture.md](docs/architecture.md), [docs/protocol.md](docs/protocol.md), [docs/adr/](docs/adr).

## Project layout

```
apps/cli, apps/desktop     Front-ends (desktop = thin Qt adapter + QML)
src/core/common            Result<T>, clock, logging, SHA-256/random, path safety, worker pool
src/core/application       Application (DB + migrations), Engine (the one entry point), Settings
src/core/device            DeviceManager (trust, pairing), PeerSession (authenticated connection)
src/core/sync              SyncManager, version vectors, diff, state machine, local index
src/core/transfer          ChunkWriter (verified, resumable receive)
src/core/conflict          deterministic winner + conflict naming
src/core/versioning        VersionStore + retention
src/network                protocol (framing, messages), security (identity), transport (TLS), discovery
src/filesystem             scanner, watcher interface
src/storage                SQLite wrapper, migrator, repositories
src/platform               linux/ macos/ windows/ posix/  (the only OS-specific code)
migrations/                NNNN_name.sql, embedded into the binary
tests/unit, tests/integration, scripts/system_test.sh
docs/                      architecture, protocol, ADRs
```

Dependency direction: `apps → application → core → (network, filesystem, storage) → common`.
Nothing below `apps/desktop` links Qt.

## Development

```bash
cmake --preset dev && cmake --build --preset dev        # build
ctest --preset dev --output-on-failure                  # test
build/dev/tests/lansync_unit_tests --gtest_filter='EndToEnd.*'   # one suite
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

Add a migration: create `migrations/000N_name.sql` (never edit a shipped one) and re-run CMake.
Developer aid: `LANSYNC_SCREENSHOT_DIR=<dir> LANSYNC_WINDOW_SIZE=600x700 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software lansync`
renders every page to PNG and exits.

## Known limitations

* **Platforms**: only Linux has been built and run. macOS/Windows have no native file watcher
  (periodic scans only), Windows has no network-interface enumeration, and none of that code
  has ever been compiled.
* **Folder model**: a folder syncs with exactly one paired device, matched by name. There is no
  multi-device mesh for one folder.
* **Not synced**: symlinks (skipped), permissions, ownership, extended attributes. Case-insensitive
  filesystem collisions, Unicode normalization differences and Windows reserved names are not handled.
* **Renames** are detected as delete + create; an identical local file is reused so nothing is re-downloaded,
  but history shows a delete and a new file.
* **Resume** is tested by planting a partial file; it has not been tested by killing a transfer mid-stream.
  A part file is only reused if the remote content hash is unchanged.
* **Scale**: the peer's index is held in memory (memory grows with file count); deletion tombstones are never
  garbage collected; no ignore patterns; no rate limiting / bandwidth cap.
* **I/O**: disk reads/writes for transfer chunks happen on the network thread (fine on a LAN, a slow disk
  can delay other transfers); archiving across filesystems copies on that thread too.
* **Security**: the identity key is stored unencrypted (file mode 0600); certificates are valid for 10 years
  with no rotation; the 6-digit code has ~20 bits of strength (attempts are visible and abortable); discovery
  datagrams can be spoofed to cause connection attempts (never trust). `--auto-approve-pairing` in the CLI
  disables code comparison and exists only for tests.
* **Conflicts**: clock skew can change *which* version keeps the name, never whether both are kept.
* **UI**: English only, light theme only, no OS-level notifications (in-app toasts only), no accessibility audit.
* **IPv6**: discovery is IPv4 only.

## Future improvements

* Windows (`ReadDirectoryChangesW`, adapters) and macOS (FSEvents) backends and CI for all three platforms.
* Multi-device folders (share one folder with several peers), selective sync and ignore patterns.
* Rolling/block-level delta transfer for large modified files.
* Bandwidth limits, QoS, pause-on-battery/metered networks.
* Encrypted identity key (OS keychain), certificate rotation.
* True rename tracking, tombstone garbage collection, index paging to disk for huge trees.
* OS notifications, tray icon, dark theme, translations, accessibility pass.
* Packaging: signed installers (MSI/DMG/AppImage/Flatpak).
