#include "kvcache/block_table.h"
#include "kvcache/min_heap.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace kvcache;

void check(bool ok, const char* condition, int line) {
    if (!ok) throw std::runtime_error("line " + std::to_string(line) + ": " + condition);
}
#define CHECK(condition) check((condition), #condition, __LINE__)

template <class Exception, class Function>
void expect_throw(Function action) {
    bool caught = false;
    try { action(); } catch (const Exception&) { caught = true; }
    CHECK(caught);
}

void pool_tests() {
    static_assert(!std::is_copy_constructible<BlockPool>::value, "pool must not copy");
    static_assert(!std::is_move_constructible<BlockPool>::value, "pool address must stay stable");
    expect_throw<std::invalid_argument>([] { BlockPool p(0, 2, 1); });
    expect_throw<std::invalid_argument>([] { BlockPool p(1, 0, 1); });
    expect_throw<std::invalid_argument>([] { BlockPool p(1, 2, 0); });
    expect_throw<std::overflow_error>([] {
        BlockPool p(2, std::numeric_limits<std::size_t>::max(), 2);
    });
    expect_throw<std::overflow_error>([] {
        BlockPool p(std::numeric_limits<std::size_t>::max(), 2, 1);
    });
    BlockPool pool(2, 2, 3);
    CHECK(pool.payload_capacity_bytes() == 12);
    BlockId first = *pool.allocate();
    CHECK(first == 0 && pool.references(first) == 1);
    pool.append(first, {1, 2, 3});
    pool.retain(first);
    expect_throw<std::logic_error>([&] { pool.append(first, {4, 5, 6}); });
    CHECK(pool.used_tokens(first) == 1);
    pool.release(first);
    BlockId copy = *pool.clone(first);
    pool.append(copy, {4, 5, 6});
    CHECK(pool.read_token(first, 0) == Payload({1, 2, 3}));
    CHECK(pool.used_tokens(first) == 1 && pool.full(copy));
    CHECK(!pool.allocate() && !pool.clone(first));
    expect_throw<std::logic_error>([&] { pool.append(copy, {7, 8, 9}); });
    expect_throw<std::invalid_argument>([&] { pool.append(first, {1}); });
    expect_throw<std::out_of_range>([&] { pool.read_token(first, 1); });
    expect_throw<std::out_of_range>([&] { pool.retain(9); });
    pool.release(copy);
    expect_throw<std::out_of_range>([&] { pool.release(copy); });
    expect_throw<std::out_of_range>([&] { pool.clone(copy); });
    CHECK(*pool.allocate() == copy);
    CHECK(pool.used_tokens(copy) == 0);
    pool.append(copy, {9, 9, 9});
    CHECK(pool.read_token(copy, 0) == Payload({9, 9, 9}));
    pool.release(copy);
    pool.release(first);
    CHECK(pool.free_count() == pool.capacity() && pool.check_invariants());
}

void cow_tests() {
    BlockPool pool(4, 2, 1);
    {
        BlockTable original(pool);
        CHECK(original.append({10}) && original.append({11}) && original.append({12}));
        CHECK(original.blocks() == std::vector<BlockId>({0, 1}));
        auto copy = original.fork();
        CHECK(copy.blocks() == original.blocks());
        CHECK(pool.references(0) == 2 && pool.references(1) == 2);
        CHECK(copy.append({13}));
        CHECK(copy.blocks() == std::vector<BlockId>({0, 2}));
        CHECK(copy.copy_count() == 1 && original.copy_count() == 0);
        CHECK(original.token_count() == 3 && original.read_token(2) == Payload({12}));
        CHECK(copy.read_token(3) == Payload({13}));
        original.clear(); // Parent-first release must not invalidate the child.
        CHECK(!pool.allocated(1) && pool.references(0) == 1);
        CHECK(copy.read_token(0) == Payload({10}));
        CHECK(copy.check_invariants() && pool.check_invariants());
    }
    CHECK(pool.free_count() == 4);
    BlockPool small(1, 2, 1);
    BlockTable parent(small);
    CHECK(parent.append({7}));
    auto child = parent.fork();
    CHECK(!child.append({8}));
    CHECK(child.blocks() == parent.blocks());
    CHECK(small.references(0) == 2 && child.token_count() == 1 && child.copy_count() == 0);
    expect_throw<std::invalid_argument>([&] { child.append({1, 2}); });
    parent.clear();
    CHECK(child.append({8})); // Exclusive tail can be written even when the pool is full.
    CHECK(child.copy_count() == 0 && child.read_token(1) == Payload({8}));
    CHECK(!child.append({9}));
    child.clear();
    child.clear();
    CHECK(small.free_count() == 1 && small.check_invariants());
}

void table_tests() {
    static_assert(!std::is_copy_constructible<BlockTable>::value, "use explicit fork");
    BlockPool pool(5, 2, 1);
    BlockTable a(pool);
    auto empty = a.fork();
    CHECK(empty.token_count() == 0 && pool.free_count() == 5);
    expect_throw<std::out_of_range>([&] { a.read_token(0); });
    CHECK(a.append({1}));
    expect_throw<std::invalid_argument>([&] { BlockTable::from_prefix(pool, a.blocks()); });
    CHECK(pool.references(a.blocks().front()) == 1);
    CHECK(a.append({2}));
    CHECK(a.append({3}) && a.append({4}));
    auto ids = a.blocks();
    for (BlockId id : ids) pool.retain(id); // The future radix index owns these references.
    auto cached = BlockTable::from_prefix(pool, ids);
    a.clear();
    CHECK(pool.references(ids[0]) == 2);
    CHECK(cached.append({5})); // A full shared tail gets a new block, not a copy.
    CHECK(cached.copy_count() == 0 && cached.token_count() == 5);
    auto before = pool.references(ids[0]);
    expect_throw<std::out_of_range>([&] { BlockTable::from_prefix(pool, {ids[0], 99}); });
    CHECK(pool.references(ids[0]) == before);
    auto moved = std::move(cached);
    CHECK(cached.token_count() == 0 && cached.blocks().empty());
    CHECK(moved.token_count() == 5 && moved.check_invariants());
    CHECK(cached.append({20})); // Moved-from tables remain empty and usable.
    BlockPool other(2, 2, 1);
    BlockTable target(other);
    CHECK(target.append({9}));
    target = std::move(moved);
    CHECK(other.free_count() == 2 && target.token_count() == 5);
    auto* same = &target;
    target = std::move(*same);
    CHECK(target.token_count() == 5);
    target.clear();
    for (BlockId id : ids) {
        CHECK(pool.references(id) == 1); // Only the cache owns it now: eligible for eviction.
        pool.release(id);
    }
    cached.clear();
    CHECK(pool.free_count() == 5 && pool.check_invariants());
    BlockPool width_one(2, 1, 2);
    BlockTable one(width_one);
    CHECK(one.append({1, 2}));
    auto fork = one.fork();
    CHECK(fork.append({3, 4}) && fork.copy_count() == 0);
    CHECK(one.token_count() == 1);
}

void integration_storage_tests() {
    // Proposal-sized payloads, but a small pool to force copy-on-write failure.
    BlockPool pool(4, 16, 64);
    BlockTable parent(pool);
    std::vector<Payload> expected;
    for (std::size_t token = 0; token < 33; ++token) {
        Payload bytes(64);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(token * 17 + i);
        expected.push_back(bytes);
        CHECK(parent.append(bytes));
    }
    auto ids = parent.blocks();
    const std::vector<BlockId> cached_ids{ids[0], ids[1]};
    for (BlockId id : cached_ids) pool.retain(id);
    auto child = parent.fork();
    auto grandchild = child.fork();
    CHECK(pool.references(ids[0]) == 4 && pool.references(ids[2]) == 3);
    expect_throw<std::invalid_argument>([&] {
        BlockTable::from_prefix(pool, {ids[0], ids[2]});
    });
    CHECK(pool.references(ids[0]) == 4 && pool.references(ids[2]) == 3);

    const Payload child_bytes(64, 0xAB), grandchild_bytes(64, 0xCD);
    CHECK(child.append(child_bytes) && child.copy_count() == 1);
    CHECK(pool.free_count() == 0);
    CHECK(!grandchild.append(grandchild_bytes));
    CHECK(grandchild.blocks() == ids && grandchild.token_count() == expected.size());
    CHECK(pool.references(ids[2]) == 2 && grandchild.copy_count() == 0);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(parent.read_token(i) == expected[i]);
        CHECK(child.read_token(i) == expected[i]);
        CHECK(grandchild.read_token(i) == expected[i]);
    }
    parent.clear();
    CHECK(grandchild.append(grandchild_bytes));
    CHECK(grandchild.copy_count() == 0); // The tail is now exclusive despite a full pool.
    CHECK(child.read_token(33) == child_bytes);
    CHECK(grandchild.read_token(33) == grandchild_bytes);
    child.clear();
    grandchild.clear();
    for (BlockId id : cached_ids) CHECK(pool.references(id) == 1);

    auto reused = BlockTable::from_prefix(pool, cached_ids);
    for (BlockId id : cached_ids) pool.release(id); // Request retains the prefix after cache removal.
    for (std::size_t i = 0; i < 32; ++i) CHECK(reused.read_token(i) == expected[i]);
    CHECK(reused.append(child_bytes) && reused.copy_count() == 0);
    CHECK(reused.check_invariants() && pool.check_invariants());
    reused.clear();
    CHECK(pool.free_count() == pool.capacity());
}

// A vector of complete per-request payloads is independent of physical sharing.
void random_storage_tests() {
    std::size_t successful_copies = 0, rejected_appends = 0, prefix_imports = 0;
    for (unsigned seed : {538u, 2026u, 17u}) {
        for (std::size_t width : {1u, 2u, 7u}) {
            BlockPool pool(12, width, 4);
            std::vector<std::unique_ptr<BlockTable>> requests(20);
            std::vector<std::vector<Payload>> reference(20);
            std::vector<BlockId> cache;
            std::map<BlockId, std::vector<Payload>> cached_reference;
            std::mt19937 random(seed);
            for (int step = 0; step < 3000; ++step) {
                std::size_t slot = random() % requests.size();
                unsigned op = random() % 7;
                if (op == 0 && !requests[slot]) {
                    requests[slot] = std::make_unique<BlockTable>(pool);
                    reference[slot].clear();
                } else if (op == 1 && requests[slot]) {
                    Payload payload(4);
                    for (auto& byte : payload) byte = static_cast<std::uint8_t>(random());
                    auto ids = requests[slot]->blocks();
                    auto old_count = requests[slot]->copy_count();
                    if (requests[slot]->append(payload)) {
                        reference[slot].push_back(payload);
                        successful_copies += requests[slot]->copy_count() - old_count;
                    } else {
                        ++rejected_appends;
                        CHECK(ids == requests[slot]->blocks());
                        CHECK(old_count == requests[slot]->copy_count());
                    }
                } else if (op == 2 && requests[slot]) {
                    std::size_t dest = random() % requests.size();
                    if (!requests[dest]) {
                        requests[dest] = std::make_unique<BlockTable>(requests[slot]->fork());
                        reference[dest] = reference[slot];
                    }
                } else if (op == 3) {
                    requests[slot].reset();
                    reference[slot].clear();
                } else if (op == 4 && requests[slot]) {
                    for (std::size_t b = 0; b < requests[slot]->blocks().size(); ++b) {
                        BlockId id = requests[slot]->blocks()[b];
                        if (pool.full(id) && std::find(cache.begin(), cache.end(), id) == cache.end()) {
                            pool.retain(id);
                            cache.push_back(id);
                            auto first = reference[slot].begin() + b * width;
                            cached_reference[id] = std::vector<Payload>(first, first + width);
                        }
                    }
                } else if (op == 5 && !cache.empty()) {
                    pool.release(cache.back());
                    cached_reference.erase(cache.back());
                    cache.pop_back();
                } else if (op == 6 && !requests[slot] && !cache.empty()) {
                    BlockId id = cache[random() % cache.size()];
                    requests[slot] = std::make_unique<BlockTable>(BlockTable::from_prefix(pool, {id}));
                    reference[slot] = cached_reference.at(id);
                    ++prefix_imports;
                }
                std::vector<std::size_t> expected_refs(pool.capacity(), 0);
                for (BlockId id : cache) {
                    ++expected_refs[id];
                    for (std::size_t j = 0; j < width; ++j)
                        CHECK(pool.read_token(id, j) == cached_reference.at(id)[j]);
                }
                for (std::size_t i = 0; i < requests.size(); ++i) {
                    if (!requests[i]) continue;
                    CHECK(requests[i]->check_invariants());
                    CHECK(requests[i]->token_count() == reference[i].size());
                    for (std::size_t j = 0; j < reference[i].size(); ++j)
                        CHECK(requests[i]->read_token(j) == reference[i][j]);
                    for (BlockId id : requests[i]->blocks()) ++expected_refs[id];
                }
                for (BlockId id = 0; id < pool.capacity(); ++id) {
                    CHECK(pool.allocated(id) == (expected_refs[id] != 0));
                    if (pool.allocated(id)) CHECK(pool.references(id) == expected_refs[id]);
                }
                CHECK(pool.check_invariants());
            }
            requests.clear();
            for (BlockId id : cache) pool.release(id);
            CHECK(pool.free_count() == pool.capacity());
        }
    }
    CHECK(successful_copies > 0 && rejected_appends > 0 && prefix_imports > 0);
}

void heap_tests() {
    MinHeap<int> heap;
    expect_throw<std::out_of_range>([&] { heap.top(); });
    expect_throw<std::out_of_range>([&] { heap.pop(); });
    CHECK(!heap.erase(4) && !heap.update(4, 8));
    heap.push(9, 10);
    heap.push(2, 10);
    CHECK(heap.top().key == 2);
    expect_throw<std::invalid_argument>([&] { heap.push(2, 1); });
    CHECK(heap.size() == 2 && heap.check_invariants());
    CHECK(heap.update(9, 1) && heap.top().key == 9);
    CHECK(heap.update(9, 20) && heap.top().key == 2);
    CHECK(heap.erase(9) && heap.pop().key == 2 && heap.empty());
    MinHeap<int, std::greater<int>> reverse;
    reverse.push(0, 4);
    reverse.push(1, 9);
    CHECK(reverse.pop().value == 9 && reverse.check_invariants());

    for (unsigned seed : {538u, 2026u, 17u}) {
        MinHeap<int> actual;
        std::map<std::size_t, int> expected;
        std::mt19937 random(seed);
        for (int step = 0; step < 10000; ++step) {
            std::size_t key = random() % 120;
            int value = static_cast<int>(random() % 101) - 50;
            switch (random() % 4) {
            case 0:
                if (expected.count(key) == 0) { actual.push(key, value); expected[key] = value; }
                break;
            case 1:
                CHECK(actual.update(key, value) == (expected.count(key) != 0));
                if (expected.count(key)) expected[key] = value;
                break;
            case 2:
                CHECK(actual.erase(key) == (expected.erase(key) != 0));
                break;
            case 3:
                if (!expected.empty()) {
                    auto best = std::min_element(expected.begin(), expected.end(), [](auto a, auto b) {
                        return a.second != b.second ? a.second < b.second : a.first < b.first;
                    });
                    auto entry = actual.pop();
                    CHECK(entry.key == best->first && entry.value == best->second);
                    expected.erase(best);
                }
            }
            CHECK(actual.size() == expected.size() && actual.check_invariants());
            if (!expected.empty()) {
                auto best = std::min_element(expected.begin(), expected.end(), [](auto a, auto b) {
                    return a.second != b.second ? a.second < b.second : a.first < b.first;
                });
                CHECK(actual.top().key == best->first && actual.top().value == best->second);
            }
        }
    }
}

void large_heap_tests() {
    MinHeap<std::uint64_t> heap;
    std::vector<std::pair<std::uint64_t, std::size_t>> expected;
    std::mt19937_64 random(538);
    for (std::size_t key = 0; key < 4096; ++key) {
        auto priority = random();
        heap.push(key, priority);
        expected.push_back({priority, key});
    }
    const auto max_key = std::numeric_limits<std::size_t>::max();
    const auto max_rank = std::numeric_limits<std::uint64_t>::max();
    heap.push(max_key, max_rank);
    expected.push_back({max_rank, max_key});
    CHECK(heap.update(max_key, 0) && heap.top().key == max_key);
    CHECK(heap.update(max_key, max_rank) && heap.check_invariants());
    std::sort(expected.begin(), expected.end()); // Independent test oracle only.
    for (const auto& wanted : expected) {
        auto actual = heap.pop();
        CHECK(actual.key == wanted.second && actual.value == wanted.first);
    }
    CHECK(heap.empty() && heap.check_invariants());
}

int main() {
    try {
        pool_tests(); std::cout << "PASS block pool\n";
        cow_tests(); std::cout << "PASS copy-on-write and capacity failure\n";
        table_tests(); std::cout << "PASS prefix ownership, moves and release\n";
        integration_storage_tests(); std::cout << "PASS 16-token/64-byte payloads and multi-fork cache ownership\n";
        random_storage_tests(); std::cout << "PASS 27000 seeded storage operations\n";
        heap_tests(); std::cout << "PASS indexed heap and 30000 reference operations\n";
        large_heap_tests(); std::cout << "PASS 4097-entry heap with 64-bit priorities\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
