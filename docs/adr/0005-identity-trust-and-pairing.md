# ADR 0005: Device identity, trust and pairing

**Status:** Accepted (implemented)

## Decisions
* **Identity**: each device generates an ECDSA P-256 key and a self-signed X.509 certificate on first run
  (key file mode 0600). **Device ID = SHA-256 of the DER public key**; fingerprint = SHA-256 of the DER certificate.
  There is no CA.
* **Transport**: TLS 1.3 only, mutual authentication. The TLS layer accepts any well-formed self-signed
  certificate; *who* the peer is gets decided by the application (pinning), never by the TLS verify callback.
* **Trust**: a session is trusted only if the peer's id is a *paired* device and its fingerprint equals the pinned
  one. A hello whose `device_id` differs from the certificate-derived id is dropped. Pre-trust sessions may only
  speak `hello/ping/pong/error/pair_*`.
* **Discovery is not trust**: UDP announcements are unauthenticated hints used only to find an address.
* **Pairing needs two approvals** and a verification code both users compare.

## Why commit-then-reveal
A first implementation derived the 6-digit code from the two persistent fingerprints only. An active
man-in-the-middle can read both devices' certificates beforehand (pre-pairing TLS is open) and then generate
keys offline (~10⁶ cheap ECDSA key generations) until both victims display the same digits. The code would
then prove nothing.

Pairing now uses commit-then-reveal: each side commits to a fresh 128-bit nonce (`SHA-256(domain ‖ nonce)`),
nonces are revealed only after both commitments exist, and the code covers both certificates and both nonces.
The attacker's certificates are fixed by the handshake and the nonces cannot be chosen after the fact, so the
remaining chance of a successful interception is about 1 in 10⁶ per attempt, each of which is visible and
abortable. A reveal that does not match its commitment aborts pairing with no prompt (tested).

## Consequences
* The code has ~20 bits of strength; this is the standard SAS trade-off and is documented as such.
* Revocation = unpair: the pinned key and fingerprint are cleared; a re-pair is required.
* The private key is not encrypted at rest (file permissions only); rotation is not implemented.
* Simultaneous initiation is resolved deterministically: the attempt started by the smaller device id wins.
