# ADR 0007: Threading and ownership

**Status:** Accepted (implemented, verified with ASan/UBSan and TSan)

* One **Engine thread** (single-threaded `boost::asio::io_context`) owns all stateful components and the SQLite
  connection. No component uses locks.
* **Blocking disk work** (scan, hashing, resume prefix hashing, local file copy) runs on a small `WorkerPool`;
  results are posted back, and every continuation re-looks-up its object by id and checks a weak "alive" token, so
  callbacks cannot touch destroyed objects.
* **Connections/sessions** are `shared_ptr`; async handlers capture `shared_from_this`; owners clear handlers on
  close to break reference cycles. A session reaches its owner through an interface pointer that is cleared when
  the session closes.
* **Front-ends** talk to the `Engine` only through `async/run/call/snapshotAsync`. The Qt adapter never blocks the
  GUI thread; it coalesces engine events into one refresh and converts snapshots into QML properties.
* **Shutdown order**: stop timers/sessions on the engine thread → stop the io_context → join → destroy components
  (member declaration order guarantees the worker pool is joined before the io_context dies).

## Trade-off
Chunk reads/writes happen on the engine thread. On a LAN this keeps the design simple and race-free; a slow disk
can delay other transfers. Moving chunk I/O to the pool is a documented future change.
