// Order-state machine. Every state change in the system goes through next_state().
#pragma once
#include "mc/types.hpp"

namespace mc {

enum class Ev : std::uint8_t {
  Submit, RiskPass, RiskReject, VenueAck, VenueReject, PartialFill, FullFill,
  CancelReq, CancelAck, ModifyReq, ModifyAck, Expire
};

inline bool is_terminal(State s) {
  return s == State::RejectedRisk || s == State::RejectedVenue || s == State::Filled ||
         s == State::Cancelled || s == State::Expired;
}
inline bool is_live(State s) {
  return s == State::Acked || s == State::PartiallyFilled || s == State::CancelPending ||
         s == State::ModifyPending;
}

// Returns true and writes the new state if (s, e) is a legal transition.
inline bool next_state(State s, Ev e, State& out) {
  const bool working = (s == State::Acked || s == State::PartiallyFilled);
  switch (e) {
    case Ev::Submit:      if (s == State::New) { out = State::RiskCheck; return true; } break;
    case Ev::RiskPass:    if (s == State::RiskCheck) { out = State::Sent; return true; } break;
    case Ev::RiskReject:  if (s == State::RiskCheck) { out = State::RejectedRisk; return true; } break;
    case Ev::VenueAck:    if (s == State::Sent) { out = State::Acked; return true; } break;
    case Ev::VenueReject: if (s == State::Sent) { out = State::RejectedVenue; return true; } break;
    case Ev::PartialFill:
      if (working) { out = State::PartiallyFilled; return true; }
      if (s == State::CancelPending || s == State::ModifyPending) { out = s; return true; }
      break;
    case Ev::FullFill:
      if (working || s == State::CancelPending || s == State::ModifyPending) { out = State::Filled; return true; }
      break;
    case Ev::CancelReq:   if (working) { out = State::CancelPending; return true; } break;
    case Ev::CancelAck:   if (s == State::CancelPending) { out = State::Cancelled; return true; } break;
    case Ev::ModifyReq:   if (working) { out = State::ModifyPending; return true; } break;
    case Ev::ModifyAck:   if (s == State::ModifyPending) { out = State::Acked; return true; } break;
    case Ev::Expire:      if (working) { out = State::Expired; return true; } break;
  }
  return false;
}

}  // namespace mc
