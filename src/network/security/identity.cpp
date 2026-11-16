#include "network/security/identity.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

#include <sys/stat.h>

#include "core/common/crypto.hpp"

namespace lansync::security {

namespace {

template <typename T, void (*Free)(T*)>
struct Deleter { void operator()(T* p) const { if (p) Free(p); } };

using PkeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using X509Ptr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;

Error sslError(const std::string& what) { return Error{ErrorCode::SecurityError, what}; }

std::string bioToString(BIO* bio) {
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(len));
}

Result<std::string> publicKeyDer(EVP_PKEY* key) {
    unsigned char* der = nullptr;
    const int len = i2d_PUBKEY(key, &der);
    if (len <= 0) return sslError("cannot encode public key");
    std::string out(reinterpret_cast<char*>(der), static_cast<std::size_t>(len));
    OPENSSL_free(der);
    return out;
}

Result<std::string> publicKeyPem(EVP_PKEY* key) {
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (PEM_write_bio_PUBKEY(bio.get(), key) != 1) return sslError("cannot PEM-encode public key");
    return bioToString(bio.get());
}

std::string sha256Of(const std::string& bytes) {
    crypto::Sha256 h;
    h.update(bytes);
    return h.finishHex();
}

Result<std::string> certDer(X509* cert) {
    unsigned char* der = nullptr;
    const int len = i2d_X509(cert, &der);
    if (len <= 0) return sslError("cannot encode certificate");
    std::string out(reinterpret_cast<char*>(der), static_cast<std::size_t>(len));
    OPENSSL_free(der);
    return out;
}

}  // namespace

Result<PeerIdentity> peerFromCertificate(X509* cert) {
    if (!cert) return sslError("no certificate");
    PkeyPtr key(X509_get_pubkey(cert));
    if (!key) return sslError("certificate has no public key");
    // Only accept the key type we generate; refuse weak/unexpected key types.
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_EC) return sslError("unsupported key type");

    auto pubDer = publicKeyDer(key.get());
    if (!pubDer) return pubDer.error();
    auto pubPem = publicKeyPem(key.get());
    if (!pubPem) return pubPem.error();
    auto cDer = certDer(cert);
    if (!cDer) return cDer.error();

    return PeerIdentity{sha256Of(*pubDer), sha256Of(*cDer), std::move(*pubPem)};
}

Result<PeerIdentity> peerFromCertificateDer(std::span<const unsigned char> der) {
    const unsigned char* p = der.data();
    X509Ptr cert(d2i_X509(nullptr, &p, static_cast<long>(der.size())));
    if (!cert) return sslError("cannot parse certificate");
    return peerFromCertificate(cert.get());
}

Result<Identity> identityFromPem(std::string_view privateKeyPem, std::string_view certPem) {
    BioPtr kb(BIO_new_mem_buf(privateKeyPem.data(), static_cast<int>(privateKeyPem.size())));
    BioPtr cb(BIO_new_mem_buf(certPem.data(), static_cast<int>(certPem.size())));
    PkeyPtr key(PEM_read_bio_PrivateKey(kb.get(), nullptr, nullptr, nullptr));
    X509Ptr cert(PEM_read_bio_X509(cb.get(), nullptr, nullptr, nullptr));
    if (!key) return sslError("cannot parse private key");
    if (!cert) return sslError("cannot parse certificate");
    if (X509_check_private_key(cert.get(), key.get()) != 1) {
        return sslError("certificate does not match private key");
    }
    auto peer = peerFromCertificate(cert.get());
    if (!peer) return peer.error();
    Identity id;
    id.deviceId = peer->deviceId;
    id.fingerprint = peer->fingerprint;
    id.publicKeyPem = peer->publicKeyPem;
    id.privateKeyPem = std::string(privateKeyPem);
    id.certificatePem = std::string(certPem);
    return id;
}

Result<Identity> generateIdentity(std::string_view commonName) {
    PkeyPtr key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (!key) return sslError("key generation failed");

    X509Ptr cert(X509_new());
    if (!cert) return sslError("X509_new failed");
    X509_set_version(cert.get(), 2);

    // Random 64-bit serial.
    auto serialBytes = crypto::fromHex(crypto::randomHex(8));
    ASN1_INTEGER* serial = X509_get_serialNumber(cert.get());
    BIGNUM* bn = BN_bin2bn(serialBytes->data(), static_cast<int>(serialBytes->size()), nullptr);
    BN_to_ASN1_INTEGER(bn, serial);
    BN_free(bn);

    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -24 * 3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 10L * 365 * 24 * 3600);
    X509_set_pubkey(cert.get(), key.get());

    X509_NAME* name = X509_get_subject_name(cert.get());
    const std::string cn(commonName.substr(0, 64));
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char*>(cn.data()),
                               static_cast<int>(cn.size()), -1, 0);
    X509_set_issuer_name(cert.get(), name);
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) return sslError("signing failed");

    BioPtr kb(BIO_new(BIO_s_mem()));
    BioPtr cb(BIO_new(BIO_s_mem()));
    if (PEM_write_bio_PrivateKey(kb.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
        PEM_write_bio_X509(cb.get(), cert.get()) != 1) {
        return sslError("PEM encoding failed");
    }
    return identityFromPem(bioToString(kb.get()), bioToString(cb.get()));
}

Result<Identity> loadOrCreateIdentity(const std::filesystem::path& dir,
                                      std::string_view commonName) {
    const auto keyPath = dir / "identity.key";
    const auto certPath = dir / "identity.crt";
    auto slurp = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };

    const bool haveKey = std::filesystem::exists(keyPath);
    const bool haveCert = std::filesystem::exists(certPath);
    if (haveKey != haveCert) {
        return sslError("identity is incomplete in '" + dir.string() +
                        "' (one of identity.key/identity.crt is missing); refusing to replace it");
    }
    if (haveKey) return identityFromPem(slurp(keyPath), slurp(certPath));

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create '" + dir.string() + "': " + ec.message()};

    auto id = generateIdentity(commonName);
    if (!id) return id.error();

    // Create the key file with 0600 from the start (no window of wider access).
    {
        std::ofstream probe(keyPath, std::ios::binary | std::ios::trunc);
        if (!probe) return Error{ErrorCode::IoError, "cannot write identity key"};
    }
    ::chmod(keyPath.c_str(), 0600);
    {
        std::ofstream out(keyPath, std::ios::binary | std::ios::trunc);
        out << id->privateKeyPem;
        if (!out) return Error{ErrorCode::IoError, "cannot write identity key"};
    }
    {
        std::ofstream out(certPath, std::ios::binary | std::ios::trunc);
        out << id->certificatePem;
        if (!out) return Error{ErrorCode::IoError, "cannot write identity certificate"};
    }
    return id;
}

std::string newPairingNonce() { return crypto::randomHex(16); }

std::string pairingCommitment(std::string_view nonce) {
    crypto::Sha256 h;
    h.update("lansync-pair-commit-v1");
    h.update(nonce);
    return h.finishHex();
}

std::string pairingCode(std::string_view fpA, std::string_view nA, std::string_view fpB,
                        std::string_view nB) {
    std::string a = std::string(fpA) + ":" + std::string(nA);
    std::string b = std::string(fpB) + ":" + std::string(nB);
    if (b < a) std::swap(a, b);
    crypto::Sha256 h;
    h.update("lansync-pairing-v2");
    h.update(a);
    h.update("|");
    h.update(b);
    const auto digest = h.finish();
    const std::uint32_t v = (static_cast<std::uint32_t>(digest[0]) << 24) |
                            (static_cast<std::uint32_t>(digest[1]) << 16) |
                            (static_cast<std::uint32_t>(digest[2]) << 8) | digest[3];
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", v % 1000000u);
    return buf;
}

}  // namespace lansync::security
