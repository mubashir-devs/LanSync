# LanSync protocol (version 1)

Two layers: **UDP discovery** (unauthenticated hints) and a **TCP + TLS 1.3** session carrying
framed JSON messages and binary file chunks.

## 1. Discovery (UDP, default port 45454)

One JSON datagram (≤ 1024 bytes), sent every few seconds to the multicast group `239.255.77.77`
(per interface), each interface's broadcast address, and any configured unicast targets:

```json
{"app":"lansync","protocol_version":1,"message_type":"announce","device_id":"<hex>",
 "device_name":"...","app_version":"0.1.0","tcp_port":45455}
```

`message_type` is `announce` or `bye`. A device missing for 3 announce intervals (15 s by default) is
dropped. Receivers validate every field (types, lengths, hex id, port range) and ignore anything invalid.
The sender address is taken from the packet source, never from its content. **A discovery message never
authenticates anyone** and cannot grant any permission.

## 2. Framing (TCP)

```
+-----------------+-----------+----------------+
| length (u32 BE) | kind (u8) | payload        |      length = 1 + payload size
+-----------------+-----------+----------------+
kind 0 = JSON message         kind 1 = file chunk: payload = stream id (u32 BE) + raw bytes
```

* A read never equals a frame; `FrameDecoder` reassembles frames from arbitrary fragments.
* `length` is validated from the header alone (0 or > 8 MiB ⇒ connection closed) before any body is buffered.
* JSON frames are additionally capped at 1 MiB; unknown `kind` closes the connection.

## 3. Envelope

```json
{"protocol_version":1,"message_type":"index","request_id":"<hex ≤64>","device_id":"<hex>","payload":{}}
```

* `protocol_version` ≠ 1 ⇒ `error{code:"unsupported_version"}` then close.
* `device_id` must equal the id derived from the peer's **TLS certificate**; otherwise the session is dropped
  (a peer cannot claim someone else's id).
* Unknown `message_type`, bad ids, non-object payload ⇒ close.

## 4. Session

```
connect  ->  TLS 1.3 handshake (both present certificates)  ->  hello <-> hello  ->  ready
```

`hello {device_name, app_version}`. After hello the DeviceManager decides trust: the peer's device id
must be a paired device **and** its certificate fingerprint must equal the pinned one. Until trusted,
only `hello, ping, pong, error, pair_*` are accepted; anything else gets `error{code:"not_paired"}` and the
connection is closed. `ping`/`pong` keep idle connections alive (60 s idle timeout).

## 5. Pairing (untrusted session)

```
Initiator                                       Responder
pair_request {device_name, commit=H(nI)}  ───►
                                          ◄───  pair_commit {commit=H(nR)}
pair_reveal  {nonce=nI}                   ───►  (checks H(nI)==commit; then)
                                          ◄───  pair_reveal {nonce=nR}
both: code = 6 digits of SHA-256(sort[(fpA:nA),(fpB:nB)])    ── shown to both users ──
pair_decision {accepted}                  ◄──►  pair_decision {accepted}
```

`H(n) = SHA-256("lansync-pair-commit-v1" ‖ n)`. Commitments are exchanged before either nonce is revealed, so
neither side (nor an interceptor) can pick a nonce after seeing the other's. A reveal that does not match its
commitment aborts pairing without ever showing a code. Both users must approve; one-sided approval never
pairs. Both persist `{device id, name, public key, fingerprint}`. If both users start pairing simultaneously,
the attempt initiated by the smaller device id survives.

## 6. Synchronization (trusted session)

Folders are matched by `(folder name, peer device)`. Entries:

```json
{"path":"a/b.txt","dir":false,"size":123,"mtime":1700000000000,"hash":"<sha256 hex>",
 "deleted":false,"vv":{"<device id>":3}}
```

* `index {folder, entries[]}` — sent in full on connect, then for each change (≤ 500 entries/message, ≤ ~200 KB).
  Receivers validate every entry (path rules, hash format, sizes, version vector) and drop invalid ones.
* Decision per path from the version vectors: equal/local newer ⇒ nothing; remote newer ⇒ fetch / delete;
  concurrent ⇒ identical content merge, else **conflict** (see ADR 0006).

### File transfer (pull)

```
Receiver                                          Sender
file_request {folder,path,transfer_id,offset,hash} ──►  validates folder is shared, path, hash, size+mtime unchanged,
                                                        concurrency limit
                                              ◄──  file_begin {transfer_id,size,hash,mtime,offset}
                                              ◄──  chunk frame(s) [transfer_id | bytes]   (one in flight; next after write completes)
                                              ◄──  file_end {transfer_id}
file_done {transfer_id, ok, applied?, reason?} ──►
```

* `offset > 0` resumes: the receiver first re-hashes its existing part file, so the final whole-file SHA-256 covers
  resumed bytes too.
* Failures: `file_error {transfer_id, code: forbidden|not_found|changed|bad_offset|busy|io_error|aborted, message}`;
  receiver may send `file_cancel {transfer_id}` at any time.
* The receiver writes to `<folder>/.lansync/tmp/*.part`, verifies size + SHA-256, then moves the file into place
  atomically. A part file is never visible under the real name.

## 7. Limits

| Limit | Value |
|---|---|
| Frame | 8 MiB (JSON 1 MiB) |
| Index entries per message | ≤ 1000 accepted |
| Path / component | 4096 / 255 bytes |
| Version-vector entries | ≤ 64 |
| Handshake / idle timeout | 10 s / 60 s |
| Concurrent connections | 64 |
| Concurrent downloads / uploads per peer | setting (default 3) / 2× setting |
| Chunk size | setting 16 KB – 4 MB (default 256 KB) |
| Write queue per connection | 32 MiB |
