#pragma once

// ============================================================================
// PROPOSAL - Bowen to review.
//
// Written by Yingtao as the interface the KVCacheManager would like to call.
// Declarations and comments only: there is no implementation, nothing includes
// this header yet, and every name and signature here is open to change.
// Private members are left out on purpose; they are Bowen's design.
//
// Open questions for Bowen:
//   1. Is one tree node per full block acceptable to you, or do you want
//      compressed edges covering several blocks? The interface below only
//      talks about blocks, so either should fit.
//   2. If two requests fill a block with the same prefix independently, the
//      second insert() returns false and that block stays private. OK?
//   3. Should match() take the token vector (as below) or the manager's
//      per-block prefix hashes?
// ============================================================================

#include "kvcache/block_pool.h"
#include "kvcache/types.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace kvcache {

// Prefix index over FULL blocks. A path from the root spells a token prefix in
// steps of tokens_per_block tokens; each node names the block that stores the
// last step of that prefix.
//
// Ownership (from the storage README): the index holds exactly one pool
// reference for every block it contains. insert() retains, erase() releases.
// So for an indexed block, references(id) == 1 means "cached, no request uses it".
// The pool must outlive the tree.
class RadixTree {
public:
    explicit RadixTree(BlockPool& pool);
    ~RadixTree(); // releases every reference the index still holds
    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;

    // Longest cached run of full blocks at the start of `tokens`.
    // Returns the block IDs in order; k IDs means the first
    // k * tokens_per_block tokens are cached. Changes no reference count:
    // the manager pins the result with BlockTable::from_prefix.
    // Used by: ADMIT.
    std::vector<BlockId> match(const std::vector<Token>& tokens) const;

    // Records that `block` stores the last tokens_per_block tokens of `prefix`.
    // Requires: prefix.size() is a positive multiple of tokens_per_block, the
    // block is full, and the prefix one block shorter is already indexed (or
    // is empty). On success retains `block` and returns true. Returns false and
    // changes nothing if this prefix is already indexed.
    // Used by: ADMIT and APPEND, each time a request fills a block.
    bool insert(const std::vector<Token>& prefix, BlockId block);

    // Removes a leaf block from the index and releases its reference.
    // Returns false and changes nothing if the block is not indexed or still
    // has children (evicting it would orphan longer cached prefixes).
    // Used by: eviction.
    bool erase(BlockId block);

    bool contains(BlockId block) const;
    bool is_leaf(BlockId block) const; // false if not indexed
    // Parent block in the tree; nullopt for a first-level block or if not
    // indexed. After a leaf is evicted its parent may become a leaf.
    std::optional<BlockId> parent(BlockId block) const;
    std::size_t size() const noexcept; // number of indexed blocks

    // True if every node's block is allocated and full, every child prefix
    // extends its parent's, and no block appears twice.
    bool check_invariants() const;
};

} // namespace kvcache
