// G.R.I.T trader agent inside the terminal (owner's decision, 15 Nov 2026). SIMULATION ONLY.
//
// v0 is not a trained model: it is a set of rules built from Margin_call's own strategy indicators (daily
// momentum and mean reversion on the last year of NSE closes) plus what the replayed day is doing. It decides
// everything itself: what to buy, how much, when to move its exit to break-even, when to take profit and when
// to close before the session ends. Every action carries a written reason and the inputs it used.
//
// What it can NOT do:
//   - change any limit: every order goes through LiveSim::submit, i.e. the C++ risk engine, with no exemption;
//   - press or clear the kill switch: only the owner can (the server's /api/kill and /api/rearm). A kill
//     switches the agent OFF; after a re-arm the owner turns it on again;
//   - change its own mode: OFF, SHADOW (decides and logs, sends nothing) or SIM (orders on the demo exchange)
//     is set by the owner. BROKER TEST and LIVE do not exist in this code.
// Every decision and every closed trade is also written as one JSON line (drain_log) for training the next
// version (grit-trader v1). That file stays on the machine running the terminal, never in the repository.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <sstream>
#include <string>
#include <vector>
#include "mc/session.hpp"
#include "mc/strategy.hpp"

namespace mc {

enum class AgentMode : std::uint8_t { Off, Shadow, Sim };
inline const char* to_string(AgentMode m) { return m == AgentMode::Sim ? "sim" : m == AgentMode::Shadow ? "shadow" : "off"; }

class Agent {
 public:
  static constexpr const char* kModel = "grit-trader-v0";
  static constexpr const char* kSrc = "GRIT-AGENT";
  // v0 rule settings (rules, not learned): take profit, move exit to break-even, size per position.
  static constexpr double kTarget = 0.03, kBreakEven = 0.01, kSizeShare = 0.25;
  static constexpr int kFirstEntry = 300, kLastEntry = LiveSim::kSession - 1800, kCloseAll = LiveSim::kSession - 600;

  struct View {           // the daily picture of one instrument, from its history
    bool hist = false, mom = false, rev = false;
    double fast = 0, slow = 0, z = 0, last = 0;
    int bars = 0;
  };
  struct Decision {
    long id = 0; std::int64_t wall_ns = 0; double mt = -1;
    std::string act, sym, qty, px, stop, target, reason, inputs, risk, order;
    double conf = -1;
  };
  struct Pos {            // a position the agent opened (SIM: a real lot; SHADOW: on paper only)
    std::size_t inst = 0; OrderId lot = 0; OrderId order = 0; Qty qty = 0, q0 = 0;
    double entry = 0, peak = 0, mt = 0; long dec = 0; bool be = false, open = true; std::string why;
  };

  void resize(std::size_t n) { view_.assign(n, {}); low_.assign(n, 0); high_.assign(n, 0); }

  // Daily closes of instrument i (oldest first). Momentum: 20-day average above the 50-day average and the
  // last close above the 20-day average. Mean reversion: last close 2 or more deviations below its 20-day average.
  void set_history(std::size_t i, const std::vector<Bar>& bars) {
    if (i >= view_.size()) return;
    std::vector<double> c;
    for (const Bar& b : bars) c.push_back(rupees(b.c));
    View v; v.bars = static_cast<int>(c.size());
    if (c.size() >= 50) {
      v.hist = true; v.last = c.back();
      v.fast = detail::sma(c, 20); v.slow = detail::sma(c, 50);
      double var = 0;
      for (std::size_t k = c.size() - 20; k < c.size(); ++k) var += (c[k] - v.fast) * (c[k] - v.fast);
      const double sd = std::sqrt(var / 20.0);
      v.z = sd > 0 ? (c.back() - v.fast) / sd : 0;
      v.mom = v.fast > v.slow && v.last > v.fast;
      v.rev = v.z <= -2.0;
    }
    view_[i] = v;
  }
  int with_history() const { int n = 0; for (const View& v : view_) n += v.hist; return n; }

  AgentMode mode() const { return mode_; }
  // The owner sets the mode. Leaving SIM closes nothing by itself: open positions stay with their exits.
  std::string set_mode(LiveSim& sim, AgentMode m, Ts now) {
    if (m == AgentMode::Sim && sim.killed()) return "Kill switch is on. Re-arm first, then turn the agent on.";
    if (m == mode_) return "";
    note(sim, now, "MODE", std::string("Owner set the agent to ") + to_string(m) + " (was " + to_string(mode_) + ")");
    if (m != AgentMode::Shadow) for (Pos& p : pos_) if (p.open && !p.lot && !p.order) p.open = false;   // paper positions end with SHADOW
    mode_ = m;
    return "";
  }
  // Called by the server when the owner presses KILL. The agent goes OFF and cannot turn itself on again.
  void on_kill(LiveSim& sim, Ts now) {
    if (mode_ == AgentMode::Off) return;
    mode_ = AgentMode::Off;
    note(sim, now, "STOP", "Owner pressed KILL: the agent is OFF. Only the owner can re-arm and turn it on again.");
  }
  // Called after a demo reset or replay restart: paper positions and the day's highs and lows start again.
  void new_day() {
    for (Pos& p : pos_) p.open = false;
    std::fill(low_.begin(), low_.end(), 0); std::fill(high_.begin(), high_.end(), 0);
    last_mt_ = -1; last_hold_ = -1e9;
  }

  // One decision round. The server calls it on every market tick; it acts at most once every 4 seconds.
  void tick(LiveSim& sim, Ts now) {
    if (view_.size() != sim.instruments()) resize(sim.instruments());
    const double mt = sim.market_time();
    if (mt + 1 < last_mt_) new_day();
    last_mt_ = mt;
    for (std::size_t i = 0; i < sim.instruments(); ++i) {
      const double px = rupees(sim.price(i));
      if (low_[i] <= 0 || px < low_[i]) low_[i] = px;
      if (px > high_[i]) high_[i] = px;
    }
    link_lots(sim);
    if (mode_ == AgentMode::Off) return;
    if (sim.killed()) { mode_ = AgentMode::Off; return; }
    if (now < next_) return;
    next_ = now + 4000000000LL;
    if (manage(sim, now)) return;   // one action per round
    if (enter(sim, now)) return;
    if (mt - last_hold_ >= 900 && sim.has_replay() && !sim.market_closed() && mt >= kFirstEntry && mt <= kLastEntry) {
      last_hold_ = mt;
      decide(sim, now, "HOLD", "", "", "", "", "", -1,
             "No instrument clears the entry rules now (" + std::to_string(with_history()) + " of " + std::to_string(sim.instruments()) +
             " have a year of history to judge).", "all inputs", "-", "-");
    }
  }

  // JSON lines for training, removed from the agent as they are returned.
  std::vector<std::string> drain_log() { std::vector<std::string> out; out.swap(log_); return out; }

  std::string json(const LiveSim& sim) const {
    std::ostringstream o;
    o.precision(10);
    const RiskLimits& L = sim.limits();
    const double eq = sim.equity(), base = sim.base();
    double unreal = 0; int open = 0;
    for (const Pos& p : pos_) if (p.open && p.qty > 0) { ++open; unreal += (rupees(sim.price(p.inst)) - p.entry) * static_cast<double>(p.qty); }
    o << "{\"mode\":\"" << to_string(mode_) << "\",\"model\":\"" << kModel << "\",\"killed\":" << (sim.killed() ? "true" : "false")
      << ",\"hist\":" << with_history() << ",\"n\":" << sim.instruments()
      << ",\"stats\":{\"decisions\":" << next_id_ - 1 << ",\"trades\":" << trades_ << ",\"wins\":" << wins_ << ",\"realized\":" << realized_
      << ",\"unreal\":" << unreal << ",\"open\":" << open << ",\"logged\":" << logged_ << "}"
      << ",\"rules\":{\"target\":" << kTarget << ",\"breakeven\":" << kBreakEven << ",\"size\":" << kSizeShare << "}"
      << ",\"limits\":{\"stop\":" << L.stop_pct << ",\"lock\":" << L.profit_lock << ",\"daily\":" << L.daily_loss << ",\"strat\":" << L.strat_daily_loss
      << ",\"rate\":" << L.rate_per_sec << ",\"gross\":" << L.max_gross << ",\"pos\":" << L.max_positions << ",\"orders\":" << L.max_open_orders
      << ",\"used\":{\"pnl\":" << (base > 0 ? (eq - base) / base : 0) << ",\"gross\":" << (eq > 0 ? sim.gross_now() / eq : 0)
      << ",\"pos\":" << sim.open_positions() << ",\"orders\":" << sim.live_orders() << "}}"
      << ",\"positions\":[";
    bool f = true;
    for (const Pos& p : pos_) {
      if (!p.open || p.qty <= 0) continue;
      const double px = rupees(sim.price(p.inst));
      o << (f ? "" : ",") << "{\"s\":\"" << esc(sim.sym(p.inst)) << "\",\"q\":" << p.qty << ",\"e\":" << p.entry << ",\"p\":" << px
        << ",\"pnl\":" << (px - p.entry) * static_cast<double>(p.qty) << ",\"stop\":" << (p.be ? p.entry : p.entry * (1 - L.stop_pct))
        << ",\"be\":" << (p.be ? "true" : "false") << ",\"tgt\":" << p.entry * (1 + kTarget) << ",\"paper\":" << (p.lot || p.order ? "false" : "true")
        << ",\"plan\":\"" << esc(plan(p)) << "\"}";
      f = false;
    }
    o << "],\"decisions\":[";
    for (std::size_t k = dec_.size(); k-- > 0;) {
      const Decision& d = dec_[k];
      o << (k + 1 == dec_.size() ? "" : ",") << "{\"id\":" << d.id << ",\"ns\":\"" << d.wall_ns << "\",\"mt\":" << d.mt << ",\"act\":\"" << esc(d.act)
        << "\",\"s\":\"" << esc(d.sym) << "\",\"q\":\"" << esc(d.qty) << "\",\"px\":\"" << esc(d.px) << "\",\"st\":\"" << esc(d.stop) << "\",\"tg\":\"" << esc(d.target)
        << "\",\"c\":" << d.conf << ",\"why\":\"" << esc(d.reason) << "\",\"in\":\"" << esc(d.inputs) << "\",\"risk\":\"" << esc(d.risk)
        << "\",\"ord\":\"" << esc(d.order) << "\"}";
    }
    o << "]}";
    return o.str();
  }

  const std::deque<Decision>& decisions() const { return dec_; }
  const std::vector<Pos>& positions() const { return pos_; }

 private:
  static std::string esc(const std::string& s) {
    std::string r;
    for (char c : s) {
      if (c == '"' || c == '\\') { r += '\\'; r += c; }
      else if (c == '\n' || c == '\r' || c == '\t') r += ' ';
      else if (static_cast<unsigned char>(c) < 0x20) continue;
      else r += c;
    }
    return r;
  }
  static std::string f2(double x) { std::ostringstream o; o.setf(std::ios::fixed); o.precision(2); o << x; return o.str(); }
  static std::string plan(const Pos& p) {
    return p.be ? "Exit at entry if the price comes back; take profit at +3%; close by 15:20."
                : "Engine stop at -2%; exit moves to entry after +1%; take profit at +3%; close by 15:20.";
  }

  // SIM positions: the lot appears when the market buy fills (lot id = buy order id); it ends when the lot is flat.
  void link_lots(const LiveSim& sim) {
    for (Pos& p : pos_) {
      if (!p.open || !p.order) continue;
      const LiveSim::Lot* l = nullptr;
      for (const LiveSim::Lot& x : sim.lots()) if (x.id == p.order) l = &x;
      if (!p.lot) {
        if (l && l->qty > 0) { p.lot = l->id; p.qty = p.q0 = l->qty; p.entry = l->entry; p.peak = std::max(p.peak, l->entry); }
        else if (!order_live(sim, p.order)) p.open = false;   // the buy did not fill (rejected or cancelled)
        continue;
      }
      if (!l || l->qty <= 0) {
        p.open = false;
        const double pnl = l ? l->realized : 0;
        outcome(p, l ? (p.entry + pnl / static_cast<double>(std::max<Qty>(1, p.q0))) : 0, pnl, l ? l->closer : "unknown", sim.market_time());
      } else {
        p.qty = l->qty;
      }
    }
  }
  static bool order_live(const LiveSim& sim, OrderId id) {
    if (id == 0 || id > sim.orders().size()) return false;
    const State st = sim.orders()[id - 1].o.st;
    return is_live(st);
  }

  // Exits and exit moves on the agent's own positions. Returns true when it acted.
  bool manage(LiveSim& sim, Ts now) {
    const double mt = sim.market_time();
    for (Pos& p : pos_) {
      if (!p.open || p.qty <= 0 || (p.order && !p.lot)) continue;
      if (p.lot && mode_ != AgentMode::Sim) continue;   // a real lot is left to the engine's stop while the agent is not in SIM
      const double px = rupees(sim.price(p.inst));
      p.peak = std::max(p.peak, px);
      std::string why;
      if (sim.has_replay() && mt >= kCloseAll) why = "Session ends at 15:30; v0 does not hold positions overnight.";
      else if (px >= p.entry * (1 + kTarget)) why = "Take-profit reached (+3% from entry).";
      else if (p.be && px <= p.entry) why = "Price came back to entry after a +1% gain; exit at break-even.";
      if (!why.empty()) { close(sim, p, now, why); return true; }
      if (!p.be && p.peak >= p.entry * (1 + kBreakEven)) {
        p.be = true;
        decide(sim, now, "MOVE EXIT", sim.sym(p.inst), std::to_string(p.qty), f2(px), "exit at entry " + f2(p.entry), "+3%", 0.8,
               "Up 1% from entry: the agent now exits at entry if the price falls back (the engine's 2% stop stays too).", "own position", "-",
               p.lot ? "lot " + std::to_string(p.lot) : "paper");
        return true;
      }
    }
    return false;
  }
  void close(LiveSim& sim, Pos& p, Ts now, const std::string& why) {
    const double px = rupees(sim.price(p.inst));
    if (mode_ == AgentMode::Sim && p.lot) {
      const int n = sim.close_lots({p.lot}, kSrc, now);
      decide(sim, now, "CLOSE", sim.sym(p.inst), std::to_string(p.qty), f2(px), "-", "-", -1, why, "own position", "-",
             n ? "exit queued" : "already closing");
    } else if (!p.lot && !p.order) {   // paper position (SHADOW)
      decide(sim, now, "CLOSE", sim.sym(p.inst), std::to_string(p.qty), f2(px), "-", "-", -1, why, "own position", "not sent (SHADOW)", "paper");
      p.open = false;
      outcome(p, px, (px - p.entry) * static_cast<double>(p.qty), "AGENT (paper)", sim.market_time());
    } else {   // a real lot while the agent is in SHADOW: the agent does not send orders, the engine's stop stays
      decide(sim, now, "CLOSE", sim.sym(p.inst), std::to_string(p.qty), f2(px), "-", "-", -1, why, "own position", "not sent (SHADOW)",
             "lot stays open");
    }
  }

  // New entries: at most one per round, long only (the demo exchange has no short selling).
  bool enter(LiveSim& sim, Ts now) {
    const double mt = sim.market_time();
    if (!sim.has_replay() || sim.market_closed() || mt < kFirstEntry || mt > kLastEntry) return false;
    if (sim.halted() || sim.locked() || sim.killed()) return false;
    int mine = 0;
    for (const Pos& p : pos_) mine += p.open;
    if (mine >= sim.limits().max_positions) return false;
    std::size_t best = sim.instruments();
    double best_c = 0; std::string best_why, best_in;
    for (std::size_t i = 0; i < sim.instruments(); ++i) {
      const View& v = view_[i];
      if (!v.hist) continue;
      bool held = false;
      for (const Pos& p : pos_) held = held || (p.open && p.inst == i);
      if (held) continue;
      const double px = rupees(sim.price(i)), op = rupees(sim.day_open(i));
      double c = 0; std::string why, in;
      if (v.mom && op > 0 && px > op * 1.002) {
        c = 0.55 + (px > op * 1.006 ? 0.1 : 0) + (v.fast > v.slow * 1.02 ? 0.05 : 0);
        why = "Daily momentum is up (20-day average " + f2(v.fast) + " above 50-day " + f2(v.slow) + ") and today trades above its open (" + f2(op) + ").";
        in = "momentum indicators (1 year of closes) · replay price vs open";
      } else if (v.rev && low_[i] > 0 && px >= low_[i] * 1.003 && px < op) {
        c = 0.55 + (v.z <= -2.5 ? 0.1 : 0);
        why = "Close is " + f2(-v.z) + " deviations below its 20-day average and today has bounced 0.3% off its low (" + f2(low_[i]) + ").";
        in = "mean-reversion indicators (1 year of closes) · replay price vs day low";
      }
      if (c > best_c) { best_c = c; best = i; best_why = why; best_in = in; }
    }
    if (best >= sim.instruments()) return false;
    const double px = rupees(sim.best_ask(best) > 0 ? sim.best_ask(best) : sim.price(best));
    const Qty lot = sim.lot_size(best);
    const double each = px * static_cast<double>(lot);
    Qty lots = each > 0 ? static_cast<Qty>(std::floor(sim.equity() * kSizeShare / each)) : 0;
    if (lots < 1) lots = 1;
    const Qty q = lots * lot;
    const std::string stop = f2(px * (1 - sim.limits().stop_pct)), tgt = f2(px * (1 + kTarget));
    Pos p; p.inst = best; p.qty = p.q0 = q; p.entry = px; p.peak = px; p.mt = mt; p.why = best_why;
    if (mode_ == AgentMode::Sim) {
      const std::string veto = sim.submit(best, Side::Buy, OrdType::Market, q, 0, kSrc, now);
      const LiveSim::Rec& r = sim.orders().back();
      p.dec = decide(sim, now, "BUY", sim.sym(best), std::to_string(q), f2(px), stop, tgt, best_c, best_why, best_in,
                     veto.empty() ? "PASS" : "VETO · " + veto, std::string(to_string(r.o.st)) + " · order " + std::to_string(r.o.id));
      if (veto.empty()) { p.order = r.o.id; pos_.push_back(p); link_lots(sim); }
    } else {
      p.dec = decide(sim, now, "BUY", sim.sym(best), std::to_string(q), f2(px), stop, tgt, best_c, best_why, best_in, "not sent (SHADOW)", "paper");
      pos_.push_back(p);
    }
    if (pos_.size() > 200) pos_.erase(std::remove_if(pos_.begin(), pos_.end(), [](const Pos& x) { return !x.open; }), pos_.end());
    return true;
  }

  long decide(const LiveSim& sim, Ts, const std::string& act, const std::string& sym, const std::string& qty, const std::string& px,
              const std::string& stop, const std::string& tgt, double conf, const std::string& why, const std::string& in,
              const std::string& risk, const std::string& order) {
    Decision d;
    d.id = next_id_++; d.wall_ns = sim.wall_ns(); d.mt = sim.has_replay() ? sim.market_time() : -1;
    d.act = act; d.sym = sym; d.qty = qty; d.px = px; d.stop = stop; d.target = tgt; d.conf = conf; d.reason = why; d.inputs = in;
    d.risk = risk; d.order = order;
    dec_.push_back(d);
    if (dec_.size() > 120) dec_.pop_front();
    std::ostringstream o;
    o.precision(10);
    o << "{\"type\":\"decision\",\"id\":" << d.id << ",\"wall_ns\":" << d.wall_ns << ",\"mt\":" << d.mt << ",\"mode\":\"" << to_string(mode_)
      << "\",\"model\":\"" << kModel << "\",\"act\":\"" << esc(act) << "\",\"sym\":\"" << esc(sym) << "\",\"qty\":\"" << esc(qty) << "\",\"px\":\"" << esc(px)
      << "\",\"stop\":\"" << esc(stop) << "\",\"target\":\"" << esc(tgt) << "\",\"conf\":" << conf << ",\"reason\":\"" << esc(why)
      << "\",\"inputs\":\"" << esc(in) << "\",\"risk\":\"" << esc(risk) << "\",\"order\":\"" << esc(order) << "\"";
    const int i = sym.empty() ? -1 : sim.find_symbol(sym);
    if (i >= 0) {
      const View& v = view_[static_cast<std::size_t>(i)];
      const std::size_t k = static_cast<std::size_t>(i);
      o << ",\"state\":{\"px\":" << rupees(sim.price(k)) << ",\"open\":" << rupees(sim.day_open(k)) << ",\"low\":" << low_[k] << ",\"high\":" << high_[k]
        << ",\"sma20\":" << v.fast << ",\"sma50\":" << v.slow << ",\"z20\":" << v.z << ",\"mom\":" << (v.mom ? "true" : "false")
        << ",\"rev\":" << (v.rev ? "true" : "false") << ",\"equity\":" << sim.equity() << ",\"positions\":" << sim.open_positions() << "}";
    }
    o << "}";
    log_.push_back(o.str()); ++logged_;
    return d.id;
  }
  void note(const LiveSim& sim, Ts now, const std::string& act, const std::string& why) {
    decide(sim, now, act, "", "", "", "", "", -1, why, "owner", "-", "-");
  }
  void outcome(const Pos& p, double exit, double pnl, const std::string& by, double mt) {
    ++trades_; wins_ += pnl > 0; realized_ += pnl;
    std::ostringstream o;
    o.precision(10);
    o << "{\"type\":\"outcome\",\"decision\":" << p.dec << ",\"lot\":" << p.lot << ",\"qty\":" << p.q0 << ",\"entry\":" << p.entry
      << ",\"exit\":" << exit << ",\"pnl\":" << pnl << ",\"pnl_pct\":" << (p.entry > 0 ? (exit / p.entry - 1) : 0) << ",\"closed_by\":\"" << esc(by)
      << "\",\"held_s\":" << (mt - p.mt) << ",\"paper\":" << (p.lot ? "false" : "true") << "}";
    log_.push_back(o.str()); ++logged_;
  }

  AgentMode mode_ = AgentMode::Off;
  std::vector<View> view_;
  std::vector<double> low_, high_;
  std::vector<Pos> pos_;
  std::deque<Decision> dec_;
  std::vector<std::string> log_;
  Ts next_ = 0;
  long next_id_ = 1, logged_ = 0;
  int trades_ = 0, wins_ = 0;
  double realized_ = 0, last_mt_ = -1, last_hold_ = -1e9;
};

}  // namespace mc
