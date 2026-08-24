// Price-time-priority limit order book, templated on the price-ladder
// structure (see ladders.hpp). MapBook and VecBook run identical matching
// logic and differ only in how price levels are indexed.
//
// Semantics:
//   - add_limit: matches against the opposite side while it crosses, then
//     rests any remainder. Rejects qty == 0 and ids that are still resting.
//   - add_market: matches until filled or the opposite side is empty;
//     returns the unfilled remainder, which never rests.
//   - cancel: O(1) — hash lookup, intrusive unlink; empty levels are erased.
//   - modify: same price + qty <= remaining -> in-place decrease, queue
//     position KEPT. Price change or qty increase -> cancel + new arrival
//     under the same id (loses time priority, may match immediately).
//   - Trades always execute at the resting order's price.
#pragma once
#include "ladders.hpp"
#include "types.hpp"

#include <optional>
#include <unordered_map>

namespace matcheng {

template <template <bool> class LadderT>
class OrderBookT {
public:
    using ClockFn = uint64_t (*)();

    static const char* ladder_name() { return LadderT<true>::name(); }

    // --- Mutations ---------------------------------------------------------

    bool add_limit(OrderId id, Side side, Price px, Qty qty, std::vector<Trade>& out) {
        if (qty == 0 || orders_.find(id) != orders_.end()) return false;
        uint64_t ts = clock_();
        Qty rem = side == Side::Buy ? match_against(asks_, id, px, false, qty, ts, out)
                                    : match_against(bids_, id, px, false, qty, ts, out);
        if (rem > 0) rest(id, side, px, rem);
        return true;
    }

    Qty add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& out) {
        if (qty == 0) return 0;
        uint64_t ts = clock_();
        return side == Side::Buy ? match_against(asks_, id, 0, true, qty, ts, out)
                                 : match_against(bids_, id, 0, true, qty, ts, out);
    }

    bool cancel(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        remove(it);
        return true;
    }

    bool modify(OrderId id, Price new_px, Qty new_qty, std::vector<Trade>& out) {
        auto it = orders_.find(id);
        if (it == orders_.end() || new_qty == 0) return false;
        Order* o = it->second;
        if (new_px == o->price && new_qty <= o->remaining) {
            Qty delta = o->remaining - new_qty;
            o->remaining = new_qty;
            o->level->total_qty -= delta;
            return true;
        }
        Side side = o->side;
        remove(it);
        add_limit(id, side, new_px, new_qty, out);
        return true;
    }

    // --- Queries ------------------------------------------------------------

    std::optional<Price> best_bid() { Level* l = bids_.best(); return l ? std::optional(l->price) : std::nullopt; }
    std::optional<Price> best_ask() { Level* l = asks_.best(); return l ? std::optional(l->price) : std::nullopt; }

    Qty qty_at(Side side, Price px) const {
        Qty q = 0;
        for_each_level(side, [&](Price p, const Level& lvl) { if (p == px) q = lvl.total_qty; });
        return q;
    }

    std::optional<Qty> order_remaining(OrderId id) const {
        auto it = orders_.find(id);
        return it == orders_.end() ? std::nullopt : std::optional(it->second->remaining);
    }

    size_t order_count() const { return orders_.size(); }
    size_t level_count(Side side) const { return side == Side::Buy ? bids_.size() : asks_.size(); }

    template <class F>
    void for_each_level(Side side, F&& f) const {
        side == Side::Buy ? bids_.for_each(f) : asks_.for_each(f);
    }

    void set_clock(ClockFn fn) { clock_ = fn; }

private:
    template <class OppLadder>
    Qty match_against(OppLadder& opp, OrderId aggr_id, Price limit_px, bool is_market,
                      Qty qty, uint64_t ts, std::vector<Trade>& out) {
        // Aggressor is a buy iff the opposite ladder holds asks.
        constexpr bool buying = !std::decay_t<OppLadder>::is_bid;
        while (qty > 0) {
            Level* lvl = opp.best();
            if (!lvl) break;
            if (!is_market && (buying ? limit_px < lvl->price : limit_px > lvl->price)) break;
            while (qty > 0 && lvl->head) {
                Order* resting = lvl->head;
                Qty fill = qty < resting->remaining ? qty : resting->remaining;
                out.push_back({lvl->price, fill, resting->id, aggr_id, ++seq_, ts});
                resting->remaining -= fill;
                lvl->total_qty -= fill;
                qty -= fill;
                if (resting->remaining == 0) {
                    lvl->head = resting->next;
                    (resting->next ? resting->next->prev : lvl->tail) = nullptr;
                    orders_.erase(resting->id);
                    pool_.release(resting);
                }
            }
            if (lvl->total_qty == 0) opp.erase(lvl->price);
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price px, Qty qty) {
        Order* o = pool_.alloc();
        o->id = id;
        o->price = px;
        o->remaining = qty;
        o->side = side;
        Level* lvl = side == Side::Buy ? bids_.get_or_create(px) : asks_.get_or_create(px);
        lvl->push_back(o);
        orders_.emplace(id, o);
    }

    void remove(typename std::unordered_map<OrderId, Order*>::iterator it) {
        Order* o = it->second;
        Level* lvl = o->level;
        lvl->unlink(o);
        if (lvl->total_qty == 0) {
            if (o->side == Side::Buy) bids_.erase(lvl->price);
            else asks_.erase(lvl->price);
        }
        orders_.erase(it);
        pool_.release(o);
    }

    struct BidTag : LadderT<true>  { static constexpr bool is_bid = true; };
    struct AskTag : LadderT<false> { static constexpr bool is_bid = false; };

    BidTag bids_;
    AskTag asks_;
    std::unordered_map<OrderId, Order*> orders_;
    Pool<Order> pool_;
    uint64_t seq_ = 0;
    ClockFn clock_ = &wall_clock_ns;
};

using MapBook = OrderBookT<MapLadder>;
using VecBook = OrderBookT<VecLadder>;

}  // namespace matcheng
