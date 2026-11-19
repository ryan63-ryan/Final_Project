#pragma once

// ============================================================================
// PROPOSAL - Bowen to review.
//
// Written by Yingtao as the interface the KVCacheManager would like to call.
// Declarations and comments only: there is no implementation, nothing includes
// this header yet, and every name and signature here is open to change.
// The concrete classes (LRUPolicy, FIFOPolicy) are not declared here; they are
// Bowen's design, presumably built on MinHeap keyed by BlockId.
//
// Open questions for Bowen:
//   1. Who decides that a block is evictable (indexed, references == 1, leaf)?
//      Below, the manager decides and reports it through set_evictable(), so
//      the policy only orders blocks. If you would rather have the policy query
//      the pool and tree itself, set_evictable() can go away.
//   2. The manager's capacity check needs "how many blocks could be evicted in
//      total", including parents that become leaves once their children go.
//      evictable_count() below counts only blocks evictable right now. Should
//      the manager compute the larger number, or should the tree or policy?
// ============================================================================

#include "kvcache/block_pool.h"
#include "kvcache/config.h"

#include <cstddef>
#include <memory>
#include <optional>

namespace kvcache {

// Chooses which cached block to evict. The policy only ranks blocks; it never
// touches the pool or the index. To evict, the manager calls victim(), then
// RadixTree::erase(block), then on_erase(block).
class EvictionPolicy {
public:
    virtual ~EvictionPolicy() = default;

    // `block` entered the prefix index. It starts as not evictable.
    virtual void on_insert(BlockId block) = 0;
    // `block` was reused by an ADMIT (cache hit). LRU refreshes its rank;
    // FIFO ignores this.
    virtual void on_access(BlockId block) = 0;
    // `block` left the prefix index. The policy forgets it.
    virtual void on_erase(BlockId block) = 0;

    // The manager reports when a tracked block becomes evictable (no request
    // holds it and it is a leaf) or stops being evictable.
    virtual void set_evictable(BlockId block, bool evictable) = 0;

    // The evictable block to remove next, or nullopt if there is none.
    // LRU: least recently inserted-or-accessed. FIFO: earliest inserted.
    // Ties break toward the smaller block ID, as in MinHeap.
    virtual std::optional<BlockId> victim() const = 0;
    virtual std::size_t evictable_count() const = 0;

    virtual bool check_invariants() const = 0;
};

// Creates the policy named in the config file (policy=lru | fifo).
std::unique_ptr<EvictionPolicy> make_policy(PolicyKind kind);

} // namespace kvcache
