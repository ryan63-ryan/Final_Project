#pragma once

#include "kvcache/block_pool.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace kvcache {

// Same definition as the manager's types.h; move it there once both are merged.
using Token = std::int32_t;

// Prefix index over FULL blocks. A path from the root spells a token prefix in
// steps of tokens_per_block tokens; each node names the block that stores the
// last step of that prefix. One node per block: edges are not compressed.
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
    std::size_t size() const noexcept { return index_.size(); }

    // Every node's block is allocated, full and referenced; keys have
    // tokens_per_block tokens; parent links and the block index agree with the
    // tree; no block appears twice.
    bool check_invariants() const;

private:
    struct Node;
    using Children = std::map<std::vector<Token>, std::unique_ptr<Node>>;
    struct Node {
        Node* parent = nullptr; // nullptr for the root
        BlockId block = 0;      // unused for the root
        Children children;
        Children::iterator self; // this node's entry in parent->children
    };

    void release_all() noexcept;
    bool check_node(const Node& node, std::size_t& count) const;

    BlockPool* pool_;
    std::unique_ptr<Node> root_;
    std::unordered_map<BlockId, Node*> index_;
};

} // namespace kvcache
