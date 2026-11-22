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

    // Every indexed block is allocated and full; every non-root edge holds
    // k >= 1 blocks and k*B tokens, and its key is its first B tokens; parent
    // links, map positions, the block index (node and offset) and the node count
    // agree with the tree; no block appears twice.
    bool check_invariants() const;

private:
    struct Node;
    using Children = std::map<std::vector<Token>, std::unique_ptr<Node>>;
    struct Node {
        Node* parent = nullptr;      // nullptr for the root
        std::vector<Token> tokens;   // blocks.size() * B tokens; empty for the root
        std::vector<BlockId> blocks; // the edge's blocks in path order; empty for the root
        Children children;           // keyed by each child's first B tokens
        Children::iterator self;     // this node's entry in parent->children
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
    bool check_node(const Node& node, std::size_t& blocks, std::size_t& nodes) const;

    BlockPool* pool_;
    std::unique_ptr<Node> root_;
    std::unordered_map<BlockId, Slot> index_;
    std::size_t nodes_ = 0;
};

} // namespace kvcache
