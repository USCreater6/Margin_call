// Demo exchange for bar-driven simulation. SIMULATION ONLY: it never talks to a real venue.
//
// Fill model (deliberately conservative):
//  * Orders decided on bar t execute from bar t+1.
//  * At each bar open a synthetic order-book ladder is built around the open price and
//    market / marketable orders are matched through the real matching engine, so price-time
//    priority, depth and partial fills are exercised. Market fills also pay slippage.
//  * A resting limit order fills only if the bar trades strictly through its price.
//  * A stop order triggers if the bar touches its price; on a gap it fills at the worse open.
#pragma once
#include <algorithm>
#include <limits>
#include <vector>
#include "mc/order_book.hpp"
#include "mc/osm.hpp"
#include "mc/types.hpp"

namespace mc {

struct ExchConfig {
  Price tick = 5;                 // paise
  int half_spread_ticks = 1;
  int slippage_ticks = 1;
  int levels = 5;
  Qty depth_per_level = 500;
  double participation = 0.10;    // max share of bar volume one resting order may take
  double fee_bps = 3.0;           // placeholder cost model; real fee and tax schedule must be verified
  double reject_prob = 0.0;       // injected venue rejects
  std::uint64_t seed = 42;
};

class Rng {   // splitmix64: small, fast, deterministic
 public:
  explicit Rng(std::uint64_t s) : s_(s) {}
  std::uint64_t next() {
    std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  double uni() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
 private:
  std::uint64_t s_;
};

class SimExchange {
 public:
  SimExchange(const ExchConfig& c, std::size_t n_inst)
      : cfg_(c), rng_(c.seed), work_(n_inst), incoming_(n_inst), books_(n_inst) {}

  const ExchConfig& config() const { return cfg_; }
  int live() const { return live_; }
  int osm_errors() const { return osm_errors_; }
  std::vector<Order>& closed() { return closed_; }
  double fee(Price px, Qty qty) const { return rupees(px) * static_cast<double>(qty) * cfg_.fee_bps / 1e4; }

  // Order must arrive in state SENT. Returns false on an (injected) venue reject.
  bool submit(Order o) {
    if (cfg_.reject_prob > 0 && rng_.uni() < cfg_.reject_prob) {
      advance(o, Ev::VenueReject);
      closed_.push_back(o);
      return false;
    }
    advance(o, Ev::VenueAck);
    incoming_[o.inst].push_back(o);
    ++live_;
    return true;
  }

  const Order* find(OrderId id) const {
    for (const auto* v : {&work_, &incoming_})
      for (const auto& per : *v)
        for (const auto& o : per) if (o.id == id) return &o;
    return nullptr;
  }

  bool cancel(OrderId id) {
    for (auto* v : {&work_, &incoming_})
      for (auto& per : *v)
        for (std::size_t i = 0; i < per.size(); ++i)
          if (per[i].id == id) {
            advance(per[i], Ev::CancelReq); advance(per[i], Ev::CancelAck);
            closed_.push_back(per[i]);
            per.erase(per.begin() + static_cast<std::ptrdiff_t>(i));
            --live_;
            return true;
          }
    return false;
  }

  // Cancels every live order matching pred. Returns how many were cancelled.
  template <class P>
  int cancel_if(P&& pred) {
    std::vector<OrderId> ids;
    for (auto* v : {&work_, &incoming_})
      for (auto& per : *v) for (auto& o : per) if (pred(o)) ids.push_back(o.id);
    for (OrderId id : ids) cancel(id);
    return static_cast<int>(ids.size());
  }
  int cancel_all() { return cancel_if([](const Order&) { return true; }); }

  template <class F>
  void on_open(std::uint32_t inst, const Bar& b, F&& on_fill) {
    merge(inst);
    OrderBook& book = books_[inst];
    book.clear();
    const Price open = round_tick(b.o), hs = cfg_.half_spread_ticks * cfg_.tick;
    auto noop = [](const Trade&) {};
    for (int k = 0; k < cfg_.levels; ++k) {
      book.add_limit(kMaker + static_cast<OrderId>(2 * k), Side::Sell, open + hs + k * cfg_.tick, cfg_.depth_per_level, noop);
      book.add_limit(kMaker + static_cast<OrderId>(2 * k + 1), Side::Buy, std::max(cfg_.tick, open - hs - k * cfg_.tick), cfg_.depth_per_level, noop);
    }
    const Price slip = cfg_.slippage_ticks * cfg_.tick;
    for (auto& o : work_[inst]) {
      const Qty rem = o.qty - o.filled;
      bool as_market = (o.type == OrdType::Market);
      if (o.type == OrdType::Stop) {
        const bool trig = (o.side == Side::Sell) ? open <= o.px : open >= o.px;
        if (!trig) continue;
        as_market = true;   // gapped through the stop: fill at the open, the worse price
      }
      if (as_market) {
        const Qty unf = book.add_market(o.id, o.side, rem, [&](const Trade& t) {
          const Price px = o.side == Side::Buy ? t.px + slip : std::max(cfg_.tick, t.px - slip);
          emit(o, px, t.qty, b.date, on_fill);
        });
        if (unf > 0) { advance(o, Ev::CancelReq); advance(o, Ev::CancelAck); }   // no more liquidity
      } else {   // limit
        const bool marketable = (o.side == Side::Buy) ? (book.best_ask() > 0 && o.px >= book.best_ask())
                                                      : (book.best_bid() > 0 && o.px <= book.best_bid());
        if (!marketable) continue;
        const Qty rest = book.add_limit(o.id, o.side, o.px, rem, [&](const Trade& t) { emit(o, t.px, t.qty, b.date, on_fill); });
        if (rest > 0) book.cancel(o.id);   // remainder keeps working outside the synthetic book
      }
    }
    sweep_done(inst);
  }

  template <class F>
  void on_range(std::uint32_t inst, const Bar& b, F&& on_fill) {
    merge(inst);
    const Qty cap = (cfg_.participation > 0 && b.vol > 0)
        ? std::max<Qty>(1, static_cast<Qty>(static_cast<double>(b.vol) * cfg_.participation))
        : std::numeric_limits<Qty>::max();
    const Price slip = cfg_.slippage_ticks * cfg_.tick;
    for (auto& o : work_[inst]) {
      const Qty rem = o.qty - o.filled;
      if (o.type == OrdType::Limit) {
        const bool through = (o.side == Side::Buy) ? b.l < o.px : b.h > o.px;
        if (through) emit(o, o.px, std::min(rem, cap), b.date, on_fill);
      } else if (o.type == OrdType::Stop) {
        const bool trig = (o.side == Side::Sell) ? b.l <= o.px : b.h >= o.px;
        if (trig) emit(o, o.side == Side::Sell ? std::max(cfg_.tick, o.px - slip) : o.px + slip, rem, b.date, on_fill);
      }
    }
    sweep_done(inst);
  }

 private:
  static constexpr OrderId kMaker = 1ULL << 62;

  Price round_tick(Price p) const { return std::max(cfg_.tick, (p / cfg_.tick) * cfg_.tick); }
  void advance(Order& o, Ev e) { State n; if (next_state(o.st, e, n)) o.st = n; else ++osm_errors_; }
  void merge(std::uint32_t inst) {
    auto& in = incoming_[inst];
    work_[inst].insert(work_[inst].end(), in.begin(), in.end());
    in.clear();
  }
  template <class F>
  void emit(Order& o, Price px, Qty q, std::int32_t date, F&& on_fill) {
    o.filled += q;
    advance(o, o.filled == o.qty ? Ev::FullFill : Ev::PartialFill);
    on_fill(Fill{o.id, o.inst, o.strat, o.side, px, q, date});
  }
  void sweep_done(std::uint32_t inst) {
    auto& w = work_[inst];
    for (std::size_t i = 0; i < w.size();) {
      if (is_terminal(w[i].st)) {
        closed_.push_back(w[i]);
        w.erase(w.begin() + static_cast<std::ptrdiff_t>(i));
        --live_;
      } else ++i;
    }
  }

  ExchConfig cfg_;
  Rng rng_;
  std::vector<std::vector<Order>> work_, incoming_;
  std::vector<OrderBook> books_;
  std::vector<Order> closed_;
  int live_ = 0, osm_errors_ = 0;
};

}  // namespace mc
