#include "kvcache/block_pool.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace kvcache {

BlockPool::BlockPool(std::size_t capacity, std::size_t tokens_per_block,
                     std::size_t bytes_per_token)
    : tokens_per_block_(tokens_per_block), bytes_per_token_(bytes_per_token) {
    if (capacity == 0 || tokens_per_block == 0 || bytes_per_token == 0)
        throw std::invalid_argument("pool dimensions must be positive");
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (tokens_per_block > limit / bytes_per_token ||
        capacity > limit / (tokens_per_block * bytes_per_token))
        throw std::overflow_error("pool payload size overflow");
    blocks_.reserve(capacity);
    free_.reserve(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        blocks_.emplace_back(tokens_per_block * bytes_per_token);
        free_.push_back(capacity - 1 - i);
    }
}

bool BlockPool::allocated(BlockId id) const noexcept {
    return id < blocks_.size() && blocks_[id].refs != 0;
}

const BlockPool::Block& BlockPool::get(BlockId id) const {
    if (!allocated(id)) throw std::out_of_range("block is not allocated");
    return blocks_[id];
}

BlockPool::Block& BlockPool::get(BlockId id) {
    if (!allocated(id)) throw std::out_of_range("block is not allocated");
    return blocks_[id];
}

std::optional<BlockId> BlockPool::allocate() {
    if (free_.empty()) return std::nullopt;
    BlockId id = free_.back();
    free_.pop_back();
    auto& block = blocks_[id];
    std::fill(block.data.begin(), block.data.end(), 0);
    block.used = 0;
    block.refs = 1;
    return id;
}

std::optional<BlockId> BlockPool::clone(BlockId source) {
    const auto& old = get(source);
    auto id = allocate();
    if (!id) return std::nullopt;
    auto& copy = blocks_[*id];
    std::copy(old.data.begin(), old.data.end(), copy.data.begin());
    copy.used = old.used;
    return id;
}

void BlockPool::retain(BlockId id) {
    auto& block = get(id);
    if (block.refs == std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("reference count overflow");
    ++block.refs;
}

void BlockPool::release(BlockId id) {
    auto& block = get(id);
    if (--block.refs == 0) {
        block.used = 0;
        free_.push_back(id); // Capacity was reserved in the constructor.
    }
}

void BlockPool::append(BlockId id, const Payload& token) {
    auto& block = get(id);
    if (token.size() != bytes_per_token_)
        throw std::invalid_argument("wrong token payload size");
    if (block.refs != 1)
        throw std::logic_error("shared blocks must be copied before writing");
    if (block.used == tokens_per_block_)
        throw std::logic_error("cannot append to a full block");
    auto offset = block.used * bytes_per_token_;
    std::copy(token.begin(), token.end(), block.data.begin() + offset);
    ++block.used;
}

Payload BlockPool::read_token(BlockId id, std::size_t index) const {
    const auto& block = get(id);
    if (index >= block.used) throw std::out_of_range("token is not stored");
    auto first = block.data.begin() + index * bytes_per_token_;
    return Payload(first, first + bytes_per_token_);
}

std::size_t BlockPool::references(BlockId id) const { return get(id).refs; }
std::size_t BlockPool::used_tokens(BlockId id) const { return get(id).used; }
bool BlockPool::full(BlockId id) const { return used_tokens(id) == tokens_per_block_; }

std::size_t BlockPool::payload_capacity_bytes() const noexcept {
    return capacity() * tokens_per_block_ * bytes_per_token_;
}

bool BlockPool::check_invariants() const {
    std::vector<bool> seen(capacity(), false);
    for (BlockId id : free_) {
        if (id >= capacity() || seen[id] || blocks_[id].refs != 0) return false;
        seen[id] = true;
    }
    for (BlockId id = 0; id < capacity(); ++id) {
        const auto& b = blocks_[id];
        if ((b.refs == 0) != seen[id] || b.used > tokens_per_block_ ||
            (b.refs == 0 && b.used != 0) ||
            b.data.size() != tokens_per_block_ * bytes_per_token_) return false;
    }
    return true;
}

} // namespace kvcache
