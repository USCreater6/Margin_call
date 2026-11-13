# Margin_call

Private systematic-trading research platform. **Everything in this repository is SIMULATION.**
There is no broker connection, no live market data and no real order path.

## What is built (Stage 1)

| Part | Where | State |
|---|---|---|
| Limit order book, price-time priority matching | `include/mc/order_book.hpp` | built, tested |
| Order-state machine | `include/mc/osm.hpp` | built, tested |
| Risk engine: per-trade stop, daily loss halt, profit lockout, order-value, exposure, position and order caps, 8/s rate limiter, kill switch | `include/mc/risk.hpp` | built, tested |
| Portfolio, PnL, per-strategy attribution | `include/mc/portfolio.hpp` | built, tested |
| Demo exchange for bar data (slippage, spread, depth, partial fills, stop gaps, injected rejects, fixed seed) | `include/mc/exchange.hpp` | built, tested |
| Data loader with duplicate / out-of-order / gap / bad-row checks | `include/mc/replay.hpp` | built, tested |
| Strategies: momentum, mean reversion (long-only, daily bars) | `include/mc/strategy.hpp` | built |
| Backtest engine, cost sensitivity, walk-forward | `include/mc/backtest.hpp`, `apps/backtest.cpp` | built, tested |
| Latency benchmark for the matching engine | `apps/bench.cpp` | built |
| Live simulation session: terminal orders go through the same risk engine and matching engine | `include/mc/session.hpp` | built, tested |
| Terminal server (static files + JSON API, localhost only by default) | `apps/server.cpp` | built, smoke-tested in CI |
| NSE daily-data fetcher; splits and bonuses from `tools/corporate_actions.csv` | `tools/fetch_nse.py` | built, run on GitHub |
| Starting prices and one-year history for all NSE equities | `tools/fetch_latest.py` | built, run in CI |
| Connector data: NSE/BSE listings, Wikidata, GLEIF, annual-report file | `tools/build_connector.py` | built, run in CI |
| Web terminal | `web/index.html` | built, see limits below |

## What is not built yet

- **Real data inside the repo.** NSE daily files are downloaded on demand (`scripts/fetch_data.sh` or the *real-data-backtest* workflow) and are not committed, because redistribution terms are not verified. Without them the report uses SYNTHETIC data.
- **Real ticks.** Starting prices are the last trading day's official NSE closes (fetched when the terminal starts). Ticks after that are synthetic.
- **MCX.** MCX's site returns HTTP 403 to automated downloads, so there are no commodity futures. GOLDBEES (NSE gold ETF) is in the watchlist instead.
- **Connector coverage.** Search covers every NSE and BSE code. Links are ownership only (Wikidata, GLEIF, and the annual-report file `tools/connector_annual_reports.csv`, which is empty so far). No supplier or customer data.
- **Short selling and leverage.** The terminal is long-only and cash-limited (open orders count), per the no-leverage default in the plan.
- **Dividend adjustment and MCX data.** Prices are adjusted for splits and bonuses only. No commodity data yet.
- **Strategies in the terminal.** Momentum and mean reversion run in the backtest only. In the terminal, orders come from the ticket or the random stub.
- **News, video and connector data.** Sample content only.
- **GRIT.** The connector slot is empty; the "stub signals" button is random, not a model.
- **Allocation-free hot path.** The order book preallocates its order pool, but price levels use `std::map`, which allocates.
- **Fee and tax model.** A flat placeholder (3 bps per side). The real schedule is not verified.
- Stop, modify and expire flows on the demo exchange are partial: stop orders and cancels are built; modify exists in the order book only.

## Run it

**GitHub Codespaces (browser only):** on the repository page choose *Code → Codespaces → Create codespace on main*.
The container builds the code, runs all tests, produces the simulation report and serves the terminal on port 8080,
which Codespaces forwards to a private address only you can open.

**Any Linux machine:**

```
bash scripts/build_and_report.sh   # build, run all tests, write web/reports/
bash scripts/serve.sh              # terminal on http://localhost:8080, driven by the C++ engine
bash scripts/fetch_data.sh 2019-01-01   # optional: download NSE daily data and rerun the report on it
```

To run on your own data, put `SYMBOL.csv` files (`date,open,high,low,close,volume`, date as YYYYMMDD, prices in rupees)
in a folder and set `MC_DATA_DIR=/path/to/folder`.

## Rules this code follows

- Every order reaches the exchange through one path: state machine → risk engine → venue.
- Every simulated result is labelled SIMULATION; synthetic input is labelled SYNTHETIC.
- Tests are not changed to make them pass. CI runs them in a normal build and under address and undefined-behaviour sanitizers.
- No performance figure is quoted without the machine it was measured on.

The approved plan is in `docs/PLAN.md`.
