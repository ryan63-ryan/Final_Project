#pragma once

#include "kvcache/block_pool.h"
#include "kvcache/min_heap.h"
#include "kvcache/radix_tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace kvcache {

// Chooses which cached block to evict. The policy only ranks blocks; it never
// touches the pool or the index. The caller reports eligibility through
// set_evictable() (true only for RadixTree::is_evictable blocks), and
// evict_one() below performs one eviction.
// Calls naming an untracked block, or on_insert of a tracked one, throw
// std::invalid_argument and change nothing.
class EvictionPolicy {
public:
    virtual ~EvictionPolicy() = default;

    // `block` entered the prefix index. It starts as not evictable.
    virtual void on_insert(BlockId block) = 0;
    // `block` was reused by a cache hit. LRU refreshes its rank; FIFO ignores it.
    virtual void on_access(BlockId block) = 0;
    // `block` left the prefix index. The policy forgets it.
    virtual void on_erase(BlockId block) = 0;
    // A tracked block became evictable (indexed leaf, no request holds it) or
    // stopped being evictable. Repeating the current state is a no-op.
    virtual void set_evictable(BlockId block, bool evictable) = 0;

    // The evictable block to remove next, or nullopt if there is none.
    // LRU: least recently inserted-or-accessed. FIFO: earliest inserted.
    virtual std::optional<BlockId> victim() const = 0;
    virtual std::size_t evictable_count() const = 0;
    virtual bool check_invariants() const = 0;
};

// Shared bookkeeping: each tracked block has a unique rank from a monotonic
// clock, and only evictable blocks sit in the heap. Smallest rank is evicted.
class RankedPolicy : public EvictionPolicy {
public:
    void on_insert(BlockId block) override;
    void on_erase(BlockId block) override;
    void set_evictable(BlockId block, bool evictable) override;
    std::optional<BlockId> victim() const override;
    std::size_t evictable_count() const override { return evictable_.size(); }
    bool check_invariants() const override;

protected:
    void require_tracked(BlockId block) const;
    void refresh(BlockId block); // gives `block` the newest rank

private:
    std::uint64_t next_rank();

    std::uint64_t clock_ = 0;
    std::unordered_map<BlockId, std::uint64_t> rank_; // every tracked block
    MinHeap<std::uint64_t> evictable_;                 // evictable subset, keyed by BlockId
};

class LRUPolicy final : public RankedPolicy {
public:
    void on_access(BlockId block) override { refresh(block); }
};

class FIFOPolicy final : public RankedPolicy {
public:
    void on_access(BlockId block) override { require_tracked(block); }
};

// Evicts policy.victim(): erases it from the tree (releasing the index's
// reference), forgets it in the policy, then re-checks the parent, which may
// have just become an evictable leaf. Returns the evicted block, or nullopt if
// nothing is evictable. Throws std::logic_error and changes nothing if the
// victim is not RadixTree::is_evictable (a stale set_evictable call).
std::optional<BlockId> evict_one(RadixTree& tree, EvictionPolicy& policy);

} // namespace kvcache
