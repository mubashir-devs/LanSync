#include <gtest/gtest.h>

#include "core/application/settings.hpp"
#include "core/device/pairing_state.hpp"

using namespace lansync;
using device::PairingState;
using Phase = PairingState::Phase;

TEST(Settings, DefaultsRoundTripThroughMap) {
    app::Settings s;
    s.chunkSizeKb = 512;
    s.deviceName = "box";
    s.allowPublicNetworks = true;
    auto back = app::Settings::fromMap(s.toMap());
    EXPECT_EQ(back.chunkSizeKb, 512);
    EXPECT_EQ(back.deviceName, "box");
    EXPECT_TRUE(back.allowPublicNetworks);
}

TEST(Settings, OutOfRangeValuesAreClamped) {
    auto s = app::Settings::fromMap({{"chunk_size_kb", "999999"},
                                     {"max_concurrent_transfers", "0"},
                                     {"reconcile_interval_sec", "-5"},
                                     {"log_level", "shouting"},
                                     {"listen_port", "abc"}});
    EXPECT_EQ(s.chunkSizeKb, 4096);
    EXPECT_EQ(s.maxConcurrentTransfers, 1);
    EXPECT_EQ(s.reconcileIntervalSec, 5);
    EXPECT_EQ(s.logLevel, "info");
    EXPECT_EQ(s.listenPort, 45455);   // unparsable -> default kept
}

TEST(PairingState, BothMustAccept) {
    PairingState p;
    EXPECT_EQ(p.phase(), Phase::AwaitingBoth);
    EXPECT_EQ(p.onLocalDecision(true), Phase::AwaitingRemote);
    EXPECT_EQ(p.onRemoteDecision(true), Phase::Paired);
}

TEST(PairingState, RemoteFirstThenLocal) {
    PairingState p;
    EXPECT_EQ(p.onRemoteDecision(true), Phase::AwaitingLocal);   // peer's accept alone never pairs
    EXPECT_FALSE(p.finished());
    EXPECT_EQ(p.onLocalDecision(true), Phase::Paired);
}

TEST(PairingState, AnyRejectIsTerminalAndSticky) {
    PairingState p;
    p.onLocalDecision(true);
    EXPECT_EQ(p.onRemoteDecision(false), Phase::Rejected);
    EXPECT_EQ(p.onRemoteDecision(true), Phase::Rejected);   // cannot be flipped afterwards
    PairingState q;
    EXPECT_EQ(q.onLocalDecision(false), Phase::Rejected);
    EXPECT_EQ(q.onRemoteDecision(true), Phase::Rejected);
}

TEST(PairingState, DecisionsCannotBeChangedOnceMade) {
    PairingState p;
    p.onLocalDecision(true);
    p.onLocalDecision(false);   // ignored
    EXPECT_EQ(p.onRemoteDecision(true), Phase::Paired);
}
