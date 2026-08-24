// Hand-constructed matching scenarios, written BEFORE the engine implementation.
// Every scenario runs against both ladder implementations (MapBook and VecBook)
// via BOOK_TEST, so the two register as separate test cases.
//
// Conventions used throughout:
//   - Prices are integer ticks. Quantities are unsigned integers.
//   - add_limit / add_market / modify append Trade records to a caller vector.
//   - add_market returns the UNFILLED remainder (0 = fully filled).
//   - modify(id, px, qty): qty is the new *remaining* quantity. Same price and
//     qty <= current remaining -> in-place decrease, queue position kept.
//     Any price change or qty increase -> treated as cancel + new arrival
//     (loses time priority, may match immediately if it crosses).
//   - Trades execute at the RESTING order's price (price improvement goes to
//     the aggressor).
#include "harness.hpp"
#include "matcheng/book.hpp"

using namespace matcheng;

#define BOOK_TEST(name)                                                        \
    template <class Book> static void scenario_##name();                       \
    static harness::Registrar reg_map_##name(#name "/map",                     \
        [] { scenario_##name<MapBook>(); });                                   \
    static harness::Registrar reg_vec_##name(#name "/vec",                     \
        [] { scenario_##name<VecBook>(); });                                   \
    template <class Book> static void scenario_##name()

namespace {
uint64_t zero_clock() { return 0; }

template <class Book> Book make_book() {
    Book b;
    b.set_clock(&zero_clock);  // deterministic timestamps for assertions
    return b;
}
}  // namespace

// --- Basic resting and non-matching behavior -------------------------------

BOOK_TEST(empty_book_market_order_gets_no_fill) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    Qty rem = b.add_market(1, Side::Sell, 100, tr);
    CHECK_EQ(rem, 100u);
    CHECK_EQ(tr.size(), 0u);
    CHECK(!b.best_bid());
    CHECK(!b.best_ask());
    CHECK_EQ(b.order_count(), 0u);
}

BOOK_TEST(non_crossing_limits_rest_without_matching) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    CHECK(b.add_limit(1, Side::Buy, 100, 10, tr));
    CHECK(b.add_limit(2, Side::Sell, 101, 10, tr));  // 1-tick spread: no match
    CHECK_EQ(tr.size(), 0u);
    CHECK_EQ(*b.best_bid(), 100);
    CHECK_EQ(*b.best_ask(), 101);
    CHECK_EQ(b.order_count(), 2u);
    CHECK_EQ(b.qty_at(Side::Buy, 100), 10u);
    CHECK_EQ(b.qty_at(Side::Sell, 101), 10u);
}

BOOK_TEST(equal_price_crosses_full_fill) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    b.add_limit(2, Side::Sell, 100, 10, tr);  // exactly at the bid: trades
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].price, 100);
    CHECK_EQ(tr[0].qty, 10u);
    CHECK_EQ(tr[0].resting_id, 1u);
    CHECK_EQ(tr[0].aggressor_id, 2u);
    CHECK_EQ(b.order_count(), 0u);
    CHECK(!b.best_bid());
    CHECK(!b.best_ask());
}

BOOK_TEST(trade_executes_at_resting_price_not_aggressor_price) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 105, 10, tr);
    b.add_limit(2, Side::Sell, 95, 10, tr);  // willing to sell at 95, bid is 105
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].price, 105);  // resting bid's price, aggressor gets improvement
    CHECK_EQ(b.order_count(), 0u);
}

// --- Partial fills ---------------------------------------------------------

BOOK_TEST(partial_fill_leaves_remainder_resting) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 100, tr);
    b.add_limit(2, Side::Sell, 100, 40, tr);
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].qty, 40u);
    CHECK_EQ(b.qty_at(Side::Buy, 100), 60u);
    CHECK_EQ(*b.order_remaining(1), 60u);
    CHECK(!b.order_remaining(2));  // aggressor fully filled, never rested
}

BOOK_TEST(aggressor_larger_than_resting_rests_remainder) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 30, tr);
    b.add_limit(2, Side::Sell, 100, 70, tr);
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].qty, 30u);
    CHECK(!b.best_bid());              // bid consumed, level removed
    CHECK_EQ(*b.best_ask(), 100);      // remainder rests as new best ask
    CHECK_EQ(b.qty_at(Side::Sell, 100), 40u);
}

BOOK_TEST(partially_filled_order_keeps_front_of_queue) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 50, tr);
    b.add_limit(2, Side::Buy, 100, 50, tr);
    b.add_market(3, Side::Sell, 30, tr);  // partial-fills order 1
    tr.clear();
    b.add_market(4, Side::Sell, 30, tr);  // must exhaust order 1 (20) before order 2
    CHECK_EQ(tr.size(), 2u);
    CHECK_EQ(tr[0].resting_id, 1u);
    CHECK_EQ(tr[0].qty, 20u);
    CHECK_EQ(tr[1].resting_id, 2u);
    CHECK_EQ(tr[1].qty, 10u);
}

// --- Priority rules --------------------------------------------------------

BOOK_TEST(time_priority_fifo_at_same_price) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    b.add_limit(2, Side::Buy, 100, 10, tr);  // same price, later arrival
    b.add_limit(3, Side::Sell, 100, 10, tr);
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].resting_id, 1u);  // earlier order fills first
    CHECK_EQ(b.qty_at(Side::Buy, 100), 10u);
    CHECK_EQ(*b.order_remaining(2), 10u);
}

BOOK_TEST(price_priority_beats_time_priority) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 99, 10, tr);   // earlier but worse price
    b.add_limit(2, Side::Buy, 100, 10, tr);  // later but better price
    b.add_limit(3, Side::Sell, 99, 10, tr);  // crosses both levels' prices
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].resting_id, 2u);   // better price wins despite later arrival
    CHECK_EQ(tr[0].price, 100);
    CHECK_EQ(*b.best_bid(), 99);
}

BOOK_TEST(multi_level_sweep_trades_best_price_first) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Sell, 101, 10, tr);
    b.add_limit(2, Side::Sell, 102, 10, tr);
    b.add_limit(3, Side::Sell, 103, 10, tr);
    b.add_limit(4, Side::Buy, 103, 25, tr);  // sweeps 101, 102, part of 103
    CHECK_EQ(tr.size(), 3u);
    CHECK_EQ(tr[0].price, 101);
    CHECK_EQ(tr[0].qty, 10u);
    CHECK_EQ(tr[1].price, 102);
    CHECK_EQ(tr[1].qty, 10u);
    CHECK_EQ(tr[2].price, 103);
    CHECK_EQ(tr[2].qty, 5u);
    CHECK_EQ(*b.best_ask(), 103);
    CHECK_EQ(b.qty_at(Side::Sell, 103), 5u);
    CHECK(!b.best_bid());  // aggressor fully filled, nothing rested
}

BOOK_TEST(limit_stops_at_its_price_and_rests) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Sell, 101, 10, tr);
    b.add_limit(2, Side::Sell, 105, 10, tr);
    b.add_limit(3, Side::Buy, 103, 30, tr);  // may take 101, must NOT take 105
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].price, 101);
    CHECK_EQ(*b.best_bid(), 103);            // remainder (20) rests at limit
    CHECK_EQ(b.qty_at(Side::Buy, 103), 20u);
    CHECK_EQ(*b.best_ask(), 105);
}

// --- Market orders ---------------------------------------------------------

BOOK_TEST(market_order_sweeps_book_and_reports_unfilled) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Sell, 101, 10, tr);
    b.add_limit(2, Side::Sell, 102, 10, tr);
    Qty rem = b.add_market(3, Side::Buy, 50, tr);
    CHECK_EQ(rem, 30u);          // 20 filled, 30 unfilled (never rests)
    CHECK_EQ(tr.size(), 2u);
    CHECK(!b.best_ask());        // book emptied
    CHECK(!b.best_bid());        // market remainder did NOT rest
    CHECK_EQ(b.order_count(), 0u);
}

BOOK_TEST(market_order_ignores_same_side_liquidity) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    Qty rem = b.add_market(2, Side::Buy, 10, tr);  // buy market, only bids present
    CHECK_EQ(rem, 10u);
    CHECK_EQ(tr.size(), 0u);
    CHECK_EQ(b.qty_at(Side::Buy, 100), 10u);
}

// --- Cancel ----------------------------------------------------------------

BOOK_TEST(cancel_removes_order_and_empty_level) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    b.add_limit(2, Side::Buy, 99, 10, tr);
    CHECK(b.cancel(1));
    CHECK_EQ(*b.best_bid(), 99);          // level 100 fully removed
    CHECK_EQ(b.qty_at(Side::Buy, 100), 0u);
    CHECK_EQ(b.order_count(), 1u);
    tr.clear();
    b.add_limit(3, Side::Sell, 100, 5, tr);  // must NOT hit phantom liquidity
    CHECK_EQ(tr.size(), 0u);
    CHECK_EQ(*b.best_ask(), 100);
}

BOOK_TEST(cancel_unknown_or_repeated_id_fails) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    CHECK(!b.cancel(42));
    b.add_limit(1, Side::Buy, 100, 10, tr);
    CHECK(b.cancel(1));
    CHECK(!b.cancel(1));  // second cancel of the same id fails
}

BOOK_TEST(cancel_middle_of_queue_preserves_fifo_of_others) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    b.add_limit(2, Side::Buy, 100, 10, tr);
    b.add_limit(3, Side::Buy, 100, 10, tr);
    b.cancel(2);
    b.add_market(4, Side::Sell, 15, tr);  // fills 1 fully, then 3 partially
    CHECK_EQ(tr.size(), 2u);
    CHECK_EQ(tr[0].resting_id, 1u);
    CHECK_EQ(tr[1].resting_id, 3u);
    CHECK_EQ(tr[1].qty, 5u);
}

BOOK_TEST(cancel_then_repost_goes_to_back_of_queue) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    b.add_limit(2, Side::Buy, 100, 10, tr);
    b.cancel(1);
    b.add_limit(1, Side::Buy, 100, 10, tr);  // repost same id, same price
    b.add_market(3, Side::Sell, 10, tr);
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].resting_id, 2u);  // reposted order lost its priority
}

// --- Modify ----------------------------------------------------------------

BOOK_TEST(modify_qty_decrease_keeps_queue_position) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 50, tr);
    b.add_limit(2, Side::Buy, 100, 50, tr);
    CHECK(b.modify(1, 100, 20, tr));  // same price, smaller qty: in place
    CHECK_EQ(tr.size(), 0u);
    CHECK_EQ(b.qty_at(Side::Buy, 100), 70u);
    b.add_market(3, Side::Sell, 20, tr);
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].resting_id, 1u);  // order 1 still at front
    CHECK_EQ(tr[0].qty, 20u);
}

BOOK_TEST(modify_qty_increase_loses_queue_position) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    b.add_limit(2, Side::Buy, 100, 10, tr);
    CHECK(b.modify(1, 100, 30, tr));  // qty increase: cancel + repost
    b.add_market(3, Side::Sell, 10, tr);
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].resting_id, 2u);  // order 2 now ahead of modified order 1
    CHECK_EQ(*b.order_remaining(1), 30u);
}

BOOK_TEST(modify_price_change_loses_position_and_may_match) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Sell, 105, 10, tr);
    b.add_limit(2, Side::Buy, 100, 10, tr);
    CHECK(b.modify(2, 105, 10, tr));  // repriced bid now crosses the ask
    CHECK_EQ(tr.size(), 1u);
    CHECK_EQ(tr[0].price, 105);
    CHECK_EQ(tr[0].resting_id, 1u);
    CHECK_EQ(tr[0].aggressor_id, 2u);  // modified order aggresses
    CHECK_EQ(b.order_count(), 0u);
}

BOOK_TEST(modify_price_change_non_crossing_moves_level) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Buy, 100, 10, tr);
    b.add_limit(2, Side::Buy, 99, 10, tr);
    CHECK(b.modify(1, 98, 10, tr));
    CHECK_EQ(tr.size(), 0u);
    CHECK_EQ(*b.best_bid(), 99);
    CHECK_EQ(b.qty_at(Side::Buy, 98), 10u);
    CHECK_EQ(b.qty_at(Side::Buy, 100), 0u);  // old level gone
}

BOOK_TEST(modify_rejects_unknown_id_and_zero_qty) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    CHECK(!b.modify(42, 100, 10, tr));
    b.add_limit(1, Side::Buy, 100, 10, tr);
    CHECK(!b.modify(1, 100, 0, tr));         // zero qty: use cancel instead
    CHECK_EQ(*b.order_remaining(1), 10u);    // unchanged
}

// --- Input validation ------------------------------------------------------

BOOK_TEST(rejects_zero_qty_and_duplicate_resting_id) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    CHECK(!b.add_limit(1, Side::Buy, 100, 0, tr));
    CHECK(b.add_limit(1, Side::Buy, 100, 10, tr));
    CHECK(!b.add_limit(1, Side::Sell, 200, 10, tr));  // id 1 still resting
    CHECK_EQ(b.order_count(), 1u);
    CHECK_EQ(*b.best_bid(), 100);
    CHECK(!b.best_ask());
}

// --- Trade record integrity ------------------------------------------------

BOOK_TEST(trade_records_carry_monotonic_sequence) {
    auto b = make_book<Book>();
    std::vector<Trade> tr;
    b.add_limit(1, Side::Sell, 101, 10, tr);
    b.add_limit(2, Side::Sell, 102, 10, tr);
    b.add_limit(3, Side::Buy, 102, 20, tr);
    CHECK_EQ(tr.size(), 2u);
    CHECK(tr[0].seq < tr[1].seq);
    CHECK_EQ(tr[0].aggressor_id, 3u);
    CHECK_EQ(tr[1].aggressor_id, 3u);
    tr.clear();
    b.add_limit(4, Side::Sell, 100, 1, tr);
    b.add_limit(5, Side::Buy, 100, 1, tr);
    CHECK_EQ(tr.size(), 1u);
    CHECK(tr[0].seq > 0u);  // sequence keeps advancing across events
}

int main() { return harness::run_all(); }
