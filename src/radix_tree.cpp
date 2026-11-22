#include "kvcache/radix_tree.h"

#include <stdexcept>
#include <utility>

namespace kvcache {

RadixTree::RadixTree(BlockPool& pool) : pool_(&pool), root_(std::make_unique<Node>()) {}

RadixTree::~RadixTree() { release_all(); }

RadixTree::RadixTree(RadixTree&& other) noexcept
    : pool_(other.pool_), root_(std::move(other.root_)) {
    index_.swap(other.index_);
}

RadixTree& RadixTree::operator=(RadixTree&& other) noexcept {
    if (this != &other) {
        release_all();
        pool_ = other.pool_;
        root_.swap(other.root_);
        index_.swap(other.index_);
    }
    return *this;
}

void RadixTree::release_all() noexcept {
    if (!root_) return; // moved-from
    for (const auto& entry : index_) pool_->release(entry.first);
    index_.clear();
    root_->children.clear();
}

std::vector<BlockId> RadixTree::match(const std::vector<Token>& tokens) const {
    const auto width = pool_->tokens_per_block();
    std::vector<BlockId> result;
    const Node* node = root_.get();
    std::vector<Token> key(width);
    for (std::size_t start = 0; tokens.size() - start >= width; start += width) {
        key.assign(tokens.begin() + start, tokens.begin() + start + width);
        auto it = node->children.find(key);
        if (it == node->children.end()) break;
        node = it->second.get();
        result.push_back(node->block);
    }
    return result;
}

bool RadixTree::insert(const std::vector<Token>& prefix, BlockId block) {
    const auto width = pool_->tokens_per_block();
    if (prefix.empty() || prefix.size() % width != 0)
        throw std::invalid_argument("prefix must be a positive multiple of tokens_per_block");
    if (!pool_->full(block)) throw std::invalid_argument("indexed blocks must be full");

    Node* node = root_.get();
    const auto last = prefix.size() - width;
    std::vector<Token> key(width);
    for (std::size_t start = 0; start < last; start += width) {
        key.assign(prefix.begin() + start, prefix.begin() + start + width);
        auto it = node->children.find(key);
        if (it == node->children.end())
            throw std::invalid_argument("parent prefix is not indexed");
        node = it->second.get();
    }
    key.assign(prefix.begin() + last, prefix.end());
    if (node->children.count(key) != 0) return false;
    if (index_.count(block) != 0) throw std::invalid_argument("block is already indexed");

    auto child = std::make_unique<Node>();
    child->parent = node;
    child->block = block;
    Node* raw = child.get();
    pool_->retain(block);
    auto added = node->children.end();
    try {
        added = node->children.emplace(std::move(key), std::move(child)).first;
        raw->self = added;
        index_.emplace(block, raw); // allocates a hash node; may throw
    } catch (...) {
        // Undo in reverse order so a throw leaves the tree and counts unchanged.
        if (added != node->children.end()) node->children.erase(added);
        pool_->release(block);
        throw;
    }
    return true;
}

bool RadixTree::erase(BlockId block) {
    auto it = index_.find(block);
    if (it == index_.end() || !it->second->children.empty()) return false;
    Node* node = it->second;
    index_.erase(it);
    node->parent->children.erase(node->self); // destroys node
    pool_->release(block);
    return true;
}

bool RadixTree::contains(BlockId block) const { return index_.count(block) != 0; }

bool RadixTree::is_leaf(BlockId block) const {
    auto it = index_.find(block);
    return it != index_.end() && it->second->children.empty();
}

bool RadixTree::is_evictable(BlockId block) const {
    return is_leaf(block) && pool_->references(block) == 1;
}

std::optional<BlockId> RadixTree::parent(BlockId block) const {
    auto it = index_.find(block);
    if (it == index_.end() || it->second->parent == root_.get()) return std::nullopt;
    return it->second->parent->block;
}

bool RadixTree::check_node(const Node& node, std::size_t& count) const {
    for (const auto& entry : node.children) {
        const Node& child = *entry.second;
        if (child.parent != &node || child.self->second.get() != &child ||
            entry.first.size() != pool_->tokens_per_block())
            return false;
        if (!pool_->allocated(child.block) || !pool_->full(child.block)) return false;
        auto it = index_.find(child.block);
        if (it == index_.end() || it->second != &child) return false; // also catches duplicates
        ++count;
        if (!check_node(child, count)) return false;
    }
    return true;
}

bool RadixTree::check_invariants() const {
    if (!root_ || root_->parent != nullptr) return false;
    std::size_t count = 0;
    return check_node(*root_, count) && count == index_.size();
}

} // namespace kvcache
