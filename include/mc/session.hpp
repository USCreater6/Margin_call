// Live simulation session behind the terminal. SIMULATION ONLY.
// Prices are synthetic ticks. Orders go: state machine -> risk engine -> real matching engine.
// Synthetic market makers are re-quoted every tick; the user's resting orders keep price-time priority.
#pragma once
#include <cmath>
#include <sstream>
#include <string>
#include <vector>
#include "mc/exchange.hpp"
#include "mc/order_book.hpp"
#include "mc/osm.hpp"
#include "mc/portfolio.hpp"
#include "mc/risk.hpp"

namespace mc {

class LiveSim {
 public:
  struct Rec { Order o; std::string src, why, code; double fill_val = 0; std::int64_t wall = 0;
    double avg_fill() const { return o.filled > 0 ? fill_val / static_cast<double>(o.filled) : 0.0; } };
  struct Evt { std::int64_t wall; std::string k, m; };
  struct Led { std::int64_t wall; std::string k; double a, b; };

  LiveSim(double capital, const RiskLimits& lim, std::uint64_t seed, double fee_bps = 3.0)
      : capital_(capital), lim_(lim), fee_bps_(fee_bps), rng_(seed), pf_(capital, 1), risk_(lim) {
    risk_.start_day(capital);
    ledger_.push_back({0, "OPENING DEMO BALANCE", capital, capital});
  }

  // `ref` is the reference price that day-change is measured against (e.g. the last real close);
  // `source` says where the starting price came from and is shown in the terminal.
  void add_instrument(const std::string& sym, const std::string& venue, Price px, Price tick,
                      const std::string& source = "placeholder", Qty lot = 1) {
    inst_.push_back(Inst{sym, venue, px, px, tick, OrderBook(256), source, lot});
    marks_.push_back(px);
    quote(inst_.size() - 1);
  }
  std::size_t instruments() const { return inst_.size(); }
  int find_symbol(const std::string& s) const {
    for (std::size_t i = 0; i < inst_.size(); ++i) if (inst_[i].sym == s) return static_cast<int>(i);
    return -1;
  }
  // Wall clock in ms. When the India (IST) calendar day changes, a new risk day starts:
  // the daily loss halt and profit lockout are cleared only here, never by re-arming.
  void set_wall(std::int64_t ms) {
    wall_ = ms;
    if (ledger_.size() == 1 && ledger_[0].wall == 0) ledger_[0].wall = ms;
    const std::int64_t day = (ms + 19800000LL) / 86400000LL;   // IST = UTC+05:30
    if (day_ != 0 && day != day_) {
      risk_.start_day(equity());
      log("NEW DAY", "New trading day: daily loss and profit limits measured from current equity");
    }
    day_ = day;
  }
  void set_price(std::size_t i, Price px) { inst_[i].px = px; marks_[i] = px; }   // test hook

  double equity() const { return pf_.equity(marks_); }
  double base() const { return capital_ + dep_; }
  double cash() const { return pf_.cash(); }
  Position position(std::size_t i) const { return pf_.pos(0, static_cast<std::uint32_t>(i)); }
  int live_orders() const { return live_; }
  bool killed() const { return risk_.killed(); }
  bool halted() const { return risk_.halted(); }
  bool locked() const { return risk_.locked(); }
  bool stub() const { return stub_; }
  const std::vector<Rec>& orders() const { return orders_; }
  const std::vector<Evt>& events() const { return events_; }
  Price price(std::size_t i) const { return inst_[i].px; }
  Price best_ask(std::size_t i) const { return inst_[i].book.best_ask(); }
  Price best_bid(std::size_t i) const { return inst_[i].book.best_bid(); }

  // Returns an empty string if accepted, otherwise the reason it was vetoed.
  std::string submit(std::size_t i, Side side, OrdType type, Qty qty, Price px, const std::string& src, Ts now, bool force = false) {
    Inst& in = inst_[i];
    Rec r;
    r.o.id = orders_.size() + 1; r.o.inst = static_cast<std::uint32_t>(i); r.o.side = side; r.o.type = type;
    r.o.px = type == OrdType::Limit ? px : 0; r.o.qty = qty; r.src = src; r.wall = wall_;
    adv(r.o, Ev::Submit);
    std::string why;
    if (!force) {
      if (type == OrdType::Stop) why = "Stop orders are not available in the terminal yet";
      else if (type == OrdType::Limit && (px <= 0 || px % in.tick != 0)) why = "Limit price must be a positive multiple of the tick size";
      else if (self_cross(i, side, type, px)) { why = "Self-trade prevention: it would match your own resting order"; r.code = "SELF_TRADE"; }
      else {
        RiskCtx c;
        c.equity = equity(); c.gross = pf_.gross(marks_); c.open_orders = live_; c.positions = pf_.open_positions();
        c.pos_qty = position(i).qty;
        // a market buy is checked at the best ask it will actually pay, not the last price
        c.ref_px = rupees(type == OrdType::Limit ? px : (side == Side::Buy && in.book.best_ask() > 0 ? in.book.best_ask() : in.px));
        c.cash = pf_.cash();
        c.long_only = true;
        for (const Rec& q : orders_) {
          if (!is_live(q.o.st)) continue;
          const Qty rem = q.o.qty - q.o.filled;
          if (q.o.side == Side::Sell && q.o.inst == i) c.pending_sell_qty += rem;
          if (q.o.side == Side::Buy) c.pending_buy_notional += rupees(q.o.px) * static_cast<double>(rem) * (1.0 + fee_bps_ / 1e4);
        }
        const Reject rj = risk_.check(r.o, c, now);
        why = human(rj);
        if (rj != Reject::None) r.code = to_string(rj);
      }
      if (r.code.empty() && !why.empty()) r.code = "BAD_ORDER";
    }
    if (!why.empty()) {
      adv(r.o, Ev::RiskReject); r.why = why;
      orders_.push_back(r);
      log("RISK VETO", tag(r.o.id) + " (" + src + ") " + why);
      return why;
    }
    adv(r.o, Ev::RiskPass); adv(r.o, Ev::VenueAck);
    orders_.push_back(r);
    ++live_; stamps_.push_back(now);
    const OrderId id = r.o.id;
    auto cb = [&](const Trade& t) { on_trade(t); };
    if (type == OrdType::Market) {
      const Qty unf = in.book.add_market(id, side, qty, cb);
      if (unf > 0) { Rec& R = orders_[id - 1]; adv(R.o, Ev::CancelReq); adv(R.o, Ev::CancelAck); --live_; log("CANCEL", tag(id) + " remainder cancelled: no liquidity"); }
    } else {
      in.book.add_limit(id, side, px, qty, cb);
      log("ORDER", "SIM " + tag(id) + " " + to_string(side) + " " + std::to_string(qty) + " " + in.sym + " LIMIT " + money(px));
    }
    return "";
  }

  bool cancel(OrderId id, Ts now) {
    if (id == 0 || id > orders_.size()) return false;
    Rec& r = orders_[id - 1];
    if (!is_live(r.o.st)) return false;
    risk_.take_rate_token(now); stamps_.push_back(now);   // cancels count against the rate cap
    inst_[r.o.inst].book.cancel(id);
    adv(r.o, Ev::CancelReq); adv(r.o, Ev::CancelAck);
    --live_;
    log("CANCEL", tag(id) + " cancelled");
    return true;
  }
  int cancel_all(Ts now) {
    int n = 0;
    for (std::size_t k = 0; k < orders_.size(); ++k) if (is_live(orders_[k].o.st)) n += cancel(k + 1, now);
    log("CANCEL ALL", std::to_string(n) + " open simulated order(s) cancelled");
    return n;
  }
  void flatten(Ts now) {
    cancel_all(now);
    for (std::size_t i = 0; i < inst_.size(); ++i) close_position(i, now);
    log("FLATTEN", "All simulated positions closed");
  }
  void kill(bool also_flatten, Ts now) {
    if (also_flatten) flatten(now); else cancel_all(now);
    risk_.kill(); stub_ = false;
    log("KILL SWITCH", also_flatten ? "Activated with flatten" : "Activated");
  }
  // Re-arming clears the kill switch only. A daily loss halt or profit lockout stays until the next day.
  void rearm() {
    risk_.rearm();
    std::string m = "Kill switch cleared.";
    if (risk_.halted()) m += " Daily loss halt still active until the next trading day.";
    if (risk_.locked()) m += " Profit lockout still active until the next trading day.";
    log("RE-ARM", m);
  }
  bool fund(double amount) {
    if (!(amount > 0) || amount > 1e12) return false;
    pf_.deposit(amount); dep_ += amount; risk_.shift_day_start(amount);
    ledger_.push_back({wall_, "DEMO FUNDING", amount, base()});
    log("WALLET", "Demo wallet funded with " + std::to_string(static_cast<long long>(amount)));
    return true;
  }
  void reset() {
    for (auto& in : inst_) in.book.clear();
    orders_.clear(); events_.clear(); ledger_.clear(); curve_.clear(); stamps_.clear();
    pf_ = Portfolio(capital_, 1); risk_ = RiskEngine(lim_); risk_.start_day(capital_);
    dep_ = fees_ = turn_ = 0; live_ = 0; stub_ = false;
    for (auto& in : inst_) { in.px = in.open; marks_[static_cast<std::size_t>(&in - &inst_[0])] = in.open; }
    ledger_.push_back({wall_, "OPENING DEMO BALANCE", capital_, capital_});
    for (std::size_t i = 0; i < inst_.size(); ++i) quote(i);
    log("RESET", "Demo account reset");
  }
  void set_stub(bool on) { stub_ = on && !risk_.killed(); log("AI STUB", stub_ ? "Stub signals started" : "Stub signals stopped"); }

  // One market tick.
  void step(Ts now, bool move = true) {
    ++ticks_;
    for (std::size_t i = 0; i < inst_.size(); ++i) {
      Inst& in = inst_[i];
      if (move) {
        const double next = static_cast<double>(in.px) * (1.0 + (rng_.uni() - 0.5) * 0.0016);
        in.px = std::max(in.tick, static_cast<Price>(std::llround(next / static_cast<double>(in.tick))) * in.tick);
      }
      marks_[i] = in.px;
      quote(i);
    }
    for (std::size_t i = 0; i < inst_.size(); ++i) {   // 2% per-trade stop
      const Position p = position(i);
      if (p.qty == 0) continue;
      const double u = (rupees(inst_[i].px) - p.avg) * static_cast<double>(p.qty);
      if (u <= -lim_.stop_pct * std::fabs(static_cast<double>(p.qty)) * p.avg) {
        log("STOP", "2% per-trade stop hit on " + inst_[i].sym);
        for (std::size_t k = 0; k < orders_.size(); ++k)
          if (is_live(orders_[k].o.st) && orders_[k].o.inst == i) cancel(k + 1, now);
        close_position(i, now);
      }
    }
    const bool was_h = risk_.halted(), was_l = risk_.locked();
    risk_.on_equity(equity());
    if (!was_h && risk_.halted()) { cancel_all(now); log("HALT", "Global daily loss limit reached. New risk blocked."); }
    if (!was_l && risk_.locked()) log("LOCKOUT", "Daily profit target reached. New risk blocked.");
    if (stub_ && ticks_ % 4 == 0 && !inst_.empty()) {
      const std::size_t i = rng_.next() % inst_.size();
      // long-only: the stub sells only what is held
      const Side s = (position(i).qty > 0 && (rng_.next() & 1)) ? Side::Sell : Side::Buy;
      log("AI STUB", std::string("Random stub signal: ") + to_string(s) + " 1 " + inst_[i].sym);
      submit(i, s, OrdType::Market, 1, 0, "AI-STUB", now);
    }
    curve_.push_back(equity() - base());
    if (curve_.size() > 900) curve_.erase(curve_.begin());
    prune(now);
  }

  std::string state_json(std::size_t sel, Ts now) {
    prune(now);
    if (sel >= inst_.size()) sel = 0;
    std::ostringstream o;
    o.precision(12);
    o << "{\"engine\":\"cpp\",\"label\":\"SIMULATION\",\"start\":" << capital_ << ",\"cash\":" << pf_.cash() << ",\"dep\":" << dep_
      << ",\"real\":" << pf_.realized(0) << ",\"fees\":" << fees_ << ",\"turn\":" << turn_ << ",\"killed\":" << (risk_.killed() ? "true" : "false")
      << ",\"halt\":" << (risk_.halted() ? "true" : "false") << ",\"lock\":" << (risk_.locked() ? "true" : "false")
      << ",\"stub\":" << (stub_ ? "true" : "false") << ",\"rate\":" << stamps_.size() << ",\"sel\":" << sel << ",\"inst\":[";
    for (std::size_t i = 0; i < inst_.size(); ++i)
      o << (i ? "," : "") << "{\"s\":\"" << esc(inst_[i].sym) << "\",\"v\":\"" << esc(inst_[i].venue) << "\",\"p\":" << rupees(inst_[i].px)
        << ",\"o\":" << rupees(inst_[i].open) << ",\"t\":" << rupees(inst_[i].tick) << ",\"lot\":" << inst_[i].lot
        << ",\"src\":\"" << esc(inst_[i].source) << "\"}";
    o << "],\"book\":{\"asks\":[";
    if (!inst_.empty()) {
      bool f = true;
      inst_[sel].book.levels(Side::Sell, 4, [&](Price p, Qty q) { o << (f ? "" : ",") << "[" << rupees(p) << "," << q << "]"; f = false; });
      o << "],\"bids\":[";
      f = true;
      inst_[sel].book.levels(Side::Buy, 4, [&](Price p, Qty q) { o << (f ? "" : ",") << "[" << rupees(p) << "," << q << "]"; f = false; });
    } else o << "],\"bids\":[";
    o << "]},\"pos\":[";
    bool first = true;
    for (std::size_t i = 0; i < inst_.size(); ++i) {
      const Position p = position(i);
      if (p.qty == 0) continue;
      o << (first ? "" : ",") << "{\"s\":\"" << esc(inst_[i].sym) << "\",\"q\":" << p.qty << ",\"avg\":" << p.avg << "}";
      first = false;
    }
    o << "],\"orders\":[";
    const std::size_t from = orders_.size() > 200 ? orders_.size() - 200 : 0;
    for (std::size_t k = orders_.size(); k-- > from;) {
      const Rec& r = orders_[k];
      o << (k + 1 == orders_.size() ? "" : ",") << "{\"id\":\"" << tag(r.o.id) << "\",\"w\":" << r.wall << ",\"src\":\"" << esc(r.src)
        << "\",\"side\":\"" << to_string(r.o.side) << "\",\"q\":" << r.o.qty << ",\"f\":" << r.o.filled << ",\"s\":\"" << esc(inst_[r.o.inst].sym)
        << "\",\"type\":\"" << to_string(r.o.type) << "\",\"px\":" << rupees(r.o.px) << ",\"fp\":" << r.avg_fill() << ",\"code\":\"" << esc(r.code) << "\""
        << ",\"st\":\"" << to_string(r.o.st) << "\",\"why\":\"" << esc(r.why) << "\"}";
    }
    o << "],\"audit\":[";
    const std::size_t ef = events_.size() > 150 ? events_.size() - 150 : 0;
    for (std::size_t k = events_.size(); k-- > ef;)
      o << (k + 1 == events_.size() ? "" : ",") << "{\"w\":" << events_[k].wall << ",\"k\":\"" << esc(events_[k].k) << "\",\"m\":\"" << esc(events_[k].m) << "\"}";
    o << "],\"ledger\":[";
    for (std::size_t k = ledger_.size(); k-- > 0;)
      o << (k + 1 == ledger_.size() ? "" : ",") << "{\"w\":" << ledger_[k].wall << ",\"k\":\"" << ledger_[k].k << "\",\"a\":" << ledger_[k].a << ",\"b\":" << ledger_[k].b << "}";
    o << "],\"curve\":[";
    for (std::size_t k = 0; k < curve_.size(); ++k) o << (k ? "," : "") << curve_[k];
    o << "]}";
    return o.str();
  }

 private:
  struct Inst { std::string sym, venue; Price px, open, tick; OrderBook book; std::string source; Qty lot; };
  static constexpr OrderId kMaker = 1ULL << 62;

  static std::string tag(OrderId id) { std::string s = std::to_string(id); return "SIM-" + std::string(s.size() < 4 ? 4 - s.size() : 0, '0') + s; }
  static std::string money(Price p) { std::ostringstream o; o.setf(std::ios::fixed); o.precision(2); o << rupees(p); return o.str(); }
  static std::string esc(const std::string& s) {
    std::string r;
    for (char c : s) { if (c == '"' || c == '\\') r += '\\'; if (static_cast<unsigned char>(c) >= 32) r += c; }
    return r;
  }
  void adv(Order& o, Ev e) { State n; if (next_state(o.st, e, n)) o.st = n; }
  void log(const std::string& k, const std::string& m) {
    events_.push_back({wall_, k, m});
    if (events_.size() > 2000) events_.erase(events_.begin(), events_.begin() + 1000);
  }
  void prune(Ts now) {
    std::size_t k = 0;
    while (k < stamps_.size() && now - stamps_[k] >= 1000000000LL) ++k;
    stamps_.erase(stamps_.begin(), stamps_.begin() + static_cast<std::ptrdiff_t>(k));
  }
  std::string human(Reject r) const {
    auto pc = [](double x) { return std::to_string(static_cast<int>(std::lround(x * 100))) + "%"; };
    switch (r) {
      case Reject::None: return "";
      case Reject::BadOrder: return "Quantity and price must be positive";
      case Reject::Killed: return "Kill switch is active";
      case Reject::RateLimit: return "Rate cap reached: " + std::to_string(lim_.rate_per_sec) + " orders per second";
      case Reject::Halted: return "Daily loss limit hit: no new risk";
      case Reject::ProfitLock: return "Daily profit lockout: no new risk";
      case Reject::StratHalted: return "Strategy daily loss limit hit";
      case Reject::Notional: return "Order value exceeds " + pc(lim_.max_order_notional) + " of equity";
      case Reject::OpenOrders: return "Maximum open orders reached (" + std::to_string(lim_.max_open_orders) + ")";
      case Reject::Positions: return "Maximum open positions reached (" + std::to_string(lim_.max_positions) + ")";
      case Reject::Gross: return "Gross exposure would exceed " + pc(lim_.max_gross) + " of equity";
      case Reject::NoShort: return "Long-only: you can sell only what you hold, after open sell orders";
      case Reject::Cash: return "Not enough cash: order value plus open buy orders exceeds available cash";
      case Reject::COUNT: break;
    }
    return "Rejected";
  }
  bool self_cross(std::size_t i, Side side, OrdType type, Price px) const {
    for (const Rec& r : orders_) {
      if (!is_live(r.o.st) || r.o.inst != i || r.o.side == side) continue;
      if (type == OrdType::Market) return true;
      if (side == Side::Buy ? r.o.px <= px : r.o.px >= px) return true;
    }
    return false;
  }
  void close_position(std::size_t i, Ts now) {
    const Position p = position(i);
    if (p.qty != 0) submit(i, p.qty > 0 ? Side::Sell : Side::Buy, OrdType::Market, std::llabs(p.qty), 0, "RISK", now, true);
  }
  // Re-quotes the synthetic market makers around the current price. May fill the user's resting orders.
  void quote(std::size_t i) {
    Inst& in = inst_[i];
    const OrderId b0 = kMaker + i * 16;
    for (OrderId k = 0; k < 10; ++k) in.book.cancel(b0 + k);
    auto cb = [&](const Trade& t) { on_trade(t); };
    for (int k = 0; k < 5; ++k) {
      const Qty qa = 1 + static_cast<Qty>(rng_.next() % 40), qb = 1 + static_cast<Qty>(rng_.next() % 40);
      in.book.add_limit(b0 + static_cast<OrderId>(2 * k), Side::Sell, in.px + in.tick * (k + 1), qa, cb);
      const Price bp = in.px - in.tick * (k + 1);
      if (bp > 0) in.book.add_limit(b0 + static_cast<OrderId>(2 * k + 1), Side::Buy, bp, qb, cb);
    }
  }
  void on_trade(const Trade& t) {
    if (t.maker < kMaker) apply_fill(t.maker, t.px, t.qty);
    if (t.taker < kMaker && t.taker != 0) apply_fill(t.taker, t.px, t.qty);
  }
  void apply_fill(OrderId id, Price px, Qty q) {
    Rec& r = orders_[id - 1];
    r.o.filled += q; r.fill_val += rupees(px) * static_cast<double>(q);
    adv(r.o, r.o.filled == r.o.qty ? Ev::FullFill : Ev::PartialFill);
    if (r.o.st == State::Filled) --live_;
    const double notional = rupees(px) * static_cast<double>(q), fee = notional * fee_bps_ / 1e4;
    Fill f; f.id = id; f.inst = r.o.inst; f.side = r.o.side; f.px = px; f.qty = q;
    pf_.apply(f, fee);
    fees_ += fee; turn_ += notional;
    log("FILL", "SIM " + tag(id) + " " + to_string(r.o.side) + " " + std::to_string(q) + " " + inst_[r.o.inst].sym + " @ " + money(px));
  }

  double capital_;
  RiskLimits lim_;
  double fee_bps_;
  Rng rng_;
  Portfolio pf_;
  RiskEngine risk_;
  std::vector<Inst> inst_;
  std::vector<Price> marks_;
  std::vector<Rec> orders_;
  std::vector<Evt> events_;
  std::vector<Led> ledger_;
  std::vector<double> curve_;
  std::vector<Ts> stamps_;
  double dep_ = 0, fees_ = 0, turn_ = 0;
  int live_ = 0;
  long ticks_ = 0;
  std::int64_t wall_ = 0, day_ = 0;
  bool stub_ = false;
};

}  // namespace mc
