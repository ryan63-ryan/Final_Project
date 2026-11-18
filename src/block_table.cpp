#include "kvcache/block_table.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace kvcache {

BlockTable::~BlockTable() { clear(); }

BlockTable::BlockTable(BlockTable&& other) noexcept : pool_(other.pool_) {
    blocks_.swap(other.blocks_);
    token_count_ = std::exchange(other.token_count_, 0);
    copy_count_ = std::exchange(other.copy_count_, 0);
}

BlockTable& BlockTable::operator=(BlockTable&& other) noexcept {
    if (this != &other) {
        clear();
        pool_ = other.pool_;
        blocks_.swap(other.blocks_);
        token_count_ = std::exchange(other.token_count_, 0);
        copy_count_ = std::exchange(other.copy_count_, 0);
    }
    return *this;
}

void BlockTable::clear() noexcept {
    for (BlockId id : blocks_) pool_->release(id);
    blocks_.clear();
    token_count_ = 0;
    copy_count_ = 0;
}

BlockTable BlockTable::fork() const {
    BlockTable copy(*pool_);
    copy.blocks_.reserve(blocks_.size());
    for (BlockId id : blocks_) {
        pool_->retain(id);
        copy.blocks_.push_back(id);
    }
    copy.token_count_ = token_count_;
    return copy;
}

BlockTable BlockTable::from_prefix(BlockPool& pool, const std::vector<BlockId>& ids) {
    if (ids.size() > std::numeric_limits<std::size_t>::max() / pool.tokens_per_block())
        throw std::overflow_error("prefix token count overflow");
    for (BlockId id : ids)
        if (!pool.full(id)) throw std::invalid_argument("cached prefix blocks must be full");
    BlockTable table(pool);
    table.blocks_.reserve(ids.size());
    for (BlockId id : ids) {
        pool.retain(id);
        table.blocks_.push_back(id);
    }
    table.token_count_ = ids.size() * pool.tokens_per_block();
    return table;
}

bool BlockTable::append(const Payload& token) {
    if (token.size() != pool_->bytes_per_token())
        throw std::invalid_argument("wrong token payload size");
    if (token_count_ == std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("token count overflow");
    if (blocks_.empty() || pool_->full(blocks_.back())) {
        if (pool_->free_count() == 0) return false;
        // Reserve before acquiring a pool reference, so an allocation exception cannot leak it.
        if (blocks_.size() == blocks_.capacity()) {
            auto next = blocks_.empty() ? 1 : blocks_.size();
            if (next <= blocks_.max_size() / 2) next *= 2;
            else next = blocks_.max_size();
            if (next <= blocks_.size()) throw std::length_error("block table is too large");
            blocks_.reserve(next);
        }
        auto id = pool_->allocate();
        pool_->append(*id, token);
        blocks_.push_back(*id);
    } else {
        BlockId id = blocks_.back();
        if (pool_->references(id) > 1) {
            auto copy = pool_->clone(id);
            if (!copy) return false;
            pool_->append(*copy, token);
            blocks_.back() = *copy;
            pool_->release(id);
            ++copy_count_;
        } else {
            pool_->append(id, token);
        }
    }
    ++token_count_;
    return true;
}

Payload BlockTable::read_token(std::size_t index) const {
    if (index >= token_count_) throw std::out_of_range("token is not stored");
    auto width = pool_->tokens_per_block();
    return pool_->read_token(blocks_[index / width], index % width);
}

bool BlockTable::check_invariants() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        BlockId id = blocks_[i];
        if (!pool_->allocated(id)) return false;
        auto used = pool_->used_tokens(id);
        if (used == 0 || (i + 1 < blocks_.size() && !pool_->full(id))) return false;
        count += used;
    }
    return count == token_count_;
}

} // namespace kvcache
