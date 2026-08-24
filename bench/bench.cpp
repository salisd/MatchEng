// Benchmark driver. Methodology (all numbers in the README come from here):
//
//   Stream: pre-generated before any timing. The generator runs a shadow book
//   so cancel/modify events target orders that are actually resting at that
//   point in the stream, and the event mix is steered so the live-order
//   population hovers near a configured target (steady state, not growth).
//
//   Two book-shape scenarios:
//     shallow: ~100 live orders, prices within 15 ticks of touch — the
//              typical liquid-instrument regime.
//     deep:    ~5000 live orders spread across hundreds of price levels —
//              stresses the ladder structure itself.
//
//   Throughput: apply the whole stream, one wall-clock read per rep.
//   5 reps on a fresh book each; median reported.
//
//   Latency: separate run, per-event clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
//   pairs. Samples INCLUDE timer overhead (measured and printed, not
//   subtracted). Timer tick resolution is printed from mach_timebase_info.
//
//   Memory: long churn run; phys_footprint (macOS task_info) reported as a
//   DELTA from a baseline taken after stream generation (the pre-generated
//   stream itself dominates absolute footprint), sampled at intervals to
//   show a plateau, not unbounded growth.
//
//   The book's trade-timestamp clock is stubbed to zero during benchmarks so
//   we measure matching, not clock syscalls; sequence numbers still run.
#include "matcheng/book.hpp"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace matcheng;

namespace {

uint64_t zero_clock() { return 0; }
uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

struct Event {
    enum Kind : uint8_t { AddLimit, AddMarket, Cancel, Modify } kind;
    OrderId id;
    Side side;
    Price price;
    Qty qty;
};

const char* kind_name(int k) {
    static const char* names[] = {"add_limit", "market", "cancel", "modify"};
    return names[k];
}

struct GenConfig {
    const char* name;
    int max_depth;        // passive prices land within [1, max_depth] ticks of touch
    int marketable_pct;   // share of adds priced to cross
    size_t target_live;   // generator steers the mix to hover near this population
};

struct StreamStats {
    size_t counts[4] = {0, 0, 0, 0};
    double mean_levels_per_side = 0;
    double mean_live_orders = 0;
    size_t trades = 0;
};

std::vector<Event> generate_stream(uint64_t seed, size_t n, const GenConfig& cfg,
                                   StreamStats& stats) {
    std::mt19937_64 rng(seed);
    std::vector<Event> ev;
    ev.reserve(n);
    MapBook shadow;
    shadow.set_clock(&zero_clock);
    std::vector<Trade> trades;
    std::vector<OrderId> live;
    OrderId next_id = 1;
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> depth(1, cfg.max_depth);
    std::uniform_int_distribution<Qty> qty_dist(1, 100);
    double lvl_accum = 0, live_accum = 0;
    size_t samples = 0;

    auto pick_live = [&]() -> OrderId {
        while (!live.empty()) {
            size_t k = rng() % live.size();
            OrderId id = live[k];
            live[k] = live.back();
            live.pop_back();
            if (shadow.order_remaining(id)) return id;  // still resting
        }
        return 0;
    };

    for (size_t i = 0; i < n; ++i) {
        trades.clear();
        // Below-target mix favors adds; above-target favors cancels, so the
        // live population oscillates around cfg.target_live.
        bool below = shadow.order_count() < cfg.target_live;
        int add_pct = below ? 65 : 40;
        int cancel_hi = below ? add_pct + 20 : add_pct + 45;  // both end at 85
        int roll = pct(rng);
        Side side = pct(rng) < 50 ? Side::Buy : Side::Sell;
        Price bid = shadow.best_bid().value_or(10000);
        Price ask = shadow.best_ask().value_or(bid + 2);
        Event e{};
        if (roll < add_pct || live.empty()) {
            e = {Event::AddLimit, next_id++, side, 0, qty_dist(rng)};
            if (pct(rng) < cfg.marketable_pct)
                e.price = side == Side::Buy ? ask : bid;
            else
                e.price = side == Side::Buy ? bid + 1 - depth(rng) : ask - 1 + depth(rng);
            shadow.add_limit(e.id, e.side, e.price, e.qty, trades);
            if (shadow.order_remaining(e.id)) live.push_back(e.id);
        } else if (roll < cancel_hi) {
            OrderId id = pick_live();
            if (id == 0) { --i; continue; }
            e = {Event::Cancel, id, side, 0, 0};
            shadow.cancel(id);
        } else if (roll < 93) {
            OrderId id = pick_live();
            if (id == 0) { --i; continue; }
            e = {Event::Modify, id, side,
                 side == Side::Buy ? bid + 1 - depth(rng) : ask - 1 + depth(rng),
                 qty_dist(rng)};
            shadow.modify(e.id, e.price, e.qty, trades);
            if (shadow.order_remaining(e.id)) live.push_back(e.id);
        } else {
            e = {Event::AddMarket, next_id++, side, 0, qty_dist(rng) / 4 + 1};
            shadow.add_market(e.id, e.side, e.qty, trades);
        }
        ev.push_back(e);
        ++stats.counts[e.kind];
        stats.trades += trades.size();
        if (i % 1024 == 0) {
            lvl_accum += (shadow.level_count(Side::Buy) + shadow.level_count(Side::Sell)) / 2.0;
            live_accum += (double)shadow.order_count();
            ++samples;
        }
    }
    stats.mean_levels_per_side = lvl_accum / (double)samples;
    stats.mean_live_orders = live_accum / (double)samples;
    return ev;
}

template <class Book>
size_t apply(Book& b, const Event& e, std::vector<Trade>& trades) {
    switch (e.kind) {
        case Event::AddLimit: b.add_limit(e.id, e.side, e.price, e.qty, trades); break;
        case Event::AddMarket: b.add_market(e.id, e.side, e.qty, trades); break;
        case Event::Cancel: b.cancel(e.id); break;
        case Event::Modify: b.modify(e.id, e.price, e.qty, trades); break;
    }
    return trades.size();
}

template <class Book>
void bench_throughput(const std::vector<Event>& ev) {
    const int reps = 5;
    std::vector<double> rates;
    size_t trades_total = 0;
    for (int r = 0; r < reps; ++r) {
        Book b;
        b.set_clock(&zero_clock);
        std::vector<Trade> trades;
        trades.reserve(4096);
        trades_total = 0;
        uint64_t t0 = now_ns();
        for (const Event& e : ev) {
            trades.clear();
            trades_total += apply(b, e, trades);
        }
        uint64_t t1 = now_ns();
        rates.push_back((double)ev.size() / ((double)(t1 - t0) / 1e9));
    }
    std::sort(rates.begin(), rates.end());
    std::printf("  throughput: %.2fM events/s median (min %.2fM, max %.2fM over %d reps), "
                "%zu trades/rep\n",
                rates[reps / 2] / 1e6, rates.front() / 1e6, rates.back() / 1e6, reps,
                trades_total);
}

void print_percentiles(const char* label, std::vector<uint32_t>& s) {
    if (s.empty()) return;
    std::sort(s.begin(), s.end());
    auto pc = [&](double p) { return s[(size_t)((double)(s.size() - 1) * p)]; };
    std::printf("  %-10s n=%-9zu p50=%4uns  p95=%4uns  p99=%4uns  p99.9=%5uns  max=%uns\n",
                label, s.size(), pc(0.50), pc(0.95), pc(0.99), pc(0.999), s.back());
}

template <class Book>
void bench_latency(const std::vector<Event>& ev) {
    Book b;
    b.set_clock(&zero_clock);
    std::vector<Trade> trades;
    trades.reserve(4096);
    std::vector<uint32_t> samples[4];
    for (auto& s : samples) s.reserve(ev.size());
    for (const Event& e : ev) {
        trades.clear();
        uint64_t t0 = now_ns();
        apply(b, e, trades);
        uint64_t t1 = now_ns();
        samples[e.kind].push_back((uint32_t)(t1 - t0));
    }
    std::vector<uint32_t> all;
    all.reserve(ev.size());
    for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());
    print_percentiles("all", all);
    for (int k = 0; k < 4; ++k) print_percentiles(kind_name(k), samples[k]);
}

size_t phys_footprint_bytes() {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    return (size_t)info.phys_footprint;
}

template <class Book>
void bench_memory(uint64_t seed, size_t n, size_t sample_every, const GenConfig& cfg) {
    StreamStats stats;
    auto ev = generate_stream(seed, n, cfg, stats);
    double base_mb = (double)phys_footprint_bytes() / (1024.0 * 1024.0);
    std::printf("  baseline after stream generation: %.1f MB phys_footprint "
                "(the %zu-event stream itself dominates this)\n", base_mb, n);
    Book b;
    b.set_clock(&zero_clock);
    std::vector<Trade> trades;
    trades.reserve(4096);
    std::printf("  %-12s %-12s %s\n", "events", "live_orders", "footprint_delta");
    for (size_t i = 0; i < ev.size(); ++i) {
        trades.clear();
        apply(b, ev[i], trades);
        if ((i + 1) % sample_every == 0)
            std::printf("  %-12zu %-12zu %+.1f MB\n", i + 1, b.order_count(),
                        (double)phys_footprint_bytes() / (1024.0 * 1024.0) - base_mb);
    }
}

void run_scenario(const GenConfig& cfg, size_t n) {
    StreamStats stats;
    std::printf("=== scenario: %s (target ~%zu live orders) ===\n", cfg.name, cfg.target_live);
    std::printf("generating %zu events (seed 42)...\n", n);
    auto ev = generate_stream(42, n, cfg, stats);
    std::printf("mix: add_limit=%zu market=%zu cancel=%zu modify=%zu | trades=%zu\n",
                stats.counts[0], stats.counts[1], stats.counts[2], stats.counts[3],
                stats.trades);
    std::printf("book shape during stream: mean %.1f price levels/side, mean %.0f live orders\n\n",
                stats.mean_levels_per_side, stats.mean_live_orders);
    std::printf("MapBook (std::map ladder):\n");
    bench_throughput<MapBook>(ev);
    bench_latency<MapBook>(ev);
    std::printf("\nVecBook (sorted-vector ladder):\n");
    bench_throughput<VecBook>(ev);
    bench_latency<VecBook>(ev);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    size_t n = 2'000'000;
    size_t mem_n = 10'000'000;
    bool do_mem = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-mem") == 0) do_mem = false;
        else n = std::strtoull(argv[i], nullptr, 10);
    }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    std::vector<uint32_t> clk;
    clk.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        uint64_t a = now_ns(), b = now_ns();
        clk.push_back((uint32_t)(b - a));
    }
    std::sort(clk.begin(), clk.end());
    std::printf("timer: tick=%u/%u ns (%.2fns resolution), back-to-back read overhead median=%uns\n\n",
                tb.numer, tb.denom, (double)tb.numer / (double)tb.denom, clk[clk.size() / 2]);

    GenConfig shallow{"shallow", 15, 15, 100};
    GenConfig deep{"deep", 300, 10, 5000};
    run_scenario(shallow, n);
    run_scenario(deep, n);

    if (do_mem) {
        std::printf("=== memory under sustained load: VecBook, shallow, %zu events (seed 43) ===\n",
                    mem_n);
        bench_memory<VecBook>(43, mem_n, mem_n / 20, shallow);
    }
    return 0;
}
