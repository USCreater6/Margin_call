// Live simulation session behind the terminal. SIMULATION ONLY.
//
// Market: a replay of one real NSE trading day. Each instrument's path starts at that day's official
// open, reaches its official high and low, and ends at its official close (NSE daily files). The order of
// high and low inside the day and every move between the four anchors are SIMULATED: the daily files carry
// no intraday path. Session = 09:15-15:30 IST = 22,500 market seconds, replayed at a chosen speed.
// Book depth: synthetic market makers re-quoted every tick; per level about 5 seconds of that day's average
// traded quantity (when the day's volume unit is verified), else a small fixed fallback.
//
// Orders: state machine -> risk engine -> matching engine. Every BUY order that fills opens its own position
// lot (own ID, entry price, nanosecond timestamp) that can be closed on its own. Every exchange action
// (order, cancel, exit) uses one token bucket: never more than `rate_per_sec` actions in any second.
// Exits (close lot, flatten, kill, 2% stop) and cancels wait in a queue for a token instead of being dropped.
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
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
  static constexpr int kSession = 22500;   // 09:15 to 15:30 IST, in seconds
  static constexpr int kRes = 4;           // replay path points per market second

  struct Rec { Order o; std::string src, why, code; double fill_val = 0; std::int64_t wall = 0, ns = 0; double mt = -1;
    OrderId lot = 0;   // for an exit: the position lot it closes
    double avg_fill() const { return o.filled > 0 ? fill_val / static_cast<double>(o.filled) : 0.0; } };
  struct Evt { std::int64_t wall; std::string k, m; };
  struct Led { std::int64_t wall; std::string k; double a, b; };
  struct Lot { OrderId id = 0; std::uint32_t inst = 0; Qty qty = 0; double cost = 0, entry = 0, realized = 0, mt = -1;
    std::int64_t ns = 0; bool closing = false; std::string closer; };
  // One real trading day (rupee prices in paise). vol_units = traded quantity in units; 0 = not verified.
  struct Day { std::string date; Price o = 0, h = 0, l = 0, c = 0, pc = 0; double vol_units = 0; std::string vol_note; };

  LiveSim(double capital, const RiskLimits& lim, std::uint64_t seed, double fee_bps = 3.0)
      : capital_(capital), lim_(lim), fee_bps_(fee_bps), rng_(seed), pf_(capital, 1), risk_(lim) {
    risk_.start_day(capital);
    ledger_.push_back({0, "OPENING DEMO BALANCE", capital, capital});
  }

  void add_instrument(const std::string& sym, const std::string& venue, Price px, Price tick,
                      const std::string& source = "placeholder", Qty lot = 1) {
    Inst in; in.sym = sym; in.venue = venue; in.px = in.start = in.prev = px; in.tick = tick; in.source = source; in.lot = lot > 0 ? lot : 1;
    in.depth_note = "no volume data: 1-40 lots per level";
    inst_.push_back(std::move(in));
    marks_.push_back(px);
    quote(inst_.size() - 1);
  }
  // Attaches a real trading day to instrument i: builds its replay path and moves the price to the day's open.
  bool set_day(std::size_t i, const Day& d) {
    if (i >= inst_.size() || d.o <= 0 || d.h <= 0 || d.l <= 0 || d.c <= 0 || d.l > d.h) return false;
    Inst& in = inst_[i];
    in.day = d; in.has_day = true; in.prev = d.pc > 0 ? d.pc : d.o;
    build_path(in);
    if (d.vol_units > 0) {
      const double per_level = d.vol_units / kSession * 5.0;
      in.level_qty = std::max<Qty>(in.lot, static_cast<Qty>(per_level / static_cast<double>(in.lot)) * in.lot);
      in.depth_note = "each level about 5 s of " + d.date + " average traded quantity";
    } else {
      in.level_qty = 0;
      in.depth_note = "volume unit not verified: small fixed depth";
    }
    if (replay_date_.empty()) replay_date_ = d.date;
    has_replay_ = true;
    in.px = in.start = in.path[0];
    marks_[i] = in.px;
    quote(i);
    return true;
  }

  std::size_t instruments() const { return inst_.size(); }
  int find_symbol(const std::string& s) const {
    for (std::size_t i = 0; i < inst_.size(); ++i) if (inst_[i].sym == s) return static_cast<int>(i);
    return -1;
  }
  // Wall clock. When the India (IST) calendar day changes, a new risk day starts:
  // the daily loss halt and profit lockout are cleared only here, never by re-arming or restarting the replay.
  void set_wall(std::int64_t ms) { set_wall_ns(ms * 1000000LL); }
  void set_wall_ns(std::int64_t ns) {
    wall_ns_ = ns; wall_ = ns / 1000000LL;
    if (ledger_.size() == 1 && ledger_[0].wall == 0) ledger_[0].wall = wall_;
    const std::int64_t day = (wall_ + 19800000LL) / 86400000LL;   // IST = UTC+05:30
    if (day_ != 0 && day != day_) {
      risk_.start_day(equity());
      log("NEW DAY", "New trading day: daily loss and profit limits measured from current equity");
    }
    day_ = day;
  }
  void set_price(std::size_t i, Price px) { inst_[i].px = px; marks_[i] = px; }   // test hook

  // Replay speed: 0 = paused, 1 = real time (6h15m), 6.25 = 60 minutes, 25 = 15 minutes.
  bool set_speed(double s) {
    static const double ok[] = {0, 1, 2, 6.25, 12.5, 25};
    for (double x : ok) if (std::fabs(x - s) < 1e-9) { speed_ = x; log("REPLAY", "Speed " + speed_text()); return true; }
    return false;
  }
  // Starts the day again from 09:15. Only when flat, so no position jumps from the close back to the open.
  std::string restart_replay() {
    if (!has_replay_) return "No trading-day data loaded";
    if (pf_.open_positions() > 0 || live_ > 0 || !q_.empty() || !later_.empty()) return "Close all positions and orders first (Flatten all)";
    mt_ = 0; closed_ = false; last_step_ = 0;
    for (std::size_t i = 0; i < inst_.size(); ++i) if (inst_[i].has_day) { inst_[i].px = inst_[i].path[0]; marks_[i] = inst_[i].px; quote(i); }
    log("REPLAY", "Replay of " + replay_date_ + " restarted at 09:15. Daily loss and profit limits are not reset by a restart.");
    return "";
  }

  double equity() const { return pf_.equity(marks_); }
  double base() const { return capital_ + dep_; }
  double cash() const { return pf_.cash(); }
  Position position(std::size_t i) const { return pf_.pos(0, static_cast<std::uint32_t>(i)); }
  int live_orders() const { return live_; }
  bool killed() const { return risk_.killed(); }
  bool halted() const { return risk_.halted(); }
  bool locked() const { return risk_.locked(); }
  bool stub() const { return stub_; }
  bool market_closed() const { return closed_; }
  double market_time() const { return mt_; }
  std::size_t queued() const { return q_.size() + later_.size(); }
  const std::vector<Rec>& orders() const { return orders_; }
  const std::vector<Evt>& events() const { return events_; }
  const std::vector<Lot>& lots() const { return lots_; }
  double realized() const { return real_; }
  Price price(std::size_t i) const { return inst_[i].px; }
  Price best_ask(std::size_t i) const { return inst_[i].book.best_ask(); }
  Price best_bid(std::size_t i) const { return inst_[i].book.best_bid(); }
  std::size_t actions_last_second(Ts now) { prune(now); return stamps_.size(); }

  // A new order from the ticket or the stub. Returns an empty string if accepted, otherwise the veto reason.
  std::string submit(std::size_t i, Side side, OrdType type, Qty qty, Price px, const std::string& src, Ts now) {
    Inst& in = inst_[i];
    Rec r;
    r.o.id = orders_.size() + 1; r.o.inst = static_cast<std::uint32_t>(i); r.o.side = side; r.o.type = type;
    r.o.px = type == OrdType::Limit ? px : 0; r.o.qty = qty; r.src = src; r.wall = wall_; r.ns = wall_ns_; r.mt = has_replay_ ? mt_ : -1;
    adv(r.o, Ev::Submit);
    std::string why;
    if (type == OrdType::Stop) why = "Stop orders are not available in the terminal yet";
    else if (qty <= 0 || qty % in.lot != 0) { why = "Quantity must be whole lots of " + std::to_string(in.lot); r.code = "LOT_SIZE"; }
    else if (type == OrdType::Limit && (px <= 0 || px % in.tick != 0)) why = "Limit price must be a positive multiple of the tick size";
    else if (risk_.killed()) { why = human(Reject::Killed); r.code = to_string(Reject::Killed); }
    else if (closed_) { why = "Market closed: the replay of " + replay_date_ + " ended at 15:30. Restart the replay."; r.code = "MARKET_CLOSED"; }
    else if (!q_.empty() || !later_.empty()) { why = "Exits and cancels are being sent (8 per second). Try again in a moment."; r.code = "QUEUE_BUSY"; }
    else if (self_cross(i, side, type, px)) { why = "Self-trade prevention: it would match your own resting order"; r.code = "SELF_TRADE"; }
    else {
      RiskCtx c;
      c.equity = equity(); c.gross = pf_.gross(marks_); c.open_orders = live_; c.positions = pf_.open_positions();
      c.pos_qty = position(i).qty;
      // a market buy is checked at the best ask it will actually pay, not the last price
      c.ref_px = rupees(type == OrdType::Limit ? px : (side == Side::Buy && in.book.best_ask() > 0 ? in.book.best_ask() : in.px));
      if (type == OrdType::Market && side == Side::Buy) {   // what the order will really pay: walk the asks, add the fee
        double cost = 0; Qty left = qty;
        in.book.levels(Side::Sell, 1 << 20, [&](Price p, Qty q) { if (left > 0) { const Qty a = std::min(left, q); cost += rupees(p) * static_cast<double>(a); left -= a; } });
        if (qty > left) c.ref_px = std::max(c.ref_px, cost / static_cast<double>(qty - left));
      }
      if (side == Side::Buy) c.ref_px *= 1.0 + fee_bps_ / 1e4;
      c.cash = pf_.cash();
      c.long_only = true;
      for (const Rec& q : orders_) {
        if (!is_live(q.o.st)) continue;
        const Qty rem = q.o.qty - q.o.filled;
        if (q.o.side == Side::Sell && q.o.inst == i) c.pending_sell_qty += rem;
        if (q.o.side == Side::Buy) c.pending_buy_notional += rupees(q.o.px) * static_cast<double>(rem) * (1.0 + fee_bps_ / 1e4);
      }
      for (const Lot& l : lots_) if (l.closing && l.inst == i) c.pending_sell_qty += l.qty;   // queued exits are sells too
      const Reject rj = risk_.check(r.o, c, now);
      why = human(rj);
      if (rj != Reject::None) r.code = to_string(rj);
    }
    if (r.code.empty() && !why.empty()) r.code = "BAD_ORDER";
    if (!why.empty()) {
      adv(r.o, Ev::RiskReject); r.why = why;
      orders_.push_back(r);
      log("RISK VETO", tag(r.o.id) + " (" + src + ") " + why);
      return why;
    }
    stamps_.push_back(now);
    send(r, now);
    return "";
  }

  // Cancels are queued and sent when a rate token is free (usually at once).
  bool cancel(OrderId id, Ts now) {
    if (!queue_cancel(id, "MANUAL")) return false;
    pump(now);
    return true;
  }
  int cancel_all(Ts now) {
    int n = 0;
    for (std::size_t k = 0; k < orders_.size(); ++k) n += queue_cancel(k + 1, "MANUAL");
    log("CANCEL ALL", std::to_string(n) + " open simulated order(s) queued for cancel");
    pump(now);
    return n;
  }
  // Closes the given position lots at market (each is its own exit order). Returns how many were queued.
  int close_lots(const std::vector<OrderId>& ids, const std::string& src, Ts now) {
    int n = 0;
    for (OrderId id : ids) n += queue_close(id, src);
    if (n) log("CLOSE", std::to_string(n) + " position lot(s) queued to close at market");
    pump(now);
    return n;
  }
  void flatten(Ts now) {
    int c = 0, p = 0;
    for (std::size_t k = 0; k < orders_.size(); ++k) c += queue_cancel(k + 1, "FLATTEN");
    for (const Lot& l : lots_) if (l.qty > 0) p += queue_close(l.id, "FLATTEN");
    stub_ = false;
    flatten_pending_ = true;
    log("FLATTEN", "Requested: " + std::to_string(c) + " order cancel(s) and " + std::to_string(p) + " position close(s) queued at " +
                       std::to_string(lim_.rate_per_sec) + " per second. Stub signals stopped.");
    pump(now);
  }
  // The kill switch blocks every new order at once, then cancels all open orders and closes every position.
  void kill(Ts now) {
    risk_.kill(); stub_ = false;
    int c = 0, p = 0;
    for (std::size_t k = 0; k < orders_.size(); ++k) c += queue_cancel(k + 1, "KILL");
    for (const Lot& l : lots_) if (l.qty > 0) p += queue_close(l.id, "KILL");
    kill_pending_ = true;
    log("KILL SWITCH", "Activated: all new orders blocked; " + std::to_string(c) + " cancel(s) and " + std::to_string(p) +
                           " position close(s) queued at " + std::to_string(lim_.rate_per_sec) + " per second");
    pump(now);
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
    orders_.clear(); events_.clear(); ledger_.clear(); curve_.clear(); stamps_.clear(); lots_.clear(); q_.clear(); later_.clear();
    pf_ = Portfolio(capital_, 1); risk_ = RiskEngine(lim_); risk_.start_day(capital_);
    dep_ = fees_ = turn_ = real_ = 0; live_ = 0; stub_ = false; kill_pending_ = flatten_pending_ = false;
    mt_ = 0; closed_ = false; last_step_ = 0;
    for (std::size_t i = 0; i < inst_.size(); ++i) { inst_[i].px = inst_[i].has_day ? inst_[i].path[0] : inst_[i].start; marks_[i] = inst_[i].px; }
    ledger_.push_back({wall_, "OPENING DEMO BALANCE", capital_, capital_});
    for (std::size_t i = 0; i < inst_.size(); ++i) quote(i);
    log("RESET", "Demo account reset" + std::string(has_replay_ ? "; replay restarted at 09:15" : ""));
  }
  void set_stub(bool on) { stub_ = on && !risk_.killed(); log("AI STUB", stub_ ? "Stub signals started" : "Stub signals stopped"); }

  // Sends queued cancels and exits while rate tokens are available. Call often (the server calls it every loop).
  void pump(Ts now) {
    while (!q_.empty()) {
      const Act a = q_.front();
      if (!still_valid(a)) { q_.pop_front(); continue; }   // nothing to send: costs no token
      if (!risk_.take_rate_token(now)) break;
      q_.pop_front();
      stamps_.push_back(now);
      if (a.close) exec_close(a, now); else do_cancel(a.id, a.src);
    }
    if (q_.empty() && later_.empty()) finish_pending(now);
  }

  // One market tick: advances the replay clock by speed x elapsed seconds (move=false keeps prices: test hook).
  void step(Ts now, bool move = true) {
    ++ticks_;
    const double dt = last_step_ ? static_cast<double>(now - last_step_) * 1e-9 : 1.0;
    last_step_ = now;
    if (move && has_replay_ && !closed_ && speed_ > 0) {
      mt_ = std::min<double>(kSession, mt_ + dt * speed_);
      for (auto& in : inst_) if (in.has_day) in.px = path_at(in, mt_);
      if (mt_ >= kSession) {
        closed_ = true; stub_ = false;
        log("SESSION CLOSE", "Replay of " + replay_date_ + " reached 15:30: prices hold at the official close. New orders are closed; exits still work.");
      }
    }
    for (std::size_t i = 0; i < inst_.size(); ++i) { marks_[i] = inst_[i].px; quote(i); }
    for (Act& a : later_) q_.push_back(a);   // exits that waited for fresh depth
    later_.clear();
    for (const Lot& l : lots_) {              // 2% per-trade stop, per position lot
      if (l.qty <= 0 || l.closing) continue;
      if (rupees(inst_[l.inst].px) <= l.entry * (1.0 - lim_.stop_pct)) {
        log("STOP", "2% per-trade stop hit on " + inst_[l.inst].sym + " lot " + tag(l.id) + " (entry " + fmt(l.entry) + ")");
        queue_close(l.id, "STOP");
      }
    }
    const bool was_h = risk_.halted(), was_l = risk_.locked();
    risk_.on_equity(equity());
    if (!was_h && risk_.halted()) {
      for (std::size_t k = 0; k < orders_.size(); ++k) queue_cancel(k + 1, "HALT");
      log("HALT", "Global daily loss limit reached. New risk blocked; open orders cancelled.");
    }
    if (!was_l && risk_.locked()) log("LOCKOUT", "Daily profit target reached. New risk blocked.");
    pump(now);
    if (stub_ && !closed_ && now >= next_stub_ && !inst_.empty()) {
      next_stub_ = now + 4000000000LL;
      const std::size_t i = rng_.next() % inst_.size();
      const Side s = (position(i).qty > 0 && (rng_.next() & 1)) ? Side::Sell : Side::Buy;   // long-only
      log("AI STUB", std::string("Random stub signal: ") + to_string(s) + " " + std::to_string(inst_[i].lot) + " " + inst_[i].sym);
      submit(i, s, OrdType::Market, inst_[i].lot, 0, "AI-STUB", now);
    }
    if (now >= next_curve_) {   // one point per second
      next_curve_ = now + 1000000000LL;
      curve_.push_back(equity() - base());
      if (curve_.size() > 900) curve_.erase(curve_.begin());
    }
    prune(now);
  }

  // ---- persistence: the whole account survives a restart (Codespace stop / start) ----
  // Tab-separated text. Strings never contain tabs or newlines (they are replaced by spaces).
  std::string snapshot() const {
    std::ostringstream o;
    o.precision(17);
    auto cl = [](const std::string& x) { std::string r = x; for (char& c : r) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return r; };
    o << "MCSNAP\t1\n";
    o << "A\t" << capital_ << "\t" << dep_ << "\t" << fees_ << "\t" << turn_ << "\t" << real_ << "\t" << pf_.cash() << "\t" << pf_.realized(0) << "\t" << pf_.fees(0) << "\n";
    o << "R\t" << risk_.killed() << "\t" << risk_.halted() << "\t" << risk_.locked() << "\t" << risk_.day_start() << "\t" << day_ << "\n";
    o << "C\t" << cl(replay_date_.empty() ? "-" : replay_date_) << "\t" << mt_ << "\t" << speed_ << "\t" << closed_ << "\t" << kill_pending_ << "\t" << flatten_pending_ << "\n";
    for (const Inst& in : inst_) o << "I\t" << cl(in.sym) << "\t" << cl(in.venue) << "\t" << in.px << "\t" << in.tick << "\t" << in.lot << "\n";
    for (std::size_t i = 0; i < inst_.size(); ++i) { const Position p = position(i); if (p.qty) o << "P\t" << cl(inst_[i].sym) << "\t" << p.qty << "\t" << p.avg << "\n"; }
    for (const Lot& l : lots_)
      o << "L\t" << l.id << "\t" << cl(inst_[l.inst].sym) << "\t" << l.qty << "\t" << l.cost << "\t" << l.entry << "\t" << l.realized << "\t" << l.mt << "\t" << l.ns
        << "\t" << l.closing << "\t" << cl(l.closer.empty() ? "-" : l.closer) << "\n";
    for (const Rec& r : orders_)
      o << "O\t" << r.o.id << "\t" << cl(inst_[r.o.inst].sym) << "\t" << static_cast<int>(r.o.side) << "\t" << static_cast<int>(r.o.type) << "\t" << r.o.px << "\t" << r.o.qty
        << "\t" << r.o.filled << "\t" << static_cast<int>(r.o.st) << "\t" << cl(r.src.empty() ? "-" : r.src) << "\t" << cl(r.why.empty() ? "-" : r.why) << "\t" << cl(r.code.empty() ? "-" : r.code)
        << "\t" << r.fill_val << "\t" << r.wall << "\t" << r.ns << "\t" << r.mt << "\t" << r.lot << "\n";
    const std::size_t ef = events_.size() > 300 ? events_.size() - 300 : 0;
    for (std::size_t k = ef; k < events_.size(); ++k) o << "E\t" << events_[k].wall << "\t" << cl(events_[k].k) << "\t" << cl(events_[k].m.empty() ? "-" : events_[k].m) << "\n";
    for (const Led& g : ledger_) o << "G\t" << g.wall << "\t" << cl(g.k) << "\t" << g.a << "\t" << g.b << "\n";
    o << "V";
    for (double v : curve_) o << "\t" << v;
    o << "\nEND\n";
    return o.str();
  }
  // Restores a snapshot. Instruments are matched by symbol; one that is no longer in today's data comes back
  // at its saved price (labelled). If the replay day changed, the account is kept and the clock starts at 09:15.
  // Returns an empty string on success, otherwise why the account was not restored.
  std::string restore(const std::string& text, Ts now) {
    std::vector<std::vector<std::string>> rows;
    { std::istringstream in(text); std::string line;
      while (std::getline(in, line)) { std::vector<std::string> f; std::istringstream ls(line); std::string x; while (std::getline(ls, x, '\t')) f.push_back(x); if (!f.empty()) rows.push_back(f); } }
    if (rows.empty() || rows[0].size() < 2 || rows[0][0] != "MCSNAP" || rows[0][1] != "1") return "not a snapshot file";
    if (rows.back()[0] != "END") return "snapshot incomplete (no END line)";
    try {
      auto D = [](const std::string& x) { return std::stod(x); };
      auto L = [](const std::string& x) { return static_cast<long long>(std::stoll(x)); };
      auto S = [](const std::string& x) { return x == "-" ? std::string() : x; };
      // instruments first (missing ones are re-created at their saved price)
      for (const auto& f : rows) if (f[0] == "I" && f.size() >= 6 && find_symbol(f[1]) < 0) {
        add_instrument(f[1], f[2], L(f[3]), L(f[4]), "saved price (not in today's data)", L(f[5]));
      }
      auto idx = [&](const std::string& sym) -> std::uint32_t {
        const int i = find_symbol(sym); if (i < 0) throw std::runtime_error("unknown symbol " + sym); return static_cast<std::uint32_t>(i); };
      double cash = 0, pfr = 0, pff = 0; std::string date; double mt = 0, spd = 1; bool cls = false, kp = false, fp = false;
      bool rk = false, rh = false, rl = false; double rds = 0; std::int64_t dday = 0;
      std::vector<std::pair<std::uint32_t, Position>> pos;
      std::vector<Lot> lots; std::vector<Rec> ords; std::vector<Evt> evs; std::vector<Led> led; std::vector<double> crv;
      double cap = capital_, dep = 0, fees = 0, turn = 0, real = 0;
      for (const auto& f : rows) {
        const std::string& t = f[0];
        if (t == "A" && f.size() >= 9) { cap = D(f[1]); dep = D(f[2]); fees = D(f[3]); turn = D(f[4]); real = D(f[5]); cash = D(f[6]); pfr = D(f[7]); pff = D(f[8]); }
        else if (t == "R" && f.size() >= 6) { rk = f[1] == "1"; rh = f[2] == "1"; rl = f[3] == "1"; rds = D(f[4]); dday = L(f[5]); }
        else if (t == "C" && f.size() >= 7) { date = S(f[1]); mt = D(f[2]); spd = D(f[3]); cls = f[4] == "1"; kp = f[5] == "1"; fp = f[6] == "1"; }
        else if (t == "P" && f.size() >= 4) { Position p; p.qty = L(f[2]); p.avg = D(f[3]); pos.push_back({idx(f[1]), p}); }
        else if (t == "L" && f.size() >= 11) {
          Lot l; l.id = static_cast<OrderId>(L(f[1])); l.inst = idx(f[2]); l.qty = L(f[3]); l.cost = D(f[4]); l.entry = D(f[5]); l.realized = D(f[6]);
          l.mt = D(f[7]); l.ns = L(f[8]); l.closing = f[9] == "1"; l.closer = S(f[10]); lots.push_back(l);
        } else if (t == "O" && f.size() >= 17) {
          Rec r; r.o.id = static_cast<OrderId>(L(f[1])); r.o.inst = idx(f[2]); r.o.side = static_cast<Side>(L(f[3])); r.o.type = static_cast<OrdType>(L(f[4]));
          r.o.px = L(f[5]); r.o.qty = L(f[6]); r.o.filled = L(f[7]); r.o.st = static_cast<State>(L(f[8])); r.src = S(f[9]); r.why = S(f[10]); r.code = S(f[11]);
          r.fill_val = D(f[12]); r.wall = L(f[13]); r.ns = L(f[14]); r.mt = D(f[15]); r.lot = static_cast<OrderId>(L(f[16]));
          if (r.o.id != ords.size() + 1) throw std::runtime_error("order ids out of sequence");
          ords.push_back(r);
        } else if (t == "E" && f.size() >= 4) evs.push_back({L(f[1]), f[2], S(f[3])});
        else if (t == "G" && f.size() >= 5) led.push_back({L(f[1]), f[2], D(f[3]), D(f[4])});
        else if (t == "V") for (std::size_t k = 1; k < f.size(); ++k) crv.push_back(D(f[k]));
      }
      for (const Lot& l : lots) if (l.id == 0 || l.id > ords.size()) throw std::runtime_error("lot without its order");
      // all parsed: apply
      for (auto& in : inst_) in.book.clear();
      capital_ = cap; dep_ = dep; fees_ = fees; turn_ = turn; real_ = real;
      pf_.restore(cash, pfr, pff, pos);
      risk_ = RiskEngine(lim_); risk_.restore(rk, rh, rl, rds);
      if (dday) day_ = dday;
      orders_ = ords; lots_ = lots; events_ = evs; ledger_ = led; curve_ = crv;
      q_.clear(); later_.clear(); stamps_.clear(); stub_ = false; last_step_ = 0;
      kill_pending_ = kp; flatten_pending_ = fp;
      const bool same_day = has_replay_ && date == replay_date_;
      if (same_day) { mt_ = std::min<double>(mt, kSession); closed_ = cls; } else { mt_ = 0; closed_ = false; }
      if (spd == 0 || spd == 1 || spd == 2 || spd == 6.25 || spd == 12.5 || spd == 25) speed_ = spd;
      for (std::size_t i = 0; i < inst_.size(); ++i) { if (inst_[i].has_day) inst_[i].px = path_at(inst_[i], mt_); marks_[i] = inst_[i].px; quote(i); }
      live_ = 0;
      auto cb = [&](const Trade& tr) { on_trade(tr); };
      for (const Rec& r : orders_) if (is_live(r.o.st)) {
        ++live_;
        if (r.o.type == OrdType::Limit) inst_[r.o.inst].book.add_limit(r.o.id, r.o.side, r.o.px, r.o.qty - r.o.filled, cb);
      }
      for (Lot& l : lots_) if (l.qty > 0 && l.closing) { l.closing = false; queue_close(l.id, l.closer.empty() ? "RESTORE" : l.closer); }
      log("RESTORED", "Account restored after a restart: cash, positions, orders and ledger." +
                          std::string(same_day ? " Replay clock continues." : has_replay_ ? " New replay day " + replay_date_ + ": clock starts at 09:15." : ""));
      pump(now);
      return "";
    } catch (const std::exception& e) {
      return std::string("snapshot unreadable: ") + e.what();
    }
  }

  std::string state_json(std::size_t sel, Ts now, bool lite = false) {
    prune(now);
    if (sel >= inst_.size()) sel = 0;
    std::ostringstream o;
    o.precision(12);
    o << "{\"engine\":\"cpp\",\"label\":\"SIMULATION\",\"start\":" << capital_ << ",\"cash\":" << pf_.cash() << ",\"dep\":" << dep_
      << ",\"real\":" << real_ << ",\"fees\":" << fees_ << ",\"turn\":" << turn_ << ",\"killed\":" << b(risk_.killed())
      << ",\"halt\":" << b(risk_.halted()) << ",\"lock\":" << b(risk_.locked()) << ",\"stub\":" << b(stub_)
      << ",\"rate\":" << stamps_.size() << ",\"rate_max\":" << lim_.rate_per_sec << ",\"queued\":" << queued()
      << ",\"notional_rule\":" << lim_.max_order_notional << ",\"sel\":" << sel
      << ",\"replay\":{\"has\":" << b(has_replay_) << ",\"date\":\"" << esc(replay_date_) << "\",\"mt\":" << mt_ << ",\"len\":" << kSession
      << ",\"speed\":" << speed_ << ",\"closed\":" << b(closed_) << "},\"inst\":[";
    for (std::size_t i = 0; i < inst_.size(); ++i) {
      const Inst& in = inst_[i];
      o << (i ? "," : "") << "{\"s\":\"" << esc(in.sym) << "\",\"v\":\"" << esc(in.venue) << "\",\"p\":" << rupees(in.px)
        << ",\"pc\":" << rupees(in.prev) << ",\"t\":" << rupees(in.tick) << ",\"lot\":" << in.lot << ",\"src\":\"" << esc(in.source) << "\"";
      if (in.has_day)
        o << ",\"day\":{\"date\":\"" << esc(in.day.date) << "\",\"o\":" << rupees(in.day.o) << ",\"h\":" << rupees(in.day.h) << ",\"l\":" << rupees(in.day.l)
          << ",\"c\":" << rupees(in.day.c) << ",\"vol\":" << in.day.vol_units << ",\"vnote\":\"" << esc(in.day.vol_note) << "\"}";
      o << ",\"depth\":\"" << esc(in.depth_note) << "\"}";
    }
    o << "],\"book\":{\"asks\":[";
    if (!inst_.empty()) {
      bool f = true;
      inst_[sel].book.levels(Side::Sell, 5, [&](Price p, Qty q) { o << (f ? "" : ",") << "[" << rupees(p) << "," << q << "]"; f = false; });
      o << "],\"bids\":[";
      f = true;
      inst_[sel].book.levels(Side::Buy, 5, [&](Price p, Qty q) { o << (f ? "" : ",") << "[" << rupees(p) << "," << q << "]"; f = false; });
    } else o << "],\"bids\":[";
    o << "]},\"pos\":[";
    bool first = true;
    for (std::size_t i = 0; i < inst_.size(); ++i) {
      const Position p = position(i);
      if (p.qty == 0) continue;
      o << (first ? "" : ",") << "{\"s\":\"" << esc(inst_[i].sym) << "\",\"q\":" << p.qty << ",\"avg\":" << p.avg << "}";
      first = false;
    }
    o << "],\"lots\":[";
    first = true;
    for (const Lot& l : lots_) {
      if (l.qty <= 0) continue;
      o << (first ? "" : ",") << "{\"id\":\"" << tag(l.id) << "\",\"s\":\"" << esc(inst_[l.inst].sym) << "\",\"q\":" << l.qty << ",\"e\":" << l.entry
        << ",\"ns\":\"" << l.ns << "\",\"mt\":" << l.mt << ",\"r\":" << l.realized << ",\"c\":" << b(l.closing) << ",\"by\":\"" << esc(l.closer) << "\"}";
      first = false;
    }
    o << "],\"orders\":[";
    const std::size_t keep = lite ? 40 : 200;
    const std::size_t from = orders_.size() > keep ? orders_.size() - keep : 0;
    for (std::size_t k = orders_.size(); k-- > from;) {
      const Rec& r = orders_[k];
      o << (k + 1 == orders_.size() ? "" : ",") << "{\"id\":\"" << tag(r.o.id) << "\",\"w\":" << r.wall << ",\"ns\":\"" << r.ns << "\",\"mt\":" << r.mt
        << ",\"src\":\"" << esc(r.src) << "\",\"side\":\"" << to_string(r.o.side) << "\",\"q\":" << r.o.qty << ",\"f\":" << r.o.filled
        << ",\"s\":\"" << esc(inst_[r.o.inst].sym) << "\",\"type\":\"" << to_string(r.o.type) << "\",\"px\":" << rupees(r.o.px) << ",\"fp\":" << r.avg_fill()
        << ",\"lot\":\"" << (r.lot ? tag(r.lot) : "") << "\",\"code\":\"" << esc(r.code) << "\",\"st\":\"" << to_string(r.o.st) << "\",\"why\":\"" << esc(r.why) << "\"}";
    }
    o << "],\"lite\":" << b(lite) << ",\"audit\":[";
    const std::size_t ef = events_.size() > (lite ? 25u : 150u) ? events_.size() - (lite ? 25u : 150u) : 0;
    for (std::size_t k = events_.size(); k-- > ef;)
      o << (k + 1 == events_.size() ? "" : ",") << "{\"w\":" << events_[k].wall << ",\"k\":\"" << esc(events_[k].k) << "\",\"m\":\"" << esc(events_[k].m) << "\"}";
    o << "],\"ledger\":[";
    if (!lite) for (std::size_t k = ledger_.size(); k-- > 0;)
      o << (k + 1 == ledger_.size() ? "" : ",") << "{\"w\":" << ledger_[k].wall << ",\"k\":\"" << ledger_[k].k << "\",\"a\":" << ledger_[k].a << ",\"b\":" << ledger_[k].b << "}";
    o << "],\"curve\":[";
    if (!lite) for (std::size_t k = 0; k < curve_.size(); ++k) o << (k ? "," : "") << curve_[k];
    o << "]}";
    return o.str();
  }

 private:
  struct Inst {
    std::string sym, venue; Price px = 0, start = 0, prev = 0, tick = 1; OrderBook book{256}; std::string source; Qty lot = 1;
    bool has_day = false; Day day; std::vector<Price> path; Qty level_qty = 0; std::string depth_note;
  };
  struct Act { bool close = false; OrderId id = 0; std::string src; };
  static constexpr OrderId kMaker = 1ULL << 62;
  static constexpr int kLevels = 10;

  static const char* b(bool x) { return x ? "true" : "false"; }
  static std::string tag(OrderId id) { std::string s = std::to_string(id); return "SIM-" + std::string(s.size() < 4 ? 4 - s.size() : 0, '0') + s; }
  static std::string fmt(double r) { std::ostringstream o; o.setf(std::ios::fixed); o.precision(2); o << r; return o.str(); }
  static std::string money(Price p) { return fmt(rupees(p)); }
  static std::string esc(const std::string& s) {
    std::string r;
    for (char c : s) { if (c == '"' || c == '\\') r += '\\'; if (static_cast<unsigned char>(c) >= 32) r += c; }
    return r;
  }
  std::string speed_text() const {
    if (speed_ == 0) return "paused";
    const double mins = kSession / 60.0 / speed_;
    return fmt(speed_) + "x (session in " + std::to_string(static_cast<int>(std::lround(mins))) + " min)";
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
      case Reject::RateLimit: return "Rate cap reached: " + std::to_string(lim_.rate_per_sec) + " actions per second";
      case Reject::Halted: return "Daily loss limit hit: no new risk";
      case Reject::ProfitLock: return "Daily profit lockout: no new risk";
      case Reject::StratHalted: return "Strategy daily loss limit hit";
      case Reject::Notional: return "Order value exceeds " + pc(lim_.max_order_notional) + " of equity";
      case Reject::OpenOrders: return "Maximum open orders reached (" + std::to_string(lim_.max_open_orders) + ")";
      case Reject::Positions: return "Maximum open positions reached (" + std::to_string(lim_.max_positions) + " instruments)";
      case Reject::Gross: return "Gross exposure would exceed " + pc(lim_.max_gross) + " of equity";
      case Reject::NoShort: return "Long-only: you can sell only what you hold, after open sells and queued exits";
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

  // Sends an accepted order to the book.
  void send(Rec& r0, Ts) {
    adv(r0.o, Ev::RiskPass); adv(r0.o, Ev::VenueAck);
    orders_.push_back(r0);
    ++live_;
    const OrderId id = r0.o.id;
    Inst& in = inst_[r0.o.inst];
    const Side side = r0.o.side; const Qty qty = r0.o.qty;
    auto cb = [&](const Trade& t) { on_trade(t); };
    if (r0.o.type == OrdType::Market) {
      const Qty unf = in.book.add_market(id, side, qty, cb);
      if (unf > 0) {
        Rec& R = orders_[id - 1]; adv(R.o, Ev::CancelReq); adv(R.o, Ev::CancelAck); --live_;
        log("CANCEL", tag(id) + " " + std::to_string(qty - unf) + " of " + std::to_string(qty) + " filled; remainder cancelled: not enough simulated depth");
      }
    } else {
      in.book.add_limit(id, side, r0.o.px, qty, cb);
      log("ORDER", "SIM " + tag(id) + " " + to_string(side) + " " + std::to_string(qty) + " " + in.sym + " LIMIT " + money(r0.o.px));
    }
  }

  bool queue_cancel(OrderId id, const std::string& src) {
    if (id == 0 || id > orders_.size() || !is_live(orders_[id - 1].o.st)) return false;
    for (const Act& a : q_) if (!a.close && a.id == id) return false;
    q_.push_back({false, id, src});
    return true;
  }
  Lot* find_lot(OrderId id) { for (auto& l : lots_) if (l.id == id) return &l; return nullptr; }
  bool queue_close(OrderId id, const std::string& src) {
    Lot* l = find_lot(id);
    if (!l || l->qty <= 0 || l->closing) return false;
    l->closing = true; l->closer = src;
    q_.push_back({true, id, src});
    return true;
  }
  bool still_valid(const Act& a) {
    if (!a.close) return a.id <= orders_.size() && is_live(orders_[a.id - 1].o.st);
    const Lot* l = find_lot(a.id);
    return l && l->qty > 0;
  }
  void do_cancel(OrderId id, const std::string& src) {
    Rec& r = orders_[id - 1];
    if (!is_live(r.o.st)) return;
    inst_[r.o.inst].book.cancel(id);
    adv(r.o, Ev::CancelReq); adv(r.o, Ev::CancelAck);
    --live_;
    log("CANCEL", tag(id) + " cancelled" + (src == "MANUAL" ? "" : " (" + src + ")"));
  }
  // Bid quantity strictly above the user's own highest resting buy: an exit up to this size cannot trade with itself.
  Qty exit_room(std::size_t i) {
    Price own = 0;
    for (const Rec& r : orders_) if (is_live(r.o.st) && r.o.inst == i && r.o.side == Side::Buy) own = std::max(own, r.o.px);
    Qty room = 0;
    inst_[i].book.levels(Side::Buy, 64, [&](Price p, Qty q) { if (p > own) room += q; });
    return room;
  }
  void exec_close(const Act& a, Ts now) {
    Lot* l = find_lot(a.id);
    if (!l || l->qty <= 0) return;
    const std::size_t i = l->inst;
    Qty q = std::min(l->qty, exit_room(i));
    q = (q / inst_[i].lot) * inst_[i].lot;
    if (q <= 0) {
      OrderId own = 0;
      for (const Rec& r : orders_) if (is_live(r.o.st) && r.o.inst == i && r.o.side == Side::Buy && (!own || r.o.px > orders_[own - 1].o.px)) own = r.o.id;
      if (own && inst_[i].book.best_bid() <= orders_[own - 1].o.px) {   // our own buy is the best bid: selling would trade with ourselves
        l->closing = false; l->closer.clear();
        log("EXIT BLOCKED", "Lot " + tag(a.id) + " not closed: your own buy " + tag(own) + " is the best bid (self-trade prevention). Cancel it, then close again.");
        return;
      }
      later_.push_back(a); return;                  // no bid depth now: retry after the next re-quote
    }
    Rec r;
    r.o.id = orders_.size() + 1; r.o.inst = static_cast<std::uint32_t>(i); r.o.side = Side::Sell; r.o.type = OrdType::Market;
    r.o.qty = q; r.src = a.src; r.wall = wall_; r.ns = wall_ns_; r.mt = has_replay_ ? mt_ : -1; r.lot = a.id;
    adv(r.o, Ev::Submit);
    log("EXIT", tag(r.o.id) + " SELL " + std::to_string(q) + " " + inst_[i].sym + " closes lot " + tag(a.id) + " (" + a.src + ")");
    send(r, now);
    l = find_lot(a.id);
    if (l && l->qty > 0) later_.push_back(a);      // partly closed: the rest goes after the next re-quote
  }
  void finish_pending(Ts now) {
    if (kill_pending_) {
      int more = 0;   // a resting order may have filled before its cancel was sent: chase until flat
      for (std::size_t k = 0; k < orders_.size(); ++k) more += queue_cancel(k + 1, "KILL");
      for (const Lot& l : lots_) if (l.qty > 0) more += queue_close(l.id, "KILL");
      if (more) { pump(now); return; }
      bool open = live_ > 0;
      for (const Lot& l : lots_) open = open || l.qty > 0;
      if (!open) { kill_pending_ = false; flatten_pending_ = false; log("KILL SWITCH", "Complete: no open orders, no open positions"); }
    }
    if (flatten_pending_) {
      flatten_pending_ = false;
      int open = 0;
      for (const Lot& l : lots_) open += l.qty > 0;
      log("FLATTEN", open ? "Done: requested closes sent; " + std::to_string(open) + " lot(s) opened since" : "Complete: all positions closed");
    }
  }

  static Price path_at(const Inst& in, double mt) {
    const std::size_t k = std::min(in.path.size() - 1, static_cast<std::size_t>(mt * kRes));
    return in.path[k];
  }
  // Builds the replay path: open -> first extreme -> second extreme -> close, Brownian bridges in between,
  // REFLECTED (not clipped) at the day's high and low so the price never sits flat on a limit.
  // 4 points per market second. Deterministic per symbol and date (a restart replays the same path).
  void build_path(Inst& in) {
    const Day& d = in.day;
    std::uint64_t h = 1469598103934665603ULL;
    for (char ch : in.sym + "|" + d.date) { h ^= static_cast<unsigned char>(ch); h *= 1099511628211ULL; }
    Rng r(h);
    auto gauss = [&r]() {
      double u1 = r.uni(); if (u1 < 1e-12) u1 = 1e-12;
      return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * r.uni());
    };
    const double lo = static_cast<double>(d.l), hi = static_cast<double>(d.h);
    auto reflect = [lo, hi](double x) {
      if (hi <= lo) return lo;
      for (int g = 0; g < 64 && (x > hi || x < lo); ++g) x = x > hi ? 2 * hi - x : 2 * lo - x;
      return std::clamp(x, lo, hi);
    };
    const double op = std::clamp(static_cast<double>(d.o), lo, hi), cl = std::clamp(static_cast<double>(d.c), lo, hi);
    const bool up = cl >= op;
    const int N = kSession * kRes;
    const int ta = static_cast<int>(N * (0.10 + 0.35 * r.uni())), tb = static_cast<int>(N * (0.55 + 0.35 * r.uni()));
    const int T[4] = {0, ta, tb, N};
    const double V[4] = {op, up ? lo : hi, up ? hi : lo, cl};
    std::vector<double> x(static_cast<std::size_t>(N) + 1, op);
    std::vector<double> w;
    for (int s = 0; s < 3; ++s) {
      const int n = T[s + 1] - T[s];
      if (n <= 0) continue;
      const double sigma = 0.3 * (hi - lo) / std::sqrt(static_cast<double>(n));
      w.assign(static_cast<std::size_t>(n) + 1, 0.0);
      for (int k = 1; k <= n; ++k) w[static_cast<std::size_t>(k)] = w[static_cast<std::size_t>(k) - 1] + sigma * gauss();
      for (int k = 0; k <= n; ++k) {
        const double f = static_cast<double>(k) / n;
        x[static_cast<std::size_t>(T[s] + k)] = reflect(V[s] + (V[s + 1] - V[s]) * f + w[static_cast<std::size_t>(k)] - f * w[static_cast<std::size_t>(n)]);
      }
    }
    const Price t = in.tick;
    const Price lo_t = ((d.l + t - 1) / t) * t, hi_t = (d.h / t) * t;
    in.path.assign(static_cast<std::size_t>(N) + 1, d.o);
    for (std::size_t k = 0; k <= static_cast<std::size_t>(N); ++k) {
      const Price p = static_cast<Price>(std::llround(x[k] / static_cast<double>(t))) * t;
      in.path[k] = lo_t <= hi_t ? std::clamp(p, lo_t, hi_t) : static_cast<Price>(std::llround(x[k]));
    }
    // the four official prices exactly
    in.path[0] = d.o; in.path[static_cast<std::size_t>(ta)] = up ? d.l : d.h; in.path[static_cast<std::size_t>(tb)] = up ? d.h : d.l; in.path[static_cast<std::size_t>(N)] = d.c;
  }

  // Re-quotes the synthetic market makers around the current price. May fill the user's resting orders.
  void quote(std::size_t i) {
    Inst& in = inst_[i];
    const OrderId b0 = kMaker + i * 64;
    for (OrderId k = 0; k < 2 * kLevels; ++k) in.book.cancel(b0 + k);
    auto cb = [&](const Trade& t) { on_trade(t); };
    const int levels = in.level_qty > 0 ? kLevels : 5;
    for (int k = 0; k < levels; ++k) {
      Qty qa, qb;
      if (in.level_qty > 0) {   // 60%..140% of the level size, whole lots
        auto sz = [&]() { const double f = 0.6 + 0.8 * rng_.uni(); return std::max<Qty>(in.lot, static_cast<Qty>(static_cast<double>(in.level_qty) * f / static_cast<double>(in.lot)) * in.lot); };
        qa = sz(); qb = sz();
      } else {                  // fallback: 1..40 lots
        qa = (1 + static_cast<Qty>(rng_.next() % 40)) * in.lot; qb = (1 + static_cast<Qty>(rng_.next() % 40)) * in.lot;
      }
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
    if (r.o.side == Side::Buy) {           // each buy order is its own position lot
      Lot* l = find_lot(id);
      if (!l) { Lot n; n.id = id; n.inst = r.o.inst; n.ns = wall_ns_; n.mt = has_replay_ ? mt_ : -1; lots_.push_back(n); l = &lots_.back(); }
      l->qty += q; l->cost += notional; l->entry = l->cost / static_cast<double>(l->qty);
    } else {                               // the targeted lot first, then oldest lots not being closed, then the rest
      Qty left = q;
      auto take = [&](Lot& l) {
        const Qty a = std::min(left, l.qty);
        if (a <= 0) return;
        const double pnl = (rupees(px) - l.entry) * static_cast<double>(a);
        l.realized += pnl; real_ += pnl; l.cost -= l.entry * static_cast<double>(a); l.qty -= a; left -= a;
        if (l.qty == 0) { l.cost = 0; l.closing = false; }
      };
      if (r.lot) if (Lot* l = find_lot(r.lot)) take(*l);
      for (Lot& l : lots_) if (left > 0 && l.inst == r.o.inst && !l.closing) take(l);
      for (Lot& l : lots_) if (left > 0 && l.inst == r.o.inst) take(l);
    }
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
  std::vector<Lot> lots_;
  std::vector<Evt> events_;
  std::vector<Led> ledger_;
  std::vector<double> curve_;
  std::vector<Ts> stamps_;
  std::deque<Act> q_;
  std::vector<Act> later_;
  double dep_ = 0, fees_ = 0, turn_ = 0, real_ = 0;
  int live_ = 0;
  long ticks_ = 0;
  std::int64_t wall_ = 0, wall_ns_ = 0, day_ = 0;
  Ts last_step_ = 0, next_stub_ = 0, next_curve_ = 0;
  bool stub_ = false, kill_pending_ = false, flatten_pending_ = false;
  bool has_replay_ = false, closed_ = false;
  double mt_ = 0, speed_ = 1;
  std::string replay_date_;
};

}  // namespace mc
