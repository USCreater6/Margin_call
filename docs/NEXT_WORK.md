# Margin_call — work remaining after this build

Status: Stage 1 (historical simulation). Nothing here is live. No strategy has passed validation.
Order of the list = recommended order. "Needs you" = a decision or input only you can give.

## A. Before any strategy can be trusted (Stage 1 completion)

1. **Strategy research continues.** 0 of 6 families pass the fixed criteria on NIFTY 50
   (OOS return at 2x cost > 0, worst fold drawdown < 15%, >= 55% positive folds). Closest:
   rotation+regime (fails positive folds 6/11) and rotation (fails drawdown 25%).
   Criteria stay fixed; new ideas are tested against them, not the other way round.
2. **Survivorship-free universe.** Backtests use today's NIFTY 50 members. Need historical
   index membership (NSE index change notices) so removed companies are included.
3. **Real costs.** Fees are a flat placeholder. Need the actual schedule: brokerage (broker
   dependent — needs you), STT, exchange charges, SEBI fee, stamp duty, GST. Must verify each rate.
4. **Tick sizes: done (14 Nov).** Read from NSE's security / contract master files (BidIntrvl) and
   checked against the day's prices; falls back to 0.01, labelled, if a file cannot be read.
5. **Corporate actions beyond splits/bonuses.** Demergers (e.g. Reliance → Jio Financial 2023),
   rights issues and dividends are not adjusted. Needed for total-return accuracy.
5b. **Real intraday path for the replay.** The terminal now replays the last NSE day's official
   open/high/low/close; the path between them is simulated. NSE's intraday chart endpoint answered on
   Saturday 14 Nov with no points; check it on a trading day (Mon 16 Nov) before using it.
6. **Strategies inside the terminal session.** Today they run in backtest only; terminal orders
   come from the ticket or the random stub.

## B. Platform hardening (before Stage 2)

7. **Persistence: done for the demo account (14 Nov).** Wallet, positions, orders, ledger and replay
   clock are saved to data/session/state.tsv every 2 s and after every action, and restored on start.
   Still open: a full append-only audit journal (every event, never trimmed) before Stage 2.
8. **Stop and modify from the terminal UI** (engine supports them; UI does not expose them).
9. **Security.** Repo is public; server has no login. Before any broker key exists:
   private repo or secrets only in Codespaces secrets, real login, HTTPS only.
10. **Codespaces cost/quota.** Free hours and storage on your GitHub plan must be checked
    (needs you: GitHub billing page).

## C. Stage 2 — live data + simulated orders (needs your approval to start)

**17 Nov 2026 (owner approved preview): market watch + replay on real 1-minute candles.** The watchlist is replaced
by a market-watch grid (tabs from config/watchlists.json: DEFAULT, two sector lists still empty; My list; NIFTY 50),
an index strip, a Depth | Chart | Option chain dock with the ticket under it, and a messages bar. The replay now
follows real 1-minute candles (tools/fetch_intraday.py, Upstox candle API; today's session once complete after 15:30,
else the last trading day): every minute's open, high, low and close are real, only moves inside a minute are
simulated; the grid shows high, low and volume only up to the replay time (no look-ahead). Verified 17 Nov: minute
volumes sum to NSE's daily volume (AIAENG 87,880 = 87,880; INFY 4,851,757 vs 4,852,092). Not yet: HFCL (BE series,
trade-for-trade) is not loaded; NIFTY options still use the official close's implied volatility.

**17 Nov 2026 (owner decision): Upstox is the live source.** The NSE server above never delivered data on 16 Nov
(available: false). tools/upstox_feed.py (called by tools/live_feed.py when the Codespaces secret
UPSTOX_ANALYTICS_TOKEN is set) uses Upstox's read-only Analytics Token (1 year, free, cannot place orders; no
OAuth app, no redirect URL, no static IP for market data). Every 4 s in NSE hours: full quotes (last price, 5-level
bid/ask, OHLC, volume, circuits) for the shown stocks and the DEFAULT list (config/watchlists.json), and LTP for
NIFTY 50, NIFTY BANK, INDIA VIX, SENSEX and the nearest MCX futures; written to data/live/quotes.tsv (engine
unchanged) and data/live/upstox_quotes.json (for the market-watch grid, design preview pending). RK Global dropped
(no public API found). **Must verify on the first market session:** keys per full-quote call (batches of 100 sent),
the rate limit for the Analytics Token, whether a Codespace IP is accepted, and whether the secret reaches the
lifecycle scripts. scripts/day_refresh.sh fetches the day's official NSE file in the evening (16:00-22:00 IST,
every 30 min) and restarts the engine on it, so the replay is never a day behind.

**Started 16 Nov 2026 (owner approved): free live prices for the demo.** tools/live_feed.py reads NSE's cm-market
data server (free, no account; about once a minute, delayed 1-3 minutes; terms: informational / not for real-time
trading) in market hours and writes data/live/quotes.tsv; the engine trades demo orders at those prices and replays
the last official day otherwise. NIFTY-FUT and NIFTY options are DERIVED from the live NIFTY 50 index (no free live
F&O source without a broker account). **Must verify on the first market session:** the server's answer field names
(it returns no data while the market is closed, so they could not be seen; the parser accepts several spellings and
keeps raw answers in data/live/raw_*.json), and whether NSE's Akamai edge blocks Codespaces (one probe got
"Access Denied"). Is nseindia.in NSE's own domain: not confirmed.

11. **Broker choice** (needs you). Then read that broker's API docs for: market data feed,
    order types, rate limits, sandbox availability. Nothing assumed until read.
12. **Intraday data** comes from the broker feed; free sources give end-of-day only.
13. **MCX commodities.** MCX blocks automated downloads (HTTP 403). Needs a broker feed or a paid source.
14. **Latency measurement on the real node** (current numbers are from cloud test machines).

## D. Information panels

15. **News:** second source (GDELT is rate-limited from GitHub runners); keyword tagging is
    simple — improve with a vetted word list or a model in the AI-engine slot.
16. **Business TV:** 14 channels verified by YouTube channel feed on 14 Nov. Later the same day the feed
    returned HTTP 404 from GitHub's machines, so the list now falls back to the names verified on 14 Nov. Whether each one allows
    embedding can only be confirmed in your browser; "Open on YouTube" is the fallback.
17. **Fundamentals:** currently NIFTY 50 then NIFTY 500, quarters from NSE filings.
    Extend to all listed companies and older quarters.
18. **Connector:** annual-report related-party lists file (tools/connector_annual_reports.csv) is empty.

## E. AI engine

19. **G.R.I.T agent v0 built (15 Nov 2026).** Agent tab: you set OFF / SHADOW / SIM; the agent decides every
    action with a reason; every order goes through the C++ risk engine; KILL turns it OFF and only you re-arm.
    v0 is rules from the strategies' indicators; v1 is trained on its logged decisions (data/agent/, never
    committed) on a GPU from 21 Nov. Headlines, filings and connector links are not used by v0 yet.

**18 Nov 2026 (owner approved the 18 Nov preview, part 2 = terminal UI):** chart on TradingView Lightweight
Charts v5.3.0 (web/vendor, Apache 2.0, credit on the chart and in its footer) in web/chart.js: 1s-1h and 1D, ranges
1D-5Y, candle/bar/line, SMA/EMA/VWAP/Bollinger/Volume/RSI/MACD computed by Margin_call, trend line, horizontal line,
measure, chart BUY/SELL with Instant on/off, Scalper (NIFTY options, MARKET, buy only, product Scalper) and Full screen.
Numbered watchlists stored by the engine (+ new, rename, delete; search + Enter adds to the open list). Floating order
form (Intraday/Delivery/MTF; MARKET/LIMIT/SL/SL-M; SL/TP/TSL %). Bottom panel: Open orders (incl. waiting SL/SL-M,
modify), Positions, Holdings, Basket (20), GTT (single/OCO), Alerts; product chips. Sections: drag to resize,
maximise (Esc), Ctrl + wheel zoom. Header: Hol. / Pos. P&L with hide. Strip: next expiry.
**Still needs you:** MTF leverage, interest per day and eligible stocks (MTF refused until set); stocks for the
CRUDE OIL & NATURAL GAS and AIR TRANSPORT SERVICE lists. **Must verify:** the Upstox stream with your token.

## Needs you (summary)

- Delete the old Codespace and create a new one once (auto-update works from then on), send a screenshot.
- Broker choice (items 3, 11).
- Approval before Stage 2.
- MTF parameters; stocks for the two sector lists.
- GitHub plan limits (item 10).
