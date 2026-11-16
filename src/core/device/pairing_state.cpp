#include "core/device/pairing_state.hpp"

namespace lansync::device {

PairingState::Phase PairingState::phase() const {
    if (local_ == Decision::Reject || remote_ == Decision::Reject) return Phase::Rejected;
    if (local_ == Decision::Accept && remote_ == Decision::Accept) return Phase::Paired;
    if (local_ == Decision::Accept) return Phase::AwaitingRemote;
    if (remote_ == Decision::Accept) return Phase::AwaitingLocal;
    return Phase::AwaitingBoth;
}

PairingState::Phase PairingState::onLocalDecision(bool accepted) {
    if (!finished() && local_ == Decision::None) local_ = accepted ? Decision::Accept : Decision::Reject;
    return phase();
}

PairingState::Phase PairingState::onRemoteDecision(bool accepted) {
    if (!finished() && remote_ == Decision::None) remote_ = accepted ? Decision::Accept : Decision::Reject;
    return phase();
}

}  // namespace lansync::device
