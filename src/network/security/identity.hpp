#pragma once
// Device identity: an ECDSA P-256 key pair plus a self-signed X.509 certificate.
//
//   device ID   = hex SHA-256 of the DER-encoded public key (SubjectPublicKeyInfo)
//   fingerprint = hex SHA-256 of the DER-encoded certificate
//
// There is no CA. Peers are authenticated by *pinning*: after pairing, a peer is
// accepted only if its TLS certificate's public key hashes to the stored device
// ID. All cryptography is OpenSSL; nothing is hand-rolled.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "core/common/result.hpp"

typedef struct x509_st X509;

namespace lansync::security {

struct PeerIdentity {
    std::string deviceId;
    std::string fingerprint;
    std::string publicKeyPem;
};

struct Identity {
    std::string deviceId;
    std::string fingerprint;
    std::string publicKeyPem;
    std::string privateKeyPem;
    std::string certificatePem;
};

Result<Identity> generateIdentity(std::string_view commonName);

// Loads identity.key / identity.crt from `dir`, creating them (key mode 0600)
// when absent. A corrupt or mismatched pair is an error, never silently replaced.
Result<Identity> loadOrCreateIdentity(const std::filesystem::path& dir,
                                      std::string_view commonName);

Result<Identity> identityFromPem(std::string_view privateKeyPem, std::string_view certPem);

Result<PeerIdentity> peerFromCertificate(X509* cert);
Result<PeerIdentity> peerFromCertificateDer(std::span<const unsigned char> der);

// Pairing verification (short authentication string, SAS).
//
// A code derived only from the two persistent certificates would let an active
// attacker who has seen both certificates grind keys offline until both victims
// show the same digits. So each side first COMMITS to a fresh random nonce
// (commitment = SHA-256 of it), and only after both commitments were exchanged
// are the nonces REVEALED. The attacker's certificates are fixed by the TLS
// handshake and the nonces cannot be chosen after the fact, leaving only a
// ~1-in-10^6 chance per (visible, abortable) attempt.
std::string newPairingNonce();                               // 128-bit random, hex
std::string pairingCommitment(std::string_view nonce);       // hex SHA-256 (domain separated)

// 6 digits over both certificates and both nonces. Symmetric: both devices
// compute the same code regardless of argument order.
std::string pairingCode(std::string_view fingerprintA, std::string_view nonceA,
                        std::string_view fingerprintB, std::string_view nonceB);

}  // namespace lansync::security
