// Central risk engine. Every order from every strategy passes check() before it reaches a venue.
#pragma once
#include <algorithm>
#include <limits>
#include "mc/types.hpp"

namespace mc {

struct RiskLimits {
  double stop_pct = 0.02;            // per-trade stop, fraction of entry price (fixed by owner)
  double profit_lock = 0.10;         // daily profit lockout (fixed by owner)
  double daily_loss = 0.03;          // global daily loss limit (approved proposal)
  double strat_daily_loss = 0.015;   // per-strategy daily loss, fraction of account equity
  double max_order_notional = 0.25;  // of equity
  double max_gross = 1.0;            // of equity; 1.0 = no leverage
  int max_positions = 3;
  int max_open_orders = 6;
  int rate_per_sec = 8;              // hard cap; every order, modify, cancel counts
};

enum class Reject : std::uint8_t {
  None, BadOrder, Killed, RateLimit, Halted, ProfitLock, StratHalted,
  Notional, OpenOrders, Positions, Gross, NoShort, Cash, COUNT
};
inline const char* to_string(Reject r) {
  switch (r) {
    case Reject::None: return "NONE"; case Reject::BadOrder: return "BAD_ORDER";
    case Reject::Killed: return "KILL_SWITCH"; case Reject::RateLimit: return "RATE_LIMIT";
    case Reject::Halted: return "DAILY_LOSS_HALT"; case Reject::ProfitLock: return "PROFIT_LOCKOUT";
    case Reject::StratHalted: return "STRATEGY_HALT"; case Reject::Notional: return "ORDER_NOTIONAL";
    case Reject::OpenOrders: return "MAX_OPEN_ORDERS"; case Reject::Positions: return "MAX_POSITIONS";
    case Reject::Gross: return "GROSS_EXPOSURE"; case Reject::NoShort: return "NO_SHORT";
    case Reject::Cash: return "INSUFFICIENT_CASH"; case Reject::COUNT: break;
  }
  return "?";
}

// Token bucket on an injected clock, so it is deterministic in simulation.
class TokenBucket {
 public:
  TokenBucket(int rate_per_sec, int burst)
      : rate_(rate_per_sec), cap_(burst), tokens_(burst) {}
  bool take(Ts now_ns) {
    if (!init_) { last_ = now_ns; init_ = true; }
    if (now_ns > last_) {
      tokens_ = std::min(cap_, tokens_ + static_cast<double>(now_ns - last_) * 1e-9 * rate_);
      last_ = now_ns;
    }
    if (tokens_ >= 1.0) { tokens_ -= 1.0; return true; }
    return false;
  }
 private:
  double rate_, cap_, tokens_;
  Ts last_ = 0;
  bool init_ = false;
};

struct RiskCtx {
  double equity = 0;       // rupees
  double gross = 0;        // rupees, sum of |position value|
  int open_orders = 0;
  int positions = 0;
  Qty pos_qty = 0;         // signed position of this strategy in this instrument
  double ref_px = 0;       // rupees
  bool strat_halted = false;
  // Open (not yet filled) orders, so several orders together cannot break a limit.
  Qty pending_sell_qty = 0;          // open sell quantity in this instrument
  double pending_buy_notional = 0;   // rupees, open buy orders across all instruments
  double cash = std::numeric_limits<double>::infinity();   // rupees available; infinity = not checked
  bool long_only = true;             // no short selling (no leverage until approved)
};

class RiskEngine {
 public:
  explicit RiskEngine(const RiskLimits& l) : lim_(l), bucket_(l.rate_per_sec, l.rate_per_sec) {}

  const RiskLimits& limits() const { return lim_; }
  bool killed() const { return killed_; }
  bool halted() const { return halted_; }
  bool locked() const { return locked_; }
  void kill() { killed_ = true; }
  void rearm() { killed_ = false; }

  void start_day(double equity) { day_start_ = equity; halted_ = false; locked_ = false; }

  // Returns true if a daily halt or lockout was newly triggered.
  bool on_equity(double equity) {
    const double pnl = equity - day_start_;
    bool fired = false;
    if (!halted_ && pnl <= -lim_.daily_loss * day_start_) { halted_ = true; fired = true; }
    if (!locked_ && pnl >= lim_.profit_lock * day_start_) { locked_ = true; fired = true; }
    return fired;
  }
  double day_start() const { return day_start_; }
  void shift_day_start(double delta) { day_start_ += delta; }   // deposits are not profit

  // An order that only reduces an existing position (counting other open orders on the same side)
  // is always allowed unless the kill switch is on.
  static bool reduces(const Order& o, Qty pos, Qty pending_same_side = 0) {
    return (o.side == Side::Sell && pos > 0 && o.qty + pending_same_side <= pos) ||
           (o.side == Side::Buy && pos < 0 && o.qty + pending_same_side <= -pos);
  }

  Reject check(const Order& o, const RiskCtx& c, Ts now_ns) {
    if (o.qty <= 0 || c.ref_px <= 0) return Reject::BadOrder;
    if (killed_) return Reject::Killed;
    const bool red = reduces(o, c.pos_qty, o.side == Side::Sell ? c.pending_sell_qty : 0);
    if (c.long_only && o.side == Side::Sell && !red) return Reject::NoShort;
    if (!red) {
      if (halted_) return Reject::Halted;
      if (locked_) return Reject::ProfitLock;
      if (c.strat_halted) return Reject::StratHalted;
      const double notional = static_cast<double>(o.qty) * c.ref_px;
      if (notional > lim_.max_order_notional * c.equity) return Reject::Notional;
      if (c.open_orders >= lim_.max_open_orders) return Reject::OpenOrders;
      if (c.pos_qty == 0 && c.positions >= lim_.max_positions) return Reject::Positions;
      if (c.gross + c.pending_buy_notional + notional > lim_.max_gross * c.equity) return Reject::Gross;
      if (o.side == Side::Buy && notional + c.pending_buy_notional > c.cash) return Reject::Cash;
    }
    if (!bucket_.take(now_ns)) return Reject::RateLimit;   // last, so vetoed orders use no token
    return Reject::None;
  }

  // Cancels and modifies also count against the rate cap.
  bool take_rate_token(Ts now_ns) { return bucket_.take(now_ns); }

 private:
  RiskLimits lim_;
  TokenBucket bucket_;
  double day_start_ = 0;
  bool killed_ = false, halted_ = false, locked_ = false;
};

}  // namespace mc
