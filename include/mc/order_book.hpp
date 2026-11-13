// Limit order book with price-time priority matching.
// Orders live in a preallocated pool and are linked per price level (FIFO).
// Note: price levels use std::map, which allocates when a new level appears.
#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>
#include "mc/types.hpp"

namespace mc {

struct Trade {
  OrderId maker = 0, taker = 0;
  Price px = 0;
  Qty qty = 0;
};

class OrderBook {
 public:
  explicit OrderBook(std::size_t capacity = 4096) {
    pool_.reserve(capacity);
    idx_.reserve(capacity);
  }

  bool has(OrderId id) const { return idx_.find(id) != idx_.end(); }
  std::size_t size() const { return idx_.size(); }
  Price best_bid() const { return bids_.empty() ? 0 : bids_.begin()->first; }
  Price best_ask() const { return asks_.empty() ? 0 : asks_.begin()->first; }
  Qty open_qty(OrderId id) const {
    auto it = idx_.find(id);
    return it == idx_.end() ? 0 : pool_[static_cast<std::size_t>(it->second)].qty;
  }
  Qty qty_at(Side s, Price px) const {
    if (s == Side::Buy) { auto it = bids_.find(px); return it == bids_.end() ? 0 : it->second.total; }
    auto it = asks_.find(px); return it == asks_.end() ? 0 : it->second.total;
  }
  // Visits the best `n` price levels of one side: f(price, total_qty).
  template <class F>
  void levels(Side s, int n, F&& f) const {
    if (s == Side::Buy) { for (auto it = bids_.begin(); it != bids_.end() && n-- > 0; ++it) f(it->first, it->second.total); }
    else { for (auto it = asks_.begin(); it != asks_.end() && n-- > 0; ++it) f(it->first, it->second.total); }
  }
  void clear() {
    pool_.clear(); free_.clear(); idx_.clear(); bids_.clear(); asks_.clear();
  }

  // Limit order: matches what crosses, rests the remainder. Returns resting qty, or -1 if rejected.
  template <class F>
  Qty add_limit(OrderId id, Side s, Price px, Qty qty, F&& on_trade) {
    if (qty <= 0 || px <= 0 || has(id)) return -1;
    Qty rem = (s == Side::Buy) ? sweep(asks_, id, qty, px, true, on_trade)
                               : sweep(bids_, id, qty, px, true, on_trade);
    if (rem > 0) rest(id, s, px, rem);
    return rem;
  }

  // Market order: matches against the book; never rests. Returns unfilled qty, or -1 if rejected.
  template <class F>
  Qty add_market(OrderId id, Side s, Qty qty, F&& on_trade) {
    if (qty <= 0) return -1;
    return (s == Side::Buy) ? sweep(asks_, id, qty, 0, false, on_trade)
                            : sweep(bids_, id, qty, 0, false, on_trade);
  }

  bool cancel(OrderId id) {
    auto it = idx_.find(id);
    if (it == idx_.end()) return false;
    unlink(it->second);
    idx_.erase(it);
    return true;
  }

  // Modify: a quantity decrease at the same price keeps time priority.
  // Any price change or quantity increase is cancel-and-replace and loses priority.
  template <class F>
  bool modify(OrderId id, Price px, Qty qty, F&& on_trade) {
    auto it = idx_.find(id);
    if (it == idx_.end() || qty <= 0 || px <= 0) return false;
    Node& n = pool_[static_cast<std::size_t>(it->second)];
    if (px == n.px && qty <= n.qty) {
      level_total(n.side, n.px) -= (n.qty - qty);
      n.qty = qty;
      return true;
    }
    const Side s = n.side;
    cancel(id);
    add_limit(id, s, px, qty, on_trade);
    return true;
  }

 private:
  struct Node {
    OrderId id; Price px; Qty qty; Side side; std::int32_t prev, next;
  };
  struct Level { std::int32_t head = -1, tail = -1; Qty total = 0; };
  using Bids = std::map<Price, Level, std::greater<Price>>;
  using Asks = std::map<Price, Level>;

  Qty& level_total(Side s, Price px) { return s == Side::Buy ? bids_[px].total : asks_[px].total; }

  void rest(OrderId id, Side s, Price px, Qty qty) {
    std::int32_t i;
    if (!free_.empty()) { i = free_.back(); free_.pop_back(); }
    else { i = static_cast<std::int32_t>(pool_.size()); pool_.push_back(Node{}); }
    Level& lv = (s == Side::Buy) ? bids_[px] : asks_[px];
    pool_[static_cast<std::size_t>(i)] = Node{id, px, qty, s, lv.tail, -1};
    if (lv.tail >= 0) pool_[static_cast<std::size_t>(lv.tail)].next = i; else lv.head = i;
    lv.tail = i;
    lv.total += qty;
    idx_[id] = i;
  }

  void unlink(std::int32_t i) {
    const Node n = pool_[static_cast<std::size_t>(i)];
    if (n.side == Side::Buy) unlink_from(bids_, n, i); else unlink_from(asks_, n, i);
    free_.push_back(i);
  }
  template <class M>
  void unlink_from(M& m, const Node& n, std::int32_t) {
    auto it = m.find(n.px);
    Level& lv = it->second;
    if (n.prev >= 0) pool_[static_cast<std::size_t>(n.prev)].next = n.next; else lv.head = n.next;
    if (n.next >= 0) pool_[static_cast<std::size_t>(n.next)].prev = n.prev; else lv.tail = n.prev;
    lv.total -= n.qty;
    if (lv.head < 0) m.erase(it);
  }

  // Match `qty` of taker against the opposite side `m`, best price first, FIFO within a level.
  template <class M, class F>
  Qty sweep(M& m, OrderId taker, Qty qty, Price limit, bool has_limit, F&& on_trade) {
    const bool taker_buys = (static_cast<void*>(&m) == static_cast<void*>(&asks_));
    while (qty > 0 && !m.empty()) {
      auto it = m.begin();
      const Price lpx = it->first;
      if (has_limit && (taker_buys ? lpx > limit : lpx < limit)) break;
      Level& lv = it->second;
      while (qty > 0 && lv.head >= 0) {
        const std::int32_t i = lv.head;
        Node& n = pool_[static_cast<std::size_t>(i)];
        const Qty q = qty < n.qty ? qty : n.qty;
        on_trade(Trade{n.id, taker, lpx, q});
        n.qty -= q; lv.total -= q; qty -= q;
        if (n.qty == 0) {
          lv.head = n.next;
          if (n.next >= 0) pool_[static_cast<std::size_t>(n.next)].prev = -1; else lv.tail = -1;
          idx_.erase(n.id);
          free_.push_back(i);
        }
      }
      if (lv.head < 0) m.erase(it);
    }
    return qty;
  }

  std::vector<Node> pool_;
  std::vector<std::int32_t> free_;
  std::unordered_map<OrderId, std::int32_t> idx_;
  Bids bids_;
  Asks asks_;
};

}  // namespace mc
