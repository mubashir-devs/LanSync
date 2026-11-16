#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sys/stat.h>

#include "core/common/crypto.hpp"
#include "network/security/identity.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::security;

TEST(Identity, GenerateProducesConsistentIds) {
    auto id = generateIdentity("test-device");
    ASSERT_TRUE(id) << id.error().message;
    EXPECT_EQ(id->deviceId.size(), 64u);
    EXPECT_TRUE(crypto::isLowerHex(id->deviceId));
    EXPECT_NE(id->deviceId, id->fingerprint);   // key hash != cert hash
    EXPECT_NE(id->privateKeyPem.find("PRIVATE KEY"), std::string::npos);
}

TEST(Identity, TwoIdentitiesDiffer) {
    auto a = generateIdentity("a");
    auto b = generateIdentity("b");
    ASSERT_TRUE(a && b);
    EXPECT_NE(a->deviceId, b->deviceId);
}

TEST(Identity, IdentityFromPemRoundTripsAndRejectsMismatch) {
    auto a = generateIdentity("a");
    auto b = generateIdentity("b");
    auto again = identityFromPem(a->privateKeyPem, a->certificatePem);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->deviceId, a->deviceId);
    EXPECT_FALSE(identityFromPem(a->privateKeyPem, b->certificatePem));   // key != cert
    EXPECT_FALSE(identityFromPem("junk", a->certificatePem));
}

TEST(Identity, LoadOrCreatePersistsWithPrivateKeyPermissions) {
    support::TempDir dir;
    auto first = loadOrCreateIdentity(dir.path() / "id", "dev");
    ASSERT_TRUE(first);
    auto second = loadOrCreateIdentity(dir.path() / "id", "other-name");
    ASSERT_TRUE(second);
    EXPECT_EQ(first->deviceId, second->deviceId);

    struct stat st{};
    ASSERT_EQ(::stat((dir.path() / "id" / "identity.key").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST(Identity, IncompleteIdentityIsNotSilentlyReplaced) {
    support::TempDir dir;
    ASSERT_TRUE(loadOrCreateIdentity(dir.path(), "dev"));
    std::filesystem::remove(dir.path() / "identity.crt");
    EXPECT_FALSE(loadOrCreateIdentity(dir.path(), "dev"));
}

TEST(Identity, PairingCodeIsSymmetricSixDigitsAndBoundToCertsAndNonces) {
    const auto c = security::pairingCode("fpA", "n1", "fpB", "n2");
    EXPECT_EQ(c, security::pairingCode("fpB", "n2", "fpA", "n1"));   // both sides agree
    EXPECT_EQ(c.size(), 6u);
    EXPECT_NE(c, security::pairingCode("fpA", "n1", "fpC", "n2"));   // different certificate => different code
    EXPECT_NE(c, security::pairingCode("fpA", "n1", "fpB", "n3"));   // different nonce => different code
}

TEST(Identity, CommitmentBindsTheNonce) {
    const auto n = security::newPairingNonce();
    EXPECT_EQ(n.size(), 32u);
    EXPECT_NE(n, security::newPairingNonce());
    EXPECT_EQ(security::pairingCommitment(n), security::pairingCommitment(n));
    EXPECT_NE(security::pairingCommitment(n), security::pairingCommitment(n + "0"));
    EXPECT_EQ(security::pairingCommitment(n).size(), 64u);
}

// The point of commit-then-reveal: with nonces unknown in advance, ~10^6 ground candidates
// would be needed per attempt. Demonstrate that fixed certificates alone no longer decide the code.
TEST(Identity, CertificatesAloneDoNotDetermineTheCode) {
    std::set<std::string> codes;
    for (int i = 0; i < 200; ++i) {
        codes.insert(security::pairingCode("fpA", security::newPairingNonce(), "fpB", security::newPairingNonce()));
    }
    EXPECT_GT(codes.size(), 150u);   // fresh nonces => effectively random 6-digit codes
}
