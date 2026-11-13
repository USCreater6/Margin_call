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
4. **Verified tick sizes.** Sim uses 0.05 for everything (labelled not verified). NSE publishes
   price-band-based tick sizes; must be loaded per symbol.
5. **Corporate actions beyond splits/bonuses.** Demergers (e.g. Reliance → Jio Financial 2023),
   rights issues and dividends are not adjusted. Needed for total-return accuracy.
6. **Strategies inside the terminal session.** Today they run in backtest only; terminal orders
   come from the ticket or the random stub.

## B. Platform hardening (before Stage 2)

7. **Persistence and audit trail.** Session state, orders and fills are in memory; a restart
   loses them. Need crash-safe journal + replay on start.
8. **Stop and modify from the terminal UI** (engine supports them; UI does not expose them).
9. **Security.** Repo is public; server has no login. Before any broker key exists:
   private repo or secrets only in Codespaces secrets, real login, HTTPS only.
10. **Codespaces cost/quota.** Free hours and storage on your GitHub plan must be checked
    (needs you: GitHub billing page).

## C. Stage 2 — live data + simulated orders (needs your approval to start)

11. **Broker choice** (needs you). Then read that broker's API docs for: market data feed,
    order types, rate limits, sandbox availability. Nothing assumed until read.
12. **Intraday data** comes from the broker feed; free sources give end-of-day only.
13. **MCX commodities.** MCX blocks automated downloads (HTTP 403). Needs a broker feed or a paid source.
14. **Latency measurement on the real node** (current numbers are from cloud test machines).

## D. Information panels

15. **News:** second source (GDELT is rate-limited from GitHub runners); keyword tagging is
    simple — improve with a vetted word list or a model in the AI-engine slot.
16. **Business TV:** 14 channels verified by YouTube channel feed. Whether each one allows
    embedding can only be confirmed in your browser; "Open on YouTube" is the fallback.
17. **Fundamentals:** currently NIFTY 50 then NIFTY 500, quarters from NSE filings.
    Extend to all listed companies and older quarters.
18. **Connector:** annual-report related-party lists file (tools/connector_annual_reports.csv) is empty.

## E. AI engine

19. **Engine slot only.** Connect your model when you decide; it stays out of the live order path
    (spec rule). Interface and evaluation harness to be agreed then.

## Needs you (summary)

- Delete the old Codespace and create a new one once (auto-update works from then on), send a screenshot.
- Broker choice (items 3, 11).
- Approval before Stage 2.
- GitHub plan limits (item 10).
