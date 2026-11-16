#pragma once
// Pure state machine for one pairing attempt (no I/O, fully unit-testable).
//
// Pairing succeeds only when BOTH users accept. Each side shows the same
// 6-digit code (see security::pairingCode); a man-in-the-middle would present
// different certificates to each side and so produce different codes.

namespace lansync::device {

class PairingState {
public:
    enum class Phase {
        AwaitingBoth,     // neither decision known
        AwaitingLocal,    // peer accepted, waiting for our user
        AwaitingRemote,   // our user accepted, waiting for peer
        Paired,
        Rejected,
    };

    [[nodiscard]] Phase phase() const;

    // Return the new phase. Decisions after a terminal phase are ignored.
    Phase onLocalDecision(bool accepted);
    Phase onRemoteDecision(bool accepted);

    [[nodiscard]] bool finished() const { return phase() == Phase::Paired || phase() == Phase::Rejected; }
    [[nodiscard]] bool localDecided() const { return local_ != Decision::None; }

private:
    enum class Decision { None, Accept, Reject };
    Decision local_{Decision::None};
    Decision remote_{Decision::None};
};

}  // namespace lansync::device
