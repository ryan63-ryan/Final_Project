// Allocation-failure tests for RadixTree::insert. This binary replaces the global
// operator new so the n-th allocation after arm(n) throws std::bad_alloc. For each
// kind of insert (extend an edge, new edge, split mid-edge, split at the first
// block), every allocation inside insert is failed in turn, and the tree, its
// block index, the node count and every reference count must equal the state
// before the call, and so must the reclaimable count. Kept in its own
// executable so no other test sees the hook.
//
// The number of allocations per insert depends on the standard library (vector
// capacity growth, container internals), so the test asserts no totals. It asserts
// coverage of the rollback that releases the new block's reference instead: each
// injected failure records whether insert had already retained the block, and
// every scenario requires a minimum count of such failures, which depends only on
// how many allocations radix_tree.cpp makes after retain (index_.emplace, plus the
// children map insert in add_child).

#include "kvcache/block_table.h"
#include "kvcache/radix_tree.h"

#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
long fail_after = -1; // -1: never fail
// The block being inserted; a failure while it has a second reference happened
// after RadixTree::insert retained it.
const kvcache::BlockPool* watched_pool = nullptr;
kvcache::BlockId watched_block = 0;
std::size_t faults_after_retain = 0;
void arm(long n) { fail_after = n; }
void disarm() { fail_after = -1; }
} // namespace

void* operator new(std::size_t size) {
    if (fail_after == 0) {
        fail_after = -1;
        if (watched_pool && watched_pool->references(watched_block) > 1) ++faults_after_retain;
        throw std::bad_alloc();
    }
    if (fail_after > 0) --fail_after;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace kvcache;
using Tokens = std::vector<Token>;
using Ids = std::vector<BlockId>;

void check(bool ok, const char* condition, int line) {
    if (!ok) throw std::runtime_error("line " + std::to_string(line) + ": " + condition);
}
#define CHECK(condition) check((condition), #condition, __LINE__)

BlockId make_block(BlockPool& pool) {
    auto id = pool.allocate();
    if (!id) throw std::runtime_error("test pool exhausted");
    for (std::size_t i = 0; i < pool.tokens_per_block(); ++i)
        pool.append(*id, Payload(pool.bytes_per_token(), static_cast<std::uint8_t>(*id + 1)));
    return *id;
}

BlockId cache(BlockPool& pool, RadixTree& tree, const Tokens& prefix) {
    BlockId id = make_block(pool);
    CHECK(tree.insert(prefix, id));
    pool.release(id);
    tree.refresh(id);
    return id;
}

// A request holds one indexed block (released by run() after the attempt).
BlockId hold(BlockPool& pool, RadixTree& tree, BlockId id) {
    pool.retain(id);
    tree.refresh(id);
    return id;
}

// Everything observable about the tree and the pool.
struct Snapshot {
    std::size_t size, nodes, free, reclaimable;
    std::vector<std::size_t> refs;            // per block ID, 0 if free
    std::vector<int> indexed, leaf;           // per block ID
    std::vector<std::optional<BlockId>> parent;
    std::vector<Ids> matches;                 // per probe
    bool operator==(const Snapshot& o) const {
        return size == o.size && nodes == o.nodes && free == o.free &&
               reclaimable == o.reclaimable && refs == o.refs &&
               indexed == o.indexed && leaf == o.leaf && parent == o.parent && matches == o.matches;
    }
};

Snapshot snapshot(const BlockPool& pool, const RadixTree& tree, const std::vector<Tokens>& probes) {
    Snapshot s{tree.size(), tree.node_count(), pool.free_count(), tree.reclaimable_count(),
               {}, {}, {}, {}, {}};
    for (BlockId id = 0; id < pool.capacity(); ++id) {
        s.refs.push_back(pool.allocated(id) ? pool.references(id) : 0);
        s.indexed.push_back(tree.contains(id));
        s.leaf.push_back(tree.is_leaf(id));
        s.parent.push_back(tree.parent(id));
    }
    for (const auto& probe : probes) s.matches.push_back(tree.match(probe));
    return s;
}

const std::vector<Tokens> kProbes = {
    {1, 1}, {1, 1, 2, 2}, {1, 1, 2, 2, 3, 3}, {1, 1, 2, 2, 3, 3, 4, 4},
    {1, 1, 2, 2, 3, 3, 5, 5}, {1, 1, 2, 2, 6, 6}, {1, 1, 7, 7}, {1, 1, 8, 8},
    {9, 9}, {1, 1, 2, 9}, {1, 1, 2, 2, 3},
};

struct Scenario {
    const char* name;
    std::size_t nodes_added;          // node_count() change on success
    std::size_t reclaimable_after;    // reclaimable_count() on success (new block held)
    std::size_t min_faults_after_retain; // allocations insert makes after retain
};

// Fails allocation 0, 1, 2, ... of tree.insert(prefix, block) until one call
// succeeds, so every allocation insert makes is failed once, however many there
// are. Every attempt starts from a freshly built pool and tree: a failed attempt
// may keep spare vector capacity (invisible, but it would shift the allocation
// sequence of a retry on the same tree and skip a failure point).
// `setup` returns the blocks it holds for a request; they are let go after each attempt.
void run(const Scenario& scenario, Ids (*setup)(BlockPool&, RadixTree&), const Tokens& prefix,
         const Ids& expected_match) {
    std::size_t faults = 0;
    faults_after_retain = 0;
    for (long n = 0;; ++n) {
        BlockPool pool(12, 2, 1);
        bool inserted = false;
        Ids held;
        {
            RadixTree tree(pool);
            held = setup(pool, tree);
            CHECK(tree.check_reclaimable());
            auto nodes = tree.node_count();
            BlockId block = make_block(pool);
            auto before = snapshot(pool, tree, kProbes);
            watched_pool = &pool;
            watched_block = block;
            arm(n);
            try {
                inserted = tree.insert(prefix, block);
                disarm();
                watched_pool = nullptr;
            } catch (const std::bad_alloc&) {
                disarm();
                watched_pool = nullptr;
                ++faults;
                CHECK(snapshot(pool, tree, kProbes) == before);
                CHECK(tree.check_invariants() && tree.check_reclaimable() && pool.check_invariants());
                CHECK(pool.references(block) == 1 && !tree.contains(block));
            }
            if (inserted) {
                CHECK(tree.contains(block) && pool.references(block) == 2 && tree.check_invariants());
                CHECK(tree.check_reclaimable() && tree.reclaimable_count() == scenario.reclaimable_after);
                CHECK(tree.node_count() == nodes + scenario.nodes_added);
                auto got = tree.match(prefix);
                CHECK(got.size() == expected_match.size() + 1 && got.back() == block);
                got.pop_back();
                CHECK(got == expected_match);
            }
            pool.release(block);
            tree.refresh(block);
            CHECK(tree.check_reclaimable());
        }
        for (BlockId id : held) pool.release(id);
        CHECK(pool.free_count() == pool.capacity()); // nothing leaked by this attempt
        if (inserted) break;
    }
    // Failures after retain exercise the rollback that releases the reference.
    CHECK(faults_after_retain >= scenario.min_faults_after_retain);
    CHECK(faults > faults_after_retain); // and failures before retain occurred too
    std::cout << "PASS " << scenario.name << ": " << faults << " injected allocation failures ("
              << faults_after_retain << " after retain), all rolled back\n";
}

int main() {
    try {
        // Edge [A] grows to [A|new]. After retain: index_.emplace.
        run({"extend an edge", 0, 0, 1},
            [](BlockPool& p, RadixTree& t) { cache(p, t, {1, 1}); return Ids{}; },
            {1, 1, 2, 2}, Ids{0});
        // [A] has children, so the new block becomes a new edge below it.
        // After retain: the children map insert and index_.emplace.
        run({"new edge below an edge with children", 1, 2, 2},
            [](BlockPool& p, RadixTree& t) {
                cache(p, t, {1, 1}); cache(p, t, {1, 1, 2, 2}); cache(p, t, {1, 1, 7, 7});
                return Ids{};
            },
            {1, 1, 8, 8}, Ids{0});
        // [A|B|C] -> {[D], [E]}; branching after B splits it into [A|B] -> {[C] -> {D, E}, new}.
        // After retain: index_.emplace (all other allocations come before it).
        run({"split mid-edge, tail keeps its children", 2, 3, 1},
            [](BlockPool& p, RadixTree& t) {
                cache(p, t, {1, 1}); cache(p, t, {1, 1, 2, 2}); cache(p, t, {1, 1, 2, 2, 3, 3});
                cache(p, t, {1, 1, 2, 2, 3, 3, 4, 4}); cache(p, t, {1, 1, 2, 2, 3, 3, 5, 5});
                return Ids{};
            },
            {1, 1, 2, 2, 6, 6}, Ids{0, 1});
        // [A|B|C] -> [A] -> {[B|C], new}. After retain: index_.emplace.
        run({"split after the first block", 2, 2, 1},
            [](BlockPool& p, RadixTree& t) {
                cache(p, t, {1, 1}); cache(p, t, {1, 1, 2, 2}); cache(p, t, {1, 1, 2, 2, 3, 3});
                return Ids{};
            },
            {1, 1, 7, 7}, Ids{0});
        // Held blocks on both sides of the cut: [A|B*|C|D*] -> {[E], [E']} splits
        // after B into [A|B*] -> {[C|D*] -> {[E], [E']}, new}. Only E and E' stay
        // reclaimable.
        run({"split with held blocks before and after the cut", 2, 2, 1},
            [](BlockPool& p, RadixTree& t) {
                cache(p, t, {1, 1});
                BlockId b = cache(p, t, {1, 1, 2, 2});
                cache(p, t, {1, 1, 2, 2, 3, 3});
                BlockId d = cache(p, t, {1, 1, 2, 2, 3, 3, 4, 4});
                cache(p, t, {1, 1, 2, 2, 3, 3, 4, 4, 5, 5});
                cache(p, t, {1, 1, 2, 2, 3, 3, 4, 4, 8, 8}); // [A|B|C|D] -> {[E], [E']}
                return Ids{hold(p, t, b), hold(p, t, d)};
            },
            {1, 1, 2, 2, 6, 6}, Ids{0, 1});
        // A held edge [A*|B] extended by a held block: B becomes stuck too.
        run({"extend below a held block", 0, 0, 1},
            [](BlockPool& p, RadixTree& t) {
                BlockId a = cache(p, t, {1, 1});
                cache(p, t, {1, 1, 2, 2});
                return Ids{hold(p, t, a)};
            },
            {1, 1, 2, 2, 3, 3}, Ids{0, 1});
    } catch (const std::exception& error) {
        disarm();
        watched_pool = nullptr;
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
