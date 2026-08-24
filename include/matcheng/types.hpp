#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

namespace matcheng {

using Price = int64_t;    // integer ticks
using Qty = uint64_t;
using OrderId = uint64_t;

enum class Side : uint8_t { Buy, Sell };

// One execution. price is always the RESTING order's limit price.
// seq is a book-wide monotonic sequence number (logical time, deterministic);
// ts_ns comes from the book's clock (wall time by default, injectable for
// deterministic tests / zero-overhead benchmarks).
struct Trade {
    Price price;
    Qty qty;
    OrderId resting_id;
    OrderId aggressor_id;
    uint64_t seq;
    uint64_t ts_ns;
};

struct Level;

// Resting order: an intrusive node in its price level's FIFO list.
// Node addresses are stable (pool-allocated), so the id index can hold raw
// pointers and cancel is O(1) unlink + O(1) hash erase.
struct Order {
    OrderId id;
    Price price;
    Qty remaining;
    Side side;
    Order* prev;
    Order* next;
    Level* level;
};

// One price level: FIFO queue of orders (head = oldest = first to fill)
// plus a maintained aggregate quantity.
struct Level {
    Price price = 0;
    Qty total_qty = 0;
    Order* head = nullptr;
    Order* tail = nullptr;

    void push_back(Order* o) {
        o->prev = tail;
        o->next = nullptr;
        (tail ? tail->next : head) = o;
        tail = o;
        o->level = this;
        total_qty += o->remaining;
    }

    void unlink(Order* o) {
        (o->prev ? o->prev->next : head) = o->next;
        (o->next ? o->next->prev : tail) = o->prev;
        total_qty -= o->remaining;
    }
};

// Deque-backed arena (stable addresses) with a free list. Steady state does
// zero allocation: freed nodes are recycled.
template <class T>
class Pool {
public:
    T* alloc() {
        if (!free_.empty()) {
            T* p = free_.back();
            free_.pop_back();
            return p;
        }
        arena_.emplace_back();
        return &arena_.back();
    }
    void release(T* p) { free_.push_back(p); }

private:
    std::deque<T> arena_;
    std::vector<T*> free_;
};

inline uint64_t wall_clock_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace matcheng
