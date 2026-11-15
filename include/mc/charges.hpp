// NSE statutory charges on a trade, as in force on 16 Nov 2026. Brokerage is 0: no broker has been chosen.
//
// Sources, checked 16 Nov 2026:
//  - Exchange transaction charges, each side, including the NSE IPFT contribution, from 1 Mar 2026:
//    NSE circular NSE/FA/73061: cash market Rs 307 per crore of traded value (0.00307%),
//    equity futures Rs 183 per crore (0.00183%).
//  - Securities transaction tax: futures 0.05% of the sale value from 1 Apr 2026 (Finance Act 2026; was 0.02%);
//    equity delivery 0.1% on buy and on sell; equity intraday 0.025% on the sell side; gold ETFs: no STT.
//  - SEBI turnover fee: Rs 10 per crore (0.0001%), each side.
//  - Stamp duty, buyer only: delivery 0.015%, intraday 0.003%, futures 0.002%.
//  - GST: 18% on (brokerage + exchange transaction charges + SEBI fee).
//  - Index options (on the premium): STT 0.15% on the sell side (from 1 Apr 2026, Finance Act 2026; was 0.1%),
//    NSE transaction charge 0.03553% each side (IPFT included since 1 Mar 2026), stamp duty 0.003% on the buy.
//    STT on options exercised at expiry (0.15% of intrinsic value) does not arise: open options are closed at
//    market before expiry (see session.hpp).
// Equity intraday vs delivery is decided the way a contract note does it: a quantity bought and sold on the
// same day is intraday; a quantity carried past the day is delivery (the session charges the delivery
// difference on the buy side when the day changes).
#pragma once
#include "mc/types.hpp"

namespace mc {

enum class Seg : std::uint8_t { Stock, GoldEtf, IndexFuture, IndexOption };
inline const char* to_string(Seg s) { return s == Seg::IndexFuture ? "index-future" : s == Seg::IndexOption ? "index-option" : s == Seg::GoldEtf ? "gold-etf" : "stock"; }

struct ChargeRates {
  double txn_cash = 307e-7, txn_fut = 183e-7, txn_opt = 3553e-7;   // per rupee of value (options: of premium), each side
  double sebi = 10e-7;                                // Rs 10 per crore
  double gst = 0.18;                                  // on (brokerage + transaction + SEBI)
  double stt_fut_sell = 0.0005, stt_opt_sell = 0.0015, stt_deliv = 0.001, stt_intra_sell = 0.00025;
  double stamp_deliv = 0.00015, stamp_intra = 0.00003, stamp_fut = 0.00002, stamp_opt = 0.00003;
  double brokerage = 0;                               // no broker yet
};

struct Charges {
  double stt = 0, txn = 0, sebi = 0, stamp = 0, gst = 0, brokerage = 0;
  double total() const { return stt + txn + sebi + stamp + gst + brokerage; }
  Charges& operator+=(const Charges& o) { stt += o.stt; txn += o.txn; sebi += o.sebi; stamp += o.stamp; gst += o.gst; brokerage += o.brokerage; return *this; }
};

// Charges on one fill of `value` rupees. `delivery` matters for stocks and ETFs only.
inline Charges nse_charges(const ChargeRates& r, Seg seg, Side side, double value, bool delivery) {
  Charges c;
  const bool fut = seg == Seg::IndexFuture, opt = seg == Seg::IndexOption, buy = side == Side::Buy;
  c.brokerage = r.brokerage * value;
  c.txn = value * (fut ? r.txn_fut : opt ? r.txn_opt : r.txn_cash);
  c.sebi = value * r.sebi;
  c.gst = r.gst * (c.brokerage + c.txn + c.sebi);
  if (buy) c.stamp = value * (fut ? r.stamp_fut : opt ? r.stamp_opt : delivery ? r.stamp_deliv : r.stamp_intra);
  if (fut) c.stt = buy ? 0 : value * r.stt_fut_sell;
  else if (opt) c.stt = buy ? 0 : value * r.stt_opt_sell;
  else if (seg == Seg::Stock) c.stt = delivery ? value * r.stt_deliv : (buy ? 0 : value * r.stt_intra_sell);
  return c;   // gold ETF: no STT
}

}  // namespace mc
