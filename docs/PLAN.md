# Margin_call — Plan for Approval

Repo: `USCreater6/Margin_call` · Status: plan and preview stage, no production code written.
Items marked **[must verify]** are not confirmed.

## 01 Project charter
- Private, single-owner systematic trading platform. Personal capital only.
- MVP is a simulation and research environment. No broker, no real orders.
- Purpose of the MVP: test the theory and produce a feasibility view for the full system.

## 02 Phased roadmap
- Stage 1: historical simulation (demo exchange, replay, backtest, walk-forward, terminal). Ends with a validation report.
- Stage 2: live market data, simulated orders. Needs a broker account.
- Stage 3: broker test environment, only if the broker has one.
- Stage 4: one strategy, one instrument, smallest size, after explicit approval.
- Live news and video are live data. Building them into the MVP moves part of Stage 2 forward (requested by owner).

## 03 Architecture
```
data files / news feeds -> replay -> market-data bus
bus -> strategies + AI engine slot (GRIT)
    -> risk engine (can veto) -> rate limiter
    -> adapter interface -> demo exchange (order book + matcher)
fills -> order-state machine -> positions / PnL / reconciliation
audit log + telemetry -> web terminal
```
- Build and tests: GitHub Actions. The C++ core is compiled natively for tests and backtests, and to WebAssembly for the browser terminal. **[must verify]**
- Where the terminal runs (GitHub Pages, a private browser workspace, or a local machine) is an open decision.

## 04 C++ modules (C++20)
- core, lob, matcher, replay, strategy, router, risk, osm (order-state machine), portfolio, costs, backtest, walkforward, telemetry, audit, adapter.
- Outside the C++ core: news ingest, connector graph builder, AI engine client.

## 05 Data flow
Bar or headline -> signal -> order intent -> pre-trade risk -> rate limiter -> exchange -> fill -> position and PnL -> reconciliation -> audit.

## 06 Order-state machine
```
NEW -> RISK_CHECK -> REJECTED_RISK | SENT
SENT -> REJECTED_VENUE | ACKED
ACKED -> PARTIALLY_FILLED -> FILLED
ACKED / PARTIALLY_FILLED -> CANCEL_PENDING -> CANCELLED
ACKED / PARTIALLY_FILLED -> MODIFY_PENDING -> ACKED
ACKED -> EXPIRED
```

## 07 Risk-control specification
- Fixed by owner: 2% stop per trade, +10% daily profit lockout, 8 orders per second hard cap, no leverage by default.
- Proposed, awaiting approval: global daily loss 3% of capital; per-strategy daily loss 1.5%; maximum order value 25% of equity; gross exposure 100% of equity; 3 open positions; 6 open orders; operating rate 6 per second.
- All limits are checked before every order. AI engine orders get no exemption.
- A daily-limit breach halts new risk and cancels open orders.

## 08 Demo-exchange specification
- Limit order book, price-time priority, market / limit / stop / cancel / modify, partial fills.
- Configurable spread, slippage, fees, taxes, margin, latency.
- Failure injection: rejects, disconnects, rate limits, gaps, duplicates, out-of-order data, outages.
- Deterministic with a seed. Every order, fill and report is labelled SIMULATION.

## 09 Backtesting methodology
- In-sample, out-of-sample, walk-forward, stress periods, fee and slippage sensitivity, worst-case fills, several market regimes.
- Free data is daily bars, so the order book is synthetic and only daily-bar strategies can be validated until a broker supplies 1-minute data.

## 10 Latency-measurement plan
- Monotonic clock stamps at each hop; histograms with p50, p90, p95, p99, p99.9, max.
- Numbers from GitHub runners and browsers are functional checks, not trading-node latency. Every number is reported with its machine.

## 11 Broker-integration checklist (Stage 2 onward)
- Broker choice, official API documentation, test environment, static IP rule, order-rate limits, algo-order rules, historical-data lookback, fees. **[must verify]**

## 12 Terminal specification
- One-screen layout, Ocean theme, light and dark modes.
- Terminal: watchlist, book, order ticket, blotter, positions, adverse-media panel (video slot and headlines), alerts, news tape.
- Connector: selected company in the middle, unlisted companies left, listed companies right with NSE and BSE codes; click opens price history, financials, corporate actions, shareholding.
- AI Engine: open connector slot for GRIT, proposed signal format, source list, backtest and latency results.
- Account: demo wallet with funding, day P&L curve, day summary, full order history, ledger.
- Risk: limits, strategy router, exposure by instrument, risk events, emergency controls.
- Admin: users and roles, system mode, audit trail. Live mode off and locked.

## 13 Testing plan
- Unit, integration, property-based (book invariants), failure-mode, deterministic replay, sanitizers and static analysis in CI.
- Results are published as they are, including failures.

## 14 Security plan
- No keys in the repo. GRIT and news keys go in environment secrets.
- The repo is public, so code, data and results are visible to anyone.
- Before Stage 2: private repo, real authentication, secrets manager.

## 15 Deployment plan
- Claude pushes to the repo; Actions builds, tests and runs backtests. No GPU needed for the MVP.
- Live news and video need a small server or scheduled job, because a static page cannot hold keys. **[must verify]**

## 16 First MVP scope
- Simulation instruments: NIFTY, a blue-chip NSE basket, MCX commodities.
- Strategies: momentum and mean reversion.
- AI engine slot with stub; media panel; connector with history view.
- First live deployment later remains one strategy on one instrument, chosen from the validation report.

## 17 Assumptions that need verification
- NSE and MCX daily files can be downloaded from GitHub runners and stored in a public repo under the exchanges' terms.
- Lot size and margin of NIFTY and MCX contracts against the capital. Cash equities trade in single shares.
- INR value of the capital at the current rate; current fee and tax schedule per segment.
- Terms of news feeds, video embeds and exchange filing access.
- Coverage of free ownership data (GLEIF, Wikidata, annual reports) for Indian companies. No free supplier or customer dataset was found.
- Reliance relationships and codes shown in the preview are unverified samples.

## 18 Not done without owner approval
- Writing production code before the plan and preview are approved.
- Connecting to any broker API or sending any real order.
- Accepting or moving real money.
- Changing approved risk limits. Skipping or merging a stage.
- Claiming performance without measurements or profitability without out-of-sample results.
