// Two interchangeable price-ladder implementations (one instance per side).
//
// A ladder maps price -> Level and can report the best (most aggressive)
// level in that side's priority order: highest price for bids, lowest for
// asks. The book template is instantiated with one of these, so both share
// the exact same matching logic and can be differential-tested and
// benchmarked against each other.
//
// MapLadder:  std::map<Price, Level>. O(log L) find/insert/erase where L is
//             the number of live price levels. Node-based: every level is a
//             separate heap allocation, traversal chases pointers.
// VecLadder:  flat std::vector of (price, Level*) kept sorted worst-to-best,
//             so best() is back() in O(1). Binary search O(log L) to locate,
//             but insert/erase memmove O(L). Levels come from a pool, and in
//             practice L is small (tens to low hundreds) and the hot path
//             touches the cache-friendly tail of the vector.
#pragma once
#include "types.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace matcheng {

template <bool IsBid>
class MapLadder {
    using Cmp = std::conditional_t<IsBid, std::greater<Price>, std::less<Price>>;

public:
    static constexpr const char* name() { return "map"; }

    Level* best() { return m_.empty() ? nullptr : &m_.begin()->second; }

    Level* get_or_create(Price px) {
        auto [it, inserted] = m_.try_emplace(px);
        if (inserted) it->second.price = px;
        return &it->second;
    }

    void erase(Price px) { m_.erase(px); }

    size_t size() const { return m_.size(); }

    // Visit levels best-to-worst.
    template <class F>
    void for_each(F&& f) const {
        for (const auto& [px, lvl] : m_) f(px, lvl);
    }

private:
    std::map<Price, Level, Cmp> m_;
};

template <bool IsBid>
class VecLadder {
public:
    static constexpr const char* name() { return "vec"; }

    Level* best() { return v_.empty() ? nullptr : v_.back().second; }

    Level* get_or_create(Price px) {
        auto it = pos(px);
        if (it != v_.end() && it->first == px) return it->second;
        Level* lvl = pool_.alloc();
        *lvl = Level{};
        lvl->price = px;
        v_.insert(it, {px, lvl});
        return lvl;
    }

    void erase(Price px) {
        auto it = pos(px);
        pool_.release(it->second);
        v_.erase(it);
    }

    size_t size() const { return v_.size(); }

    template <class F>
    void for_each(F&& f) const {  // best-to-worst = back-to-front
        for (auto it = v_.rbegin(); it != v_.rend(); ++it) f(it->first, *it->second);
    }

private:
    // Sorted worst-to-best: bids ascending by price, asks descending.
    static bool worse(Price a, Price b) { return IsBid ? a < b : a > b; }

    std::vector<std::pair<Price, Level*>>::iterator pos(Price px) {
        return std::lower_bound(
            v_.begin(), v_.end(), px,
            [](const std::pair<Price, Level*>& e, Price p) { return worse(e.first, p); });
    }

    std::vector<std::pair<Price, Level*>> v_;
    Pool<Level> pool_;
};

}  // namespace matcheng
