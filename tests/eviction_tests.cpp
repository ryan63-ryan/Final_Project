#include "kvcache/block_table.h"
#include "kvcache/eviction_policy.h"
#include "kvcache/radix_tree.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
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

using Tokens = std::vector<Token>;
using Ids = std::vector<BlockId>;

// Allocates a full block; the caller owns the one reference.
BlockId make_block(BlockPool& pool) {
    auto id = pool.allocate();
    if (!id) throw std::runtime_error("test pool exhausted");
    for (std::size_t i = 0; i < pool.tokens_per_block(); ++i)
        pool.append(*id, Payload(pool.bytes_per_token(), static_cast<std::uint8_t>(*id + 1)));
    return *id;
}

// What the manager will do when a request fills a block: index it, hand the
// index's reference to the policy, drop the request's own reference, and mark
// the new leaf evictable and its parent not.
BlockId cache_block(BlockPool& pool, RadixTree& tree, EvictionPolicy& policy,
                    const Tokens& prefix) {
    BlockId id = make_block(pool);
    CHECK(tree.insert(prefix, id));
    policy.on_insert(id);
    pool.release(id);
    tree.refresh(id);
    policy.set_evictable(id, tree.is_evictable(id));
    if (auto parent = tree.parent(id)) policy.set_evictable(*parent, tree.is_evictable(*parent));
    return id;
}

// A cache hit: the request pins the matched blocks, which become non-evictable.
BlockTable hit(BlockPool& pool, RadixTree& tree, EvictionPolicy& policy, const Tokens& tokens) {
    auto ids = tree.match(tokens);
    auto table = BlockTable::from_prefix(pool, ids);
    for (BlockId id : ids) {
        tree.refresh(id);
        policy.on_access(id);
        policy.set_evictable(id, tree.is_evictable(id));
    }
    return table;
}

// The request finishes: its blocks may become evictable again.
void finish(BlockTable& table, RadixTree& tree, EvictionPolicy& policy) {
    auto ids = table.blocks();
    table.clear();
    for (BlockId id : ids) {
        tree.refresh(id);
        if (tree.contains(id)) policy.set_evictable(id, tree.is_evictable(id));
    }
}

Ids drain(RadixTree& tree, EvictionPolicy& policy) {
    Ids order;
    while (auto id = evict_one(tree, policy)) order.push_back(*id);
    return order;
}

// Accessing A after inserting A, B, C: LRU moves A to the back, FIFO does not.
template <class Policy>
void access_order(const Ids& want_direct, const Ids& want_after_hit) {
    {
        BlockPool pool(4, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId a = cache_block(pool, tree, policy, {1, 1});
        BlockId b = cache_block(pool, tree, policy, {2, 2});
        BlockId c = cache_block(pool, tree, policy, {3, 3});
        CHECK(policy.victim() == a && policy.evictable_count() == 3);
        policy.on_access(a); // while evictable
        CHECK(policy.check_invariants());
        auto order = drain(tree, policy);
        Ids want;
        for (std::size_t i : want_direct) want.push_back(Ids{a, b, c}[i]);
        CHECK(order == want);
        CHECK(tree.size() == 0 && pool.free_count() == pool.capacity());
    }
    {
        // The realistic path: a hit pins A, accesses it, and the request ends later.
        BlockPool pool(4, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId a = cache_block(pool, tree, policy, {1, 1});
        BlockId b = cache_block(pool, tree, policy, {2, 2});
        BlockId c = cache_block(pool, tree, policy, {3, 3});
        auto request = hit(pool, tree, policy, {1, 1, 9});
        CHECK((request.blocks() == Ids{a}) && policy.victim() == b);
        finish(request, tree, policy);
        auto order = drain(tree, policy);
        Ids want;
        for (std::size_t i : want_after_hit) want.push_back(Ids{a, b, c}[i]);
        CHECK(order == want);
        CHECK(policy.check_invariants() && pool.free_count() == pool.capacity());
    }
}

// A leaf still held by a request is never evicted, whatever its rank.
template <class Policy>
void pinned_leaf() {
    BlockPool pool(4, 2, 1);
    RadixTree tree(pool);
    Policy policy;
    BlockId a = cache_block(pool, tree, policy, {1, 1}); // oldest
    BlockId b = cache_block(pool, tree, policy, {2, 2});
    auto request = BlockTable::from_prefix(pool, tree.match({1, 1}));
    policy.set_evictable(a, tree.is_evictable(a)); // pinned without an access
    CHECK(pool.references(a) == 2 && tree.is_leaf(a) && !tree.is_evictable(a));
    CHECK(policy.victim() == b && policy.evictable_count() == 1);
    CHECK(evict_one(tree, policy) == b);
    CHECK(!evict_one(tree, policy) && tree.contains(a) && pool.references(a) == 2);

    // A stale "evictable" mark cannot get a pinned leaf evicted.
    policy.set_evictable(a, true);
    expect_throw<std::logic_error>([&] { evict_one(tree, policy); });
    CHECK(tree.contains(a) && pool.references(a) == 2 && policy.victim() == a);
    CHECK(tree.check_invariants() && policy.check_invariants());
    policy.set_evictable(a, false);

    CHECK(request.read_token(1) == Payload(1, static_cast<std::uint8_t>(a + 1)));
    finish(request, tree, policy);
    CHECK(policy.victim() == a && evict_one(tree, policy) == a);
    CHECK(tree.size() == 0 && pool.free_count() == pool.capacity());
}

// Erasing a parent's last child makes evict_one re-check the parent.
template <class Policy>
void parent_recheck() {
    {
        BlockPool pool(5, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId p = cache_block(pool, tree, policy, {1, 1}); // oldest of all
        BlockId c1 = cache_block(pool, tree, policy, {1, 1, 2, 2});
        BlockId c2 = cache_block(pool, tree, policy, {1, 1, 3, 3});
        CHECK(!tree.is_evictable(p) && policy.evictable_count() == 2 && policy.victim() == c1);
        CHECK(evict_one(tree, policy) == c1);
        CHECK(!tree.is_leaf(p) && policy.evictable_count() == 1 && policy.victim() == c2);
        CHECK(evict_one(tree, policy) == c2); // last child: p is re-checked
        CHECK(tree.is_evictable(p) && policy.evictable_count() == 1 && policy.victim() == p);
        CHECK(evict_one(tree, policy) == p && !evict_one(tree, policy));
        CHECK(pool.free_count() == pool.capacity());
    }
    {
        // The parent becomes a leaf while a request holds it: re-checked, still kept.
        BlockPool pool(5, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId p = cache_block(pool, tree, policy, {1, 1});
        BlockId c = cache_block(pool, tree, policy, {1, 1, 2, 2});
        auto request = BlockTable::from_prefix(pool, tree.match({1, 1, 7, 7})); // pins p only
        CHECK(evict_one(tree, policy) == c);
        CHECK(tree.is_leaf(p) && !tree.is_evictable(p) && policy.evictable_count() == 0);
        CHECK(!evict_one(tree, policy) && tree.contains(p));
        finish(request, tree, policy);
        CHECK(policy.victim() == p && evict_one(tree, policy) == p);
        CHECK(pool.free_count() == pool.capacity());
    }
    {
        // A three-level chain empties from the bottom up.
        BlockPool pool(4, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId x = cache_block(pool, tree, policy, {1, 1});
        BlockId y = cache_block(pool, tree, policy, {1, 1, 2, 2});
        BlockId z = cache_block(pool, tree, policy, {1, 1, 2, 2, 3, 3});
        CHECK(policy.evictable_count() == 1);
        CHECK((drain(tree, policy) == Ids{z, y, x}));
    }
}

// The scan KVCacheManager::reclaimable_blocks() uses: size() minus the held
// indexed blocks and their ancestors.
std::size_t scan_reclaimable(const BlockPool& pool, const RadixTree& tree) {
    std::vector<bool> stuck(pool.capacity(), false);
    std::size_t stuck_count = 0;
    for (BlockId id = 0; id < pool.capacity(); ++id) {
        if (!tree.contains(id) || pool.references(id) == 1) continue;
        for (std::optional<BlockId> block = id; block && !stuck[*block]; block = tree.parent(*block)) {
            stuck[*block] = true;
            ++stuck_count;
        }
    }
    return tree.size() - stuck_count;
}

// Evicts until nothing is evictable. Each evict_one call must remove exactly one
// physical block, and afterwards the policy's evictable set must match the tree
// (this is where a missed parent re-check would show). The reclaimable count
// drops by exactly one per eviction, and the loop evicts exactly that many.
Ids drain_checked(BlockPool& pool, RadixTree& tree, EvictionPolicy& policy, const Ids& all) {
    Ids order;
    const auto reclaimable = tree.reclaimable_count();
    for (;;) {
        auto size = tree.size();
        auto free = pool.free_count();
        auto count = tree.reclaimable_count();
        CHECK(tree.check_reclaimable() && (count > 0) == (policy.evictable_count() > 0));
        auto id = evict_one(tree, policy);
        if (!id) {
            CHECK(tree.size() == size && pool.free_count() == free);
            CHECK(count == 0 && order.size() == reclaimable);
            return order;
        }
        order.push_back(*id);
        CHECK(tree.size() == size - 1 && pool.free_count() == free + 1 && !tree.contains(*id));
        CHECK(tree.reclaimable_count() == count - 1);
        std::size_t evictable = 0;
        for (BlockId block : all) evictable += tree.is_evictable(block) ? 1 : 0;
        CHECK(policy.evictable_count() == evictable);
        CHECK(policy.check_invariants() && tree.check_invariants());
    }
}

// Several blocks on one compressed edge are still evicted one physical block at a
// time, from the end of the edge, with the parent re-checked after each.
template <class Policy>
void compressed_edges(bool lru) {
    {
        BlockPool pool(4, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId a = cache_block(pool, tree, policy, {1, 1});
        BlockId b = cache_block(pool, tree, policy, {1, 1, 2, 2});
        BlockId c = cache_block(pool, tree, policy, {1, 1, 2, 2, 3, 3});
        CHECK(tree.node_count() == 1 && policy.evictable_count() == 1 && policy.victim() == c);
        CHECK((drain_checked(pool, tree, policy, {a, b, c}) == Ids{c, b, a}));
        CHECK(tree.node_count() == 0 && pool.free_count() == pool.capacity());
    }
    {
        // A request holds the edge up to B: only C may go, B and A stay.
        BlockPool pool(4, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId a = cache_block(pool, tree, policy, {1, 1});
        BlockId b = cache_block(pool, tree, policy, {1, 1, 2, 2});
        BlockId c = cache_block(pool, tree, policy, {1, 1, 2, 2, 3, 3});
        auto request = hit(pool, tree, policy, {1, 1, 2, 2, 9});
        CHECK((request.blocks() == Ids{a, b}));
        CHECK((drain_checked(pool, tree, policy, {a, b, c}) == Ids{c}));
        CHECK(tree.contains(a) && tree.contains(b) && tree.is_leaf(b) && !tree.is_evictable(b));
        finish(request, tree, policy);
        CHECK((drain_checked(pool, tree, policy, {a, b, c}) == Ids{b, a}));
    }
    {
        // After a split [A|B] -> {[C], [D]}, B waits for its last child.
        BlockPool pool(5, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId a = cache_block(pool, tree, policy, {1, 1});
        BlockId b = cache_block(pool, tree, policy, {1, 1, 2, 2});
        BlockId c = cache_block(pool, tree, policy, {1, 1, 2, 2, 3, 3});
        BlockId d = cache_block(pool, tree, policy, {1, 1, 2, 2, 7, 7});
        CHECK(tree.node_count() == 3 && policy.evictable_count() == 2 && policy.victim() == c);
        CHECK((drain_checked(pool, tree, policy, {a, b, c, d}) == Ids{c, d, b, a}));
    }
    {
        // Two compressed edges [A1|A2] and [B1|B2]; a hit reuses A1 and A2.
        BlockPool pool(5, 2, 1);
        RadixTree tree(pool);
        Policy policy;
        BlockId a1 = cache_block(pool, tree, policy, {1, 1});
        BlockId a2 = cache_block(pool, tree, policy, {1, 1, 2, 2});
        BlockId b1 = cache_block(pool, tree, policy, {3, 3});
        BlockId b2 = cache_block(pool, tree, policy, {3, 3, 4, 4});
        CHECK(tree.node_count() == 2);
        auto request = hit(pool, tree, policy, {1, 1, 2, 2});
        finish(request, tree, policy);
        auto order = drain_checked(pool, tree, policy, {a1, a2, b1, b2});
        CHECK(order == (lru ? Ids{b2, b1, a2, a1} : Ids{a2, a1, b2, b1}));
        CHECK(pool.free_count() == pool.capacity());
    }
}

void api_tests() {
    LRUPolicy lru;
    FIFOPolicy fifo;
    for (EvictionPolicy* policy : {static_cast<EvictionPolicy*>(&lru), static_cast<EvictionPolicy*>(&fifo)}) {
        CHECK(!policy->victim() && policy->evictable_count() == 0 && policy->check_invariants());
        expect_throw<std::invalid_argument>([&] { policy->on_access(3); });
        expect_throw<std::invalid_argument>([&] { policy->on_erase(3); });
        expect_throw<std::invalid_argument>([&] { policy->set_evictable(3, true); });
        policy->on_insert(3);
        policy->on_insert(1);
        expect_throw<std::invalid_argument>([&] { policy->on_insert(3); });
        CHECK(!policy->victim()); // tracked blocks start not evictable
        policy->set_evictable(1, true);
        policy->set_evictable(1, true); // idempotent
        policy->set_evictable(3, false);
        CHECK(policy->evictable_count() == 1 && policy->victim() == 1);
        policy->set_evictable(3, true);
        CHECK(policy->victim() == 3); // inserted first
        policy->on_erase(3);
        CHECK(policy->victim() == 1 && policy->evictable_count() == 1);
        policy->on_insert(3); // a recycled block ID starts over as the newest
        policy->set_evictable(3, true);
        CHECK(policy->victim() == 1 && policy->check_invariants());
        policy->on_erase(1);
        policy->on_erase(3);
        CHECK(!policy->victim() && policy->check_invariants());
    }
}

// Seeded operations against a scan-based model of eligibility and rank.
template <class Policy>
void random_tests(unsigned seed, std::size_t steps, bool lru) {
    const std::size_t width = 2;
    BlockPool pool(24, width, 1);
    std::mt19937 rng(seed);
    auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };
    std::size_t evictions = 0, starved = 0, promoted = 0, hits = 0;
    {
        RadixTree tree(pool);
        Policy policy;
        std::map<Tokens, BlockId> prefixes;      // model of the index
        std::map<BlockId, std::uint64_t> ranks;  // model of the policy
        std::uint64_t clock = 0;
        std::vector<std::unique_ptr<BlockTable>> requests;

        auto random_tokens = [&](std::size_t blocks) {
            Tokens t(blocks * width);
            for (auto& token : t) token = static_cast<Token>(pick(2));
            return t;
        };
        auto model_match = [&](const Tokens& t) {
            std::size_t depth = 0;
            while ((depth + 1) * width <= t.size() &&
                   prefixes.count(Tokens(t.begin(), t.begin() + (depth + 1) * width)))
                ++depth;
            return depth;
        };
        auto model_evictable = [&](const Tokens& prefix, BlockId id) {
            auto next = prefixes.upper_bound(prefix);
            bool leaf = next == prefixes.end() || next->first.size() <= prefix.size() ||
                        !std::equal(prefix.begin(), prefix.end(), next->first.begin());
            for (const auto& request : requests)
                for (BlockId held : request->blocks())
                    if (held == id) return false;
            return leaf;
        };
        auto model_victim = [&] {
            std::optional<BlockId> best;
            std::size_t count = 0;
            for (const auto& entry : prefixes) {
                if (!model_evictable(entry.first, entry.second)) continue;
                ++count;
                if (!best || ranks.at(entry.second) < ranks.at(*best)) best = entry.second;
            }
            return std::make_pair(best, count);
        };

        for (std::size_t step = 0; step < steps; ++step) {
            auto choice = pick(10);
            if (choice < 4 && pool.free_count() > 0) {
                auto t = random_tokens(1 + pick(3));
                auto depth = model_match(t);
                if ((depth + 1) * width > t.size()) continue;
                Tokens prefix(t.begin(), t.begin() + (depth + 1) * width);
                prefixes[prefix] = cache_block(pool, tree, policy, prefix);
                ranks[prefixes[prefix]] = ++clock;
            } else if (choice < 6) {
                auto t = random_tokens(1 + pick(3));
                if (model_match(t) == 0 || requests.size() >= 6) continue;
                requests.push_back(std::make_unique<BlockTable>(hit(pool, tree, policy, t)));
                if (lru) for (BlockId id : requests.back()->blocks()) ranks[id] = ++clock;
                ++hits;
            } else if (choice < 8 && !requests.empty()) {
                auto it = requests.begin() + static_cast<std::ptrdiff_t>(pick(requests.size()));
                auto table = std::move(*it);
                requests.erase(it);
                finish(*table, tree, policy);
            } else {
                auto expected = model_victim().first;
                std::optional<BlockId> parent;
                if (expected) parent = tree.parent(*expected);
                auto evicted = evict_one(tree, policy);
                CHECK(evicted == expected);
                if (!evicted) { if (!prefixes.empty()) ++starved; continue; }
                for (auto it = prefixes.begin(); it != prefixes.end(); ++it)
                    if (it->second == *evicted) { prefixes.erase(it); break; }
                ranks.erase(*evicted);
                ++evictions;
                if (parent && tree.is_evictable(*parent)) ++promoted;
            }
            auto model = model_victim();
            CHECK(policy.victim() == model.first);
            CHECK(policy.evictable_count() == model.second);
            CHECK(tree.size() == prefixes.size());
            CHECK(tree.reclaimable_count() == scan_reclaimable(pool, tree));
            CHECK((tree.reclaimable_count() > 0) == (model.second > 0));
            if (step % 101 == 0)
                CHECK(policy.check_invariants() && tree.check_invariants() && pool.check_invariants());
        }
        CHECK(evictions > 0 && starved > 0 && promoted > 0 && hits > 0);
        // Cascade from the final state, requests still pinned: evict_one succeeds
        // exactly reclaimable_count() times, one block fewer each time.
        Ids all;
        for (const auto& entry : prefixes) all.push_back(entry.second);
        const auto reclaimable = tree.reclaimable_count();
        CHECK(drain_checked(pool, tree, policy, all).size() == reclaimable);
        CHECK(tree.size() == prefixes.size() - reclaimable);
        for (auto& request : requests) finish(*request, tree, policy);
        CHECK(tree.reclaimable_count() == tree.size()); // nothing held any more
        CHECK(drain(tree, policy).size() == prefixes.size() - reclaimable && tree.size() == 0);
    }
    CHECK(pool.free_count() == pool.capacity());
}

int main() {
    try {
        api_tests(); std::cout << "PASS policy bookkeeping and errors\n";
        // Inserted A, B, C; A accessed. Indices into {A, B, C}.
        access_order<LRUPolicy>({1, 2, 0}, {1, 2, 0});
        access_order<FIFOPolicy>({0, 1, 2}, {0, 1, 2});
        std::cout << "PASS LRU evicts B, C, A and FIFO evicts A, B, C after accessing A\n";
        pinned_leaf<LRUPolicy>();
        pinned_leaf<FIFOPolicy>();
        std::cout << "PASS leaves held by a request are not evicted\n";
        parent_recheck<LRUPolicy>();
        parent_recheck<FIFOPolicy>();
        std::cout << "PASS parent re-checked after its last child is evicted\n";
        compressed_edges<LRUPolicy>(true);
        compressed_edges<FIFOPolicy>(false);
        std::cout << "PASS compressed edges evict one block at a time; LRU B2,B1,A2,A1 vs FIFO A2,A1,B2,B1\n";
        for (unsigned seed : {538u, 2026u, 17u}) {
            random_tests<LRUPolicy>(seed, 6000, true);
            random_tests<FIFOPolicy>(seed, 6000, false);
        }
        std::cout << "PASS 36000 seeded LRU/FIFO operations against a scan-based model;\n"
                     "     reclaimable count equals the scan, and each cascade evicts exactly that many\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
