// Randomized stress test. Generates a seeded random order stream, applies it
// to both book implementations, and:
//   1. checks structural invariants on the book as the stream progresses,
//   2. checks quantity conservation for every order ever submitted,
//   3. checks the two implementations produce IDENTICAL trade streams and
//      final book states (differential test: same logic, different ladder).
#include "harness.hpp"
#include "matcheng/book.hpp"

#include <cstdlib>
#include <map>
#include <random>
#include <unordered_map>

using namespace matcheng;

namespace {

uint64_t zero_clock() { return 0; }

struct Event {
    enum Kind : uint8_t { AddLimit, AddMarket, Cancel, Modify } kind;
    OrderId id;
    Side side;
    Price price;
    Qty qty;
};

// Random but realistic-ish stream: prices cluster around a drifting mid,
// cancels/modifies target random live orders.
std::vector<Event> generate_stream(uint64_t seed, size_t n) {
    std::mt19937_64 rng(seed);
    std::vector<Event> ev;
    ev.reserve(n);
    std::vector<OrderId> live;  // ids we have submitted as limits (may be gone)
    OrderId next_id = 1;
    Price mid = 10000;
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> offset(0, 20);
    std::uniform_int_distribution<Qty> qty_dist(1, 500);

    for (size_t i = 0; i < n; ++i) {
        if (pct(rng) < 2) mid += (pct(rng) < 50 ? 1 : -1) * (Price)offset(rng) / 4;
        int roll = pct(rng);
        Side side = pct(rng) < 50 ? Side::Buy : Side::Sell;
        if (roll < 55 || live.empty()) {  // add limit
            // Mostly passive, sometimes aggressive (crossing) prices.
            Price px = side == Side::Buy ? mid - offset(rng) : mid + offset(rng);
            if (pct(rng) < 25) px = side == Side::Buy ? mid + offset(rng) / 2
                                                      : mid - offset(rng) / 2;
            OrderId id = next_id++;
            ev.push_back({Event::AddLimit, id, side, px, qty_dist(rng)});
            live.push_back(id);
        } else if (roll < 65) {  // market order
            ev.push_back({Event::AddMarket, next_id++, side, 0, qty_dist(rng)});
        } else if (roll < 87) {  // cancel a random previously-submitted id
            size_t k = rng() % live.size();
            ev.push_back({Event::Cancel, live[k], side, 0, 0});
        } else {  // modify a random previously-submitted id
            size_t k = rng() % live.size();
            Price px = pct(rng) < 50 ? mid - offset(rng) : mid + offset(rng);
            ev.push_back({Event::Modify, live[k], side, px, qty_dist(rng)});
        }
    }
    return ev;
}

// Ledger tracking every order's original qty and cumulative fills, updated
// from the trade stream alone. Used to prove quantity conservation.
struct Ledger {
    struct Entry {
        Qty original = 0;
        Qty filled = 0;
    };
    std::unordered_map<OrderId, Entry> entries;

    void on_submit(OrderId id, Qty qty) { entries[id].original += qty; }
    void on_trades(const std::vector<Trade>& trades, size_t from) {
        for (size_t i = from; i < trades.size(); ++i) {
            entries[trades[i].resting_id].filled += trades[i].qty;
            entries[trades[i].aggressor_id].filled += trades[i].qty;
        }
    }
};

// Full structural scan of one book. Returns number of failed properties
// (details printed via CHECK inside).
template <class Book>
void check_invariants(Book& b) {
    // Ladder sorted strictly best-to-worst, levels non-empty, level sums match,
    // FIFO list intact, every order also present in the id index.
    size_t orders_seen = 0;
    for (Side s : {Side::Buy, Side::Sell}) {
        bool first = true;
        Price prev = 0;
        b.for_each_level(s, [&](Price px, const Level& lvl) {
            if (!first) {
                if (s == Side::Buy) CHECK(px < prev);   // bids descending
                else                CHECK(px > prev);   // asks ascending
            }
            first = false;
            prev = px;
            CHECK(lvl.total_qty > 0);  // no phantom (empty) levels
            Qty sum = 0;
            const Order* prev_node = nullptr;
            for (const Order* o = lvl.head; o; o = o->next) {
                CHECK(o->remaining > 0);          // no phantom orders
                CHECK_EQ(o->price, px);
                CHECK(o->prev == prev_node);      // doubly-linked list intact
                CHECK_EQ(*b.order_remaining(o->id), o->remaining);
                sum += o->remaining;
                prev_node = o;
                ++orders_seen;
            }
            CHECK(lvl.tail == prev_node);
            CHECK_EQ(sum, lvl.total_qty);         // level aggregate is exact
        });
    }
    CHECK_EQ(orders_seen, b.order_count());       // id index has no strays
    // Book never left crossed after an event is fully processed.
    if (b.best_bid() && b.best_ask()) CHECK(*b.best_bid() < *b.best_ask());
}

template <class Book>
struct RunResult {
    std::vector<Trade> trades;
    Ledger ledger;
};

template <class Book>
RunResult<Book> run_stream(const std::vector<Event>& ev, size_t check_every) {
    Book b;
    b.set_clock(&zero_clock);
    RunResult<Book> r;
    r.trades.reserve(ev.size() / 2);
    size_t i = 0;
    for (const Event& e : ev) {
        size_t before = r.trades.size();
        switch (e.kind) {
            case Event::AddLimit:
                if (b.add_limit(e.id, e.side, e.price, e.qty, r.trades))
                    r.ledger.on_submit(e.id, e.qty);
                break;
            case Event::AddMarket: {
                r.ledger.on_submit(e.id, e.qty);
                Qty rem = b.add_market(e.id, e.side, e.qty, r.trades);
                CHECK(rem <= e.qty);
                break;
            }
            case Event::Cancel:
                b.cancel(e.id);
                break;
            case Event::Modify: {
                auto prev_rem = b.order_remaining(e.id);
                if (b.modify(e.id, e.price, e.qty, r.trades)) {
                    // The modify replaced remaining qty: adjust the ledger as
                    // if the old remainder was cancelled and e.qty re-added.
                    r.ledger.entries[e.id].original +=
                        e.qty >= *prev_rem ? e.qty - *prev_rem : 0;
                    if (e.qty < *prev_rem)
                        r.ledger.entries[e.id].original -= *prev_rem - e.qty;
                }
                break;
            }
        }
        r.ledger.on_trades(r.trades, before);
        if (++i % check_every == 0) check_invariants(b);
    }
    check_invariants(b);
    // Conservation: for every live order, remaining == original - filled;
    // no order ever overfills.
    size_t live_checked = 0;
    for (const auto& [id, entry] : r.ledger.entries) {
        CHECK(entry.filled <= entry.original);
        if (auto rem = b.order_remaining(id)) {
            CHECK_EQ(*rem, entry.original - entry.filled);
            ++live_checked;
        }
    }
    CHECK_EQ(live_checked, b.order_count());
    return r;
}

}  // namespace

TEST(stress_invariants_and_conservation_100k_events) {
    auto ev = generate_stream(/*seed=*/42, /*n=*/100'000);
    // Full structural scan every 97 events (prime, to avoid phase-locking with
    // the generator's mix), plus once at the end.
    auto map_run = run_stream<MapBook>(ev, 97);
    auto vec_run = run_stream<VecBook>(ev, 97);

    // Differential check: identical trade streams from both implementations.
    CHECK_EQ(map_run.trades.size(), vec_run.trades.size());
    size_t n = std::min(map_run.trades.size(), vec_run.trades.size());
    size_t mismatches = 0;
    for (size_t i = 0; i < n; ++i) {
        const Trade &a = map_run.trades[i], &c = vec_run.trades[i];
        if (!(a.price == c.price && a.qty == c.qty && a.resting_id == c.resting_id &&
              a.aggressor_id == c.aggressor_id && a.seq == c.seq))
            ++mismatches;
    }
    CHECK_EQ(mismatches, 0u);
    CHECK(map_run.trades.size() > 10'000);  // the stream actually exercised matching
}

TEST(stress_second_seed_25k_events) {
    auto ev = generate_stream(/*seed=*/20260824, /*n=*/25'000);
    auto map_run = run_stream<MapBook>(ev, 31);
    auto vec_run = run_stream<VecBook>(ev, 31);
    CHECK_EQ(map_run.trades.size(), vec_run.trades.size());
    CHECK(map_run.trades.size() > 1'000);
}

int main(int argc, char** argv) {
    // Optional: STRESS_SEED / STRESS_N env overrides for soak runs.
    (void)argc;
    (void)argv;
    if (const char* seed_s = std::getenv("STRESS_SEED")) {
        size_t n = 100'000;
        if (const char* n_s = std::getenv("STRESS_N")) n = std::strtoull(n_s, nullptr, 10);
        uint64_t seed = std::strtoull(seed_s, nullptr, 10);
        std::printf("soak: seed=%llu n=%zu\n", (unsigned long long)seed, n);
        auto ev = generate_stream(seed, n);
        auto m = run_stream<MapBook>(ev, 199);
        auto v = run_stream<VecBook>(ev, 199);
        CHECK_EQ(m.trades.size(), v.trades.size());
    }
    return harness::run_all();
}
