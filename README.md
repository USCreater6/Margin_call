# Margin_call

Private systematic-trading research platform. **Everything in this repository is SIMULATION.**
There is no broker connection, no live market data and no real order path.

## What is built (Stage 1, increment 1)

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
| Web terminal | `web/index.html` | built, see limits below |

## What is not built yet

- **Real market data.** The only data so far is SYNTHETIC (`mc_gensynth`). Results on it test the engine and say nothing about real markets.
- **Terminal to engine link.** The terminal's order ticket, blotter and wallet run a demo simulation inside the browser. The C++ engine's results appear on the AI Engine page. Driving the terminal from the C++ core is the next increment.
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
bash scripts/serve.sh              # terminal on http://localhost:8080
```

To run on your own data, put `SYMBOL.csv` files (`date,open,high,low,close,volume`, date as YYYYMMDD, prices in rupees)
in a folder and set `MC_DATA_DIR=/path/to/folder`.

## Rules this code follows

- Every order reaches the exchange through one path: state machine → risk engine → venue.
- Every simulated result is labelled SIMULATION; synthetic input is labelled SYNTHETIC.
- Tests are not changed to make them pass. CI runs them in a normal build and under address and undefined-behaviour sanitizers.
- No performance figure is quoted without the machine it was measured on.

The approved plan is in `docs/PLAN.md`.
