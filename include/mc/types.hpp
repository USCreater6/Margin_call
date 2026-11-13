// Margin_call core types. All prices are integer paise (1/100 rupee) to avoid float drift.
#pragma once
#include <cstdint>

namespace mc {

using Price = std::int64_t;   // paise
using Qty = std::int64_t;
using OrderId = std::uint64_t;
using Ts = std::int64_t;      // nanoseconds (simulated or monotonic)

enum class Side : std::uint8_t { Buy, Sell };
enum class OrdType : std::uint8_t { Market, Limit, Stop };

enum class State : std::uint8_t {
  New, RiskCheck, RejectedRisk, Sent, RejectedVenue, Acked, PartiallyFilled,
  Filled, CancelPending, Cancelled, ModifyPending, Expired
};

inline const char* to_string(State s) {
  switch (s) {
    case State::New: return "NEW"; case State::RiskCheck: return "RISK_CHECK";
    case State::RejectedRisk: return "REJECTED_RISK"; case State::Sent: return "SENT";
    case State::RejectedVenue: return "REJECTED_VENUE"; case State::Acked: return "ACKED";
    case State::PartiallyFilled: return "PARTIALLY_FILLED"; case State::Filled: return "FILLED";
    case State::CancelPending: return "CANCEL_PENDING"; case State::Cancelled: return "CANCELLED";
    case State::ModifyPending: return "MODIFY_PENDING"; case State::Expired: return "EXPIRED";
  }
  return "?";
}
inline const char* to_string(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }
inline const char* to_string(OrdType t) {
  return t == OrdType::Market ? "MARKET" : t == OrdType::Limit ? "LIMIT" : "STOP";
}

struct Bar {
  std::int32_t date = 0;   // YYYYMMDD
  Price o = 0, h = 0, l = 0, c = 0;
  std::int64_t vol = 0;
};

struct Order {
  OrderId id = 0;
  std::uint32_t inst = 0;
  std::uint16_t strat = 0;
  Side side = Side::Buy;
  OrdType type = OrdType::Market;
  Price px = 0;            // limit or stop price; unused for market
  Qty qty = 0;
  Qty filled = 0;
  State st = State::New;
  std::int32_t date = 0;
};

struct Fill {
  OrderId id = 0;
  std::uint32_t inst = 0;
  std::uint16_t strat = 0;
  Side side = Side::Buy;
  Price px = 0;
  Qty qty = 0;
  std::int32_t date = 0;
};

inline double rupees(Price p) { return static_cast<double>(p) / 100.0; }

}  // namespace mc
