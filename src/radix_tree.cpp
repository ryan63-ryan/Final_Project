#include "kvcache/radix_tree.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace kvcache {

namespace {

// Makes room for `extra` more elements with geometric growth, so repeatedly
// extending an edge stays amortized O(1) per element. Changes nothing on throw.
template <class T>
void reserve_more(std::vector<T>& v, std::size_t extra) {
    if (v.capacity() - v.size() >= extra) return;
    v.reserve(std::max(v.size() + extra, 2 * v.capacity()));
}

} // namespace

RadixTree::RadixTree(BlockPool& pool) : pool_(&pool), root_(std::make_unique<Node>()) {}

RadixTree::~RadixTree() { release_all(); }

RadixTree::RadixTree(RadixTree&& other) noexcept
    : pool_(other.pool_), root_(std::move(other.root_)),
      nodes_(std::exchange(other.nodes_, 0)) {
    index_.swap(other.index_);
}

RadixTree& RadixTree::operator=(RadixTree&& other) noexcept {
    if (this != &other) {
        release_all();
        pool_ = other.pool_;
        root_.swap(other.root_);
        index_.swap(other.index_);
        std::swap(nodes_, other.nodes_);
    }
    return *this;
}

void RadixTree::release_all() noexcept {
    if (!root_) return; // moved-from
    for (const auto& entry : index_) pool_->release(entry.first);
    index_.clear();
    root_->children.clear();
    nodes_ = 0;
}

std::vector<BlockId> RadixTree::match(const std::vector<Token>& tokens) const {
    const auto width = pool_->tokens_per_block();
    std::vector<BlockId> result;
    const Node* node = root_.get();
    std::size_t pos = 0;
    std::vector<Token> key(width);
    while (tokens.size() - pos >= width) {
        key.assign(tokens.begin() + pos, tokens.begin() + pos + width);
        auto it = node->children.find(key);
        if (it == node->children.end()) break;
        node = it->second.get();
        // The key matched block 0; compare the rest of the edge block by block and
        // keep every block that matched, even if the edge ends in a mismatch.
        result.push_back(node->blocks[0]);
        pos += width;
        for (std::size_t i = 1; i < node->blocks.size(); ++i) {
            if (tokens.size() - pos < width ||
                !std::equal(tokens.begin() + pos, tokens.begin() + pos + width,
                            node->tokens.begin() + i * width))
                return result;
            result.push_back(node->blocks[i]);
            pos += width;
        }
    }
    return result;
}

std::pair<RadixTree::Node*, std::size_t>
RadixTree::locate(const std::vector<Token>& prefix, std::size_t length) const {
    const auto width = pool_->tokens_per_block();
    Node* node = root_.get();
    std::size_t used = 0; // blocks of `node` on the path so far
    std::vector<Token> key(width);
    for (std::size_t pos = 0; pos < length; pos += width) {
        if (used == node->blocks.size()) {
            key.assign(prefix.begin() + pos, prefix.begin() + pos + width);
            auto it = node->children.find(key);
            if (it == node->children.end())
                throw std::invalid_argument("parent prefix is not indexed");
            node = it->second.get();
            used = 1;
        } else if (std::equal(prefix.begin() + pos, prefix.begin() + pos + width,
                              node->tokens.begin() + used * width)) {
            ++used;
        } else {
            throw std::invalid_argument("parent prefix is not indexed");
        }
    }
    return {node, used};
}

bool RadixTree::insert(const std::vector<Token>& prefix, BlockId block) {
    const auto width = pool_->tokens_per_block();
    if (prefix.empty() || prefix.size() % width != 0)
        throw std::invalid_argument("prefix must be a positive multiple of tokens_per_block");
    if (!pool_->full(block)) throw std::invalid_argument("indexed blocks must be full");

    const auto last = prefix.size() - width;
    auto [node, used] = locate(prefix, last);
    const Token* chunk = prefix.data() + last;

    if (used < node->blocks.size()) {
        // The parent prefix ends inside an edge: the next block there either is
        // this prefix already, or the new block branches off and splits the edge.
        if (std::equal(chunk, chunk + width, node->tokens.begin() + used * width)) return false;
        if (index_.count(block) != 0) throw std::invalid_argument("block is already indexed");
        split_insert(node, used, chunk, block);
        return true;
    }
    if (node->children.count(std::vector<Token>(chunk, chunk + width)) != 0) return false;
    if (index_.count(block) != 0) throw std::invalid_argument("block is already indexed");
    if (node != root_.get() && node->children.empty()) extend(node, chunk, block);
    else add_child(node, chunk, block);
    return true;
}

// Appends `block` to the edge of a childless node.
void RadixTree::extend(Node* node, const Token* chunk, BlockId block) {
    const auto width = pool_->tokens_per_block();
    reserve_more(node->tokens, width);
    reserve_more(node->blocks, 1);
    pool_->retain(block);
    try {
        index_.emplace(block, Slot{node, node->blocks.size()}); // allocates; may throw
    } catch (...) {
        pool_->release(block);
        throw;
    }
    // Capacity is reserved: these cannot throw.
    node->tokens.insert(node->tokens.end(), chunk, chunk + width);
    node->blocks.push_back(block);
}

// Adds a one-block edge below the end of `node`'s edge (or below the root).
void RadixTree::add_child(Node* node, const Token* chunk, BlockId block) {
    const auto width = pool_->tokens_per_block();
    auto child = std::make_unique<Node>();
    child->parent = node;
    child->tokens.assign(chunk, chunk + width);
    child->blocks.push_back(block);
    std::vector<Token> key(chunk, chunk + width);
    Node* raw = child.get();
    pool_->retain(block);
    auto added = node->children.end();
    try {
        added = node->children.emplace(std::move(key), std::move(child)).first;
        raw->self = added;
        index_.emplace(block, Slot{raw, 0}); // allocates a hash node; may throw
    } catch (...) {
        // Undo in reverse order so a throw leaves the tree and counts unchanged.
        if (added != node->children.end()) node->children.erase(added);
        pool_->release(block);
        throw;
    }
    ++nodes_;
}

// Splits `node`'s edge after its first `keep` blocks (0 < keep < size) and adds
// `block` as a sibling of the split-off tail. Every allocation happens before the
// tree is touched; the commit phase only swaps, shrinks and reassigns.
void RadixTree::split_insert(Node* node, std::size_t keep, const Token* chunk, BlockId block) {
    const auto width = pool_->tokens_per_block();
    const auto cut = node->tokens.begin() + static_cast<std::ptrdiff_t>(keep * width);

    // Prepare: the tail node, the new leaf and their map entries, in a local map.
    auto tail = std::make_unique<Node>();
    tail->parent = node;
    tail->tokens.assign(cut, node->tokens.end());
    tail->blocks.assign(node->blocks.begin() + static_cast<std::ptrdiff_t>(keep),
                        node->blocks.end());
    auto leaf = std::make_unique<Node>();
    leaf->parent = node;
    leaf->tokens.assign(chunk, chunk + width);
    leaf->blocks.push_back(block);
    Node* tail_raw = tail.get();
    Node* leaf_raw = leaf.get();
    Children fresh;
    auto tail_it = fresh.emplace(std::vector<Token>(cut, cut + static_cast<std::ptrdiff_t>(width)),
                                 std::move(tail)).first;
    auto leaf_it = fresh.emplace(std::vector<Token>(chunk, chunk + width), std::move(leaf)).first;
    pool_->retain(block);
    try {
        index_.emplace(block, Slot{leaf_raw, 0}); // allocates a hash node; may throw
    } catch (...) {
        pool_->release(block); // `fresh` frees the unused nodes
        throw;
    }

    // Commit: nothing below allocates or throws.
    tail_raw->children.swap(node->children); // swap keeps the children's iterators valid
    for (auto& entry : tail_raw->children) entry.second->parent = tail_raw;
    for (std::size_t i = 0; i < tail_raw->blocks.size(); ++i)
        index_.find(tail_raw->blocks[i])->second = Slot{tail_raw, i};
    node->tokens.erase(cut, node->tokens.end());
    node->blocks.erase(node->blocks.begin() + static_cast<std::ptrdiff_t>(keep), node->blocks.end());
    node->children.swap(fresh); // node had no children left; now {tail, leaf}
    tail_raw->self = tail_it;
    leaf_raw->self = leaf_it;
    nodes_ += 2;
}

bool RadixTree::erase(BlockId block) {
    auto it = index_.find(block);
    if (it == index_.end()) return false;
    Node* node = it->second.node;
    if (it->second.offset + 1 != node->blocks.size() || !node->children.empty()) return false;
    index_.erase(it);
    if (node->blocks.size() == 1) {
        node->parent->children.erase(node->self); // destroys node
        --nodes_;
    } else {
        // Drop the last block of the edge; no merge with a single remaining child.
        node->blocks.pop_back();
        node->tokens.resize(node->tokens.size() - pool_->tokens_per_block());
    }
    pool_->release(block);
    return true;
}

bool RadixTree::contains(BlockId block) const { return index_.count(block) != 0; }

bool RadixTree::is_leaf(BlockId block) const {
    auto it = index_.find(block);
    if (it == index_.end()) return false;
    const Node* node = it->second.node;
    return it->second.offset + 1 == node->blocks.size() && node->children.empty();
}

bool RadixTree::is_evictable(BlockId block) const {
    return is_leaf(block) && pool_->references(block) == 1;
}

std::optional<BlockId> RadixTree::parent(BlockId block) const {
    auto it = index_.find(block);
    if (it == index_.end()) return std::nullopt;
    const Node* node = it->second.node;
    if (it->second.offset > 0) return node->blocks[it->second.offset - 1];
    if (node->parent == root_.get()) return std::nullopt;
    return node->parent->blocks.back();
}

bool RadixTree::check_node(const Node& node, std::size_t& blocks, std::size_t& nodes) const {
    const auto width = pool_->tokens_per_block();
    for (const auto& entry : node.children) {
        const Node& child = *entry.second;
        if (child.parent != &node || child.self->second.get() != &child) return false;
        if (child.blocks.empty() || child.tokens.size() != child.blocks.size() * width ||
            entry.first.size() != width ||
            !std::equal(entry.first.begin(), entry.first.end(), child.tokens.begin()))
            return false;
        for (std::size_t i = 0; i < child.blocks.size(); ++i) {
            BlockId id = child.blocks[i];
            if (!pool_->allocated(id) || !pool_->full(id)) return false;
            auto it = index_.find(id);
            // Also catches duplicates: one block has exactly one slot.
            if (it == index_.end() || it->second.node != &child || it->second.offset != i)
                return false;
            ++blocks;
        }
        ++nodes;
        if (!check_node(child, blocks, nodes)) return false;
    }
    return true;
}

bool RadixTree::check_invariants() const {
    if (!root_ || root_->parent != nullptr || !root_->blocks.empty() || !root_->tokens.empty())
        return false;
    std::size_t blocks = 0, nodes = 0;
    return check_node(*root_, blocks, nodes) && blocks == index_.size() && nodes == nodes_;
}

} // namespace kvcache
