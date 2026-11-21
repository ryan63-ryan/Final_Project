#include "kvcache/block_table.h"
#include "kvcache/radix_tree.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

// Allocates a block holding `used` tokens; the caller owns the one reference.
BlockId make_block(BlockPool& pool, std::size_t used) {
    auto id = pool.allocate();
    if (!id) throw std::runtime_error("test pool exhausted");
    for (std::size_t i = 0; i < used; ++i)
        pool.append(*id, Payload(pool.bytes_per_token(), static_cast<std::uint8_t>(i + 1)));
    return *id;
}

using Tokens = std::vector<Token>;

void basic_tests() {
    static_assert(!std::is_copy_constructible<RadixTree>::value, "index must not copy");
    BlockPool pool(8, 2, 1);
    RadixTree tree(pool);
    CHECK(tree.size() == 0 && tree.match({1, 2, 3}).empty() && tree.check_invariants());

    BlockId a = make_block(pool, 2), b = make_block(pool, 2), c = make_block(pool, 2);
    CHECK(tree.insert({1, 2}, a));
    CHECK(pool.references(a) == 2);
    pool.release(a); // The request finishes; only the index holds the block.
    CHECK(pool.references(a) == 1);
    CHECK(tree.insert({1, 2, 3, 4}, b) && tree.insert({1, 2, 5, 6}, c));
    pool.release(b);
    pool.release(c);
    CHECK(tree.size() == 3 && tree.check_invariants());

    CHECK((tree.match({1, 2, 3, 4}) == std::vector<BlockId>{a, b}));
    CHECK((tree.match({1, 2, 3, 4, 9}) == std::vector<BlockId>{a, b})); // partial tail ignored
    CHECK((tree.match({1, 2, 5}) == std::vector<BlockId>{a}));
    CHECK((tree.match({1, 2, 7, 8}) == std::vector<BlockId>{a}));
    CHECK(tree.match({2, 1}).empty() && tree.match({1}).empty() && tree.match({}).empty());

    CHECK(tree.contains(a) && !tree.is_leaf(a) && tree.is_leaf(b) && tree.is_leaf(c));
    CHECK(!tree.parent(a) && tree.parent(b) == a && tree.parent(c) == a);
    CHECK(!tree.contains(7) && !tree.is_leaf(7) && !tree.parent(7));

    // Same prefix filled independently: second insert is refused, block stays private.
    BlockId dup = make_block(pool, 2);
    CHECK(!tree.insert({1, 2, 3, 4}, dup));
    CHECK(pool.references(dup) == 1 && !tree.contains(dup) && tree.size() == 3);
    pool.release(dup);

    // Invalid inserts throw and change nothing.
    BlockId partial = make_block(pool, 1), spare = make_block(pool, 2);
    auto free_before = pool.free_count();
    expect_throw<std::invalid_argument>([&] { tree.insert({}, spare); });
    expect_throw<std::invalid_argument>([&] { tree.insert({1, 2, 3}, spare); });
    expect_throw<std::invalid_argument>([&] { tree.insert({9, 9}, partial); });
    expect_throw<std::invalid_argument>([&] { tree.insert({9, 9, 1, 1}, spare); }); // no parent
    expect_throw<std::invalid_argument>([&] { tree.insert({7, 7}, a); }); // already indexed
    expect_throw<std::out_of_range>([&] { tree.insert({7, 7}, 99); });
    CHECK(pool.references(spare) == 1 && pool.references(a) == 1);
    CHECK(pool.free_count() == free_before && tree.size() == 3 && tree.check_invariants());
    pool.release(partial);
    pool.release(spare);

    // Only leaves may be erased; erasing children turns the parent into a leaf.
    CHECK(!tree.erase(a) && !tree.erase(99) && tree.size() == 3);
    free_before = pool.free_count();
    CHECK(tree.erase(b) && !tree.contains(b) && pool.free_count() == free_before + 1);
    CHECK(!tree.erase(b) && !tree.is_leaf(a));
    CHECK(tree.erase(c) && tree.is_leaf(a));
    CHECK((tree.match({1, 2, 3, 4}) == std::vector<BlockId>{a}));
    CHECK(tree.check_invariants() && pool.check_invariants());
    CHECK(tree.erase(a) && tree.size() == 0 && pool.free_count() == pool.capacity());
}

void ownership_tests() {
    BlockPool pool(6, 2, 3);
    {
        RadixTree tree(pool);
        BlockTable request(pool);
        for (std::uint8_t i = 1; i <= 5; ++i) CHECK(request.append(Payload(3, i)));
        Tokens tokens{10, 11, 12, 13, 14};
        const auto& ids = request.blocks();
        CHECK(tree.insert({10, 11}, ids[0]) && tree.insert({10, 11, 12, 13}, ids[1]));
        BlockId first = ids[0], second = ids[1];
        request.clear();
        CHECK(pool.references(first) == 1 && pool.references(second) == 1);
        CHECK(pool.free_count() == 4); // the partial tail went back to the pool

        // A cache hit pins the blocks; eviction cannot take them from the request.
        auto hit = tree.match(tokens);
        CHECK((hit == std::vector<BlockId>{first, second}));
        auto reused = BlockTable::from_prefix(pool, hit);
        CHECK(pool.references(second) == 2);
        CHECK(tree.erase(second) && tree.erase(first) && tree.size() == 0);
        CHECK(pool.references(first) == 1 && reused.read_token(3) == Payload(3, 4));
        CHECK(reused.append(Payload(3, 9)) && reused.copy_count() == 0);

        // Moving the index transfers its references exactly once.
        BlockId again = make_block(pool, 2);
        CHECK(tree.insert({1, 1}, again));
        pool.release(again);
        RadixTree moved(std::move(tree));
        CHECK(moved.contains(again) && pool.references(again) == 1 && moved.check_invariants());
        RadixTree target(pool);
        BlockId other = make_block(pool, 2);
        CHECK(target.insert({2, 2}, other));
        pool.release(other);
        target = std::move(moved); // releases `other`, takes `again`
        CHECK(!pool.allocated(other) && target.contains(again) && pool.references(again) == 1);
        reused.clear();
    } // Destroying the index releases the rest.
    CHECK(pool.free_count() == pool.capacity() && pool.check_invariants());
}

// Seeded operations against a map from indexed prefix to block.
void random_tests(unsigned seed, std::size_t steps) {
    const std::size_t width = 2;
    BlockPool pool(48, width, 1);
    std::mt19937 rng(seed);
    auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
    std::map<Tokens, BlockId> reference;
    std::size_t inserted = 0, refused = 0, erased = 0, blocked = 0, hits = 0;
    {
        RadixTree tree(pool);
        auto random_tokens = [&](std::size_t blocks) {
            Tokens t(blocks * width);
            for (auto& token : t) token = static_cast<Token>(pick(2)); // small alphabet => sharing
            return t;
        };
        auto expected_match = [&](const Tokens& t) {
            std::vector<BlockId> ids;
            for (std::size_t n = width; n <= t.size(); n += width) {
                auto it = reference.find(Tokens(t.begin(), t.begin() + n));
                if (it == reference.end()) break;
                ids.push_back(it->second);
            }
            return ids;
        };
        auto has_child = [&](const Tokens& prefix) {
            auto it = reference.upper_bound(prefix);
            return it != reference.end() && it->first.size() > prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), it->first.begin());
        };
        for (std::size_t step = 0; step < steps; ++step) {
            auto choice = pick(10);
            if (choice < 4 && pool.free_count() > 0) {
                // Extend a matched prefix by one block, like a request filling a block.
                auto t = random_tokens(1 + pick(4));
                auto depth = expected_match(t).size();
                Tokens prefix(t.begin(), t.begin() + std::min(t.size(), (depth + 1) * width));
                if (prefix.size() % width != 0) continue;
                BlockId id = make_block(pool, width);
                bool fresh = reference.count(prefix) == 0;
                CHECK(tree.insert(prefix, id) == fresh);
                if (fresh) { reference[prefix] = id; ++inserted; } else { ++refused; }
                pool.release(id);
            } else if (choice < 7 && !reference.empty()) {
                auto it = reference.begin();
                std::advance(it, pick(reference.size()));
                bool leaf = !has_child(it->first);
                CHECK(tree.is_leaf(it->second) == leaf);
                CHECK(tree.erase(it->second) == leaf);
                if (leaf) { reference.erase(it); ++erased; } else { ++blocked; }
            } else {
                auto t = random_tokens(pick(5));
                auto ids = tree.match(t);
                CHECK(ids == expected_match(t));
                if (!ids.empty()) ++hits;
            }
            CHECK(tree.size() == reference.size());
            CHECK(pool.free_count() == pool.capacity() - reference.size());
            for (const auto& entry : reference) {
                CHECK(pool.references(entry.second) == 1);
                auto parent = tree.parent(entry.second);
                if (entry.first.size() == width) CHECK(!parent);
                else CHECK(parent == reference.at(Tokens(entry.first.begin(), entry.first.end() - width)));
            }
            if (step % 97 == 0) CHECK(tree.check_invariants() && pool.check_invariants());
        }
        CHECK(inserted > 0 && refused > 0 && erased > 0 && blocked > 0 && hits > 0);
    }
    CHECK(pool.free_count() == pool.capacity());
}

int main() {
    try {
        basic_tests(); std::cout << "PASS radix match, insert and erase\n";
        ownership_tests(); std::cout << "PASS radix reference ownership and moves\n";
        for (unsigned seed : {538u, 2026u, 17u}) random_tests(seed, 10000);
        std::cout << "PASS 30000 seeded radix operations\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
