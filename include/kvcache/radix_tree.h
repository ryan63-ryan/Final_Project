#pragma once

#include "kvcache/block_pool.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <unordered_map>
#include <vector>

namespace kvcache {

// Same definition as the manager's types.h; move it there once both are merged.
using Token = std::int32_t;

// Prefix index over FULL blocks. A path from the root spells a token prefix in
// steps of tokens_per_block (B) tokens, and every step is stored in one block.
// The interface is per block: match/insert/erase/parent/is_leaf name single
// blocks, exactly as if each block had its own node.
//
// Internally edges are compressed: a node holds a run of k >= 1 blocks and their
// k*B tokens, and its children are keyed by their first block's B tokens. Edges
// begin and end on block boundaries. Inserting below the last block of a
// childless edge extends that edge; inserting below a block in the middle of an
// edge splits the edge after that block. Erasing removes the last block of a
// childless edge, and the node once its edge is empty. Nodes are not merged
// after an erase, so a node may be left with a single child: compression is
// not maximal after eviction, which affects node count only, never results.
//
// The index owns exactly one pool reference per indexed block: insert() retains,
// erase() and destruction release. For an indexed block, references(id) == 1
// means "cached, no request uses it". The pool must outlive the tree.
class RadixTree {
public:
    explicit RadixTree(BlockPool& pool);
    ~RadixTree(); // releases every reference the index still holds
    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;
    // A moved-from tree may only be destroyed or assigned to.
    RadixTree(RadixTree&& other) noexcept;
    RadixTree& operator=(RadixTree&& other) noexcept;

    // Longest cached run of full blocks at the start of `tokens`. k IDs means the
    // first k * tokens_per_block tokens are cached; a trailing partial block never
    // matches. Changes no reference count: pin the result with
    // BlockTable::from_prefix before anything can be erased.
    std::vector<BlockId> match(const std::vector<Token>& tokens) const;

    // Records that `block` stores the last tokens_per_block tokens of `prefix`.
    // Returns false and changes nothing if this prefix is already indexed.
    // On success retains `block` and returns true.
    // Throws std::invalid_argument if prefix.size() is not a positive multiple of
    // tokens_per_block, the block is not full, the block is already indexed, or
    // the prefix one block shorter is not indexed; std::out_of_range if the
    // block is not allocated. A throw changes nothing.
    bool insert(const std::vector<Token>& prefix, BlockId block);

    // Removes a leaf block and releases its reference. Returns false and changes
    // nothing if the block is not indexed or still has children.
    bool erase(BlockId block);

    bool contains(BlockId block) const;
    bool is_leaf(BlockId block) const; // false if not indexed
    // Indexed leaf whose only reference is the index's own: no request uses it
    // and removing it orphans nothing.
    bool is_evictable(BlockId block) const;
    // Parent block; nullopt for a first-level block or if not indexed.
    std::optional<BlockId> parent(BlockId block) const;
    std::size_t size() const noexcept { return index_.size(); } // indexed blocks
    std::size_t node_count() const noexcept { return nodes_; }  // edges (root excluded)

    // Reclaimable blocks. An indexed block is HELD if pool.references(block) > 1
    // (a request uses it besides the index) and STUCK if it is held or has a
    // stuck child: it cannot become an evictable leaf while its holders stay.
    // Every other indexed block is reclaimable, i.e. no block in its subtree,
    // itself included, is held; evicting leaves one at a time frees exactly
    // these. The tree cannot observe the pool, so the held state of each block
    // is the one seen at its insert or at the last refresh(): after retaining or
    // releasing an indexed block, the caller must call refresh(block).

    // Number of reclaimable blocks. O(1).
    std::size_t reclaimable_count() const noexcept { return index_.size() - stuck_; }

    // Re-reads pool.references(block) and updates the count. No-op if the block
    // is not indexed or its held state did not change. Changes no reference.
    // Cost: O(1) plus a walk up the tree that continues only while an ancestor's
    // stuck state flips, plus, when clearing the highest held block of an edge,
    // a scan down that edge to the next held block.
    void refresh(BlockId block);

    // Every indexed block is allocated and full; every non-root edge holds
    // k >= 1 blocks and k*B tokens, and its key is its first B tokens; parent
    // links, map positions, the block index (node and offset) and the node count
    // agree with the tree; no block appears twice. The reclaimable bookkeeping is
    // consistent with the held state recorded per block (whatever the pool says).
    bool check_invariants() const;

    // The recorded held state of every indexed block equals
    // pool.references(block) > 1, and reclaimable_count() equals a count
    // recomputed from the pool by a full traversal. True only if the caller has
    // called refresh() after every reference change of an indexed block.
    bool check_reclaimable() const;

private:
    struct Node;
    using Children = std::map<std::vector<Token>, std::unique_ptr<Node>>;
    struct Node {
        Node* parent = nullptr;      // nullptr for the root
        std::vector<Token> tokens;   // blocks.size() * B tokens; empty for the root
        std::vector<BlockId> blocks; // the edge's blocks in path order; empty for the root
        Children children;           // keyed by each child's first B tokens
        Children::iterator self;     // this node's entry in parent->children
        // Reclaimable bookkeeping. Held blocks make every block before them on
        // the edge stuck, and a stuck child makes the whole edge stuck, so the
        // stuck blocks of an edge are always a prefix of it (see stuck_blocks).
        std::vector<std::uint8_t> held; // per block: references > 1 when last seen
        std::size_t held_top = 0;       // offset of the last held block + 1; 0 if none
        std::size_t stuck_children = 0; // children whose first block is stuck
    };
    struct Slot {
        Node* node;
        std::size_t offset; // position of the block in node->blocks
    };

    // End of the indexed path spelling prefix[0, length): the node and how many of
    // its blocks are used (0 for the root). Throws std::invalid_argument if that
    // prefix is not indexed.
    std::pair<Node*, std::size_t> locate(const std::vector<Token>& prefix,
                                         std::size_t length) const;
    void extend(Node* node, const Token* chunk, BlockId block);
    void add_child(Node* node, const Token* chunk, BlockId block);
    void split_insert(Node* node, std::size_t keep, const Token* chunk, BlockId block);
    void release_all() noexcept;
    bool check_node(const Node& node, std::size_t& blocks, std::size_t& nodes,
                    std::size_t& stuck) const;
    bool recount(const Node& node, std::size_t& stuck, bool& node_stuck) const;

    // Reclaimable bookkeeping helpers.
    static std::size_t stuck_blocks(const Node& node) noexcept {
        return node.stuck_children > 0 ? node.blocks.size() : node.held_top;
    }
    static bool is_stuck(const Node& node) noexcept { // its first block is stuck
        return node.stuck_children > 0 || node.held_top > 0;
    }
    static void lower_top(Node& node) noexcept; // after clearing held bits
    // Call after changing node's held bits, held_top or stuck_children, given
    // stuck_blocks(node) and is_stuck(node) from before the change. Fixes stuck_
    // and walks up while an ancestor's stuck state flips.
    void settle(Node* node, std::size_t before, bool was) noexcept;

    BlockPool* pool_;
    std::unique_ptr<Node> root_;
    std::unordered_map<BlockId, Slot> index_;
    std::size_t nodes_ = 0;
    std::size_t stuck_ = 0; // indexed blocks that are stuck
};

} // namespace kvcache
