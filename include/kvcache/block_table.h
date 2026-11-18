#pragma once

#include "kvcache/block_pool.h"

namespace kvcache {

// The pool must outlive every table. Each entry owns one pool reference.
class BlockTable {
public:
    explicit BlockTable(BlockPool& pool) : pool_(&pool) {}
    ~BlockTable();
    BlockTable(const BlockTable&) = delete;
    BlockTable& operator=(const BlockTable&) = delete;
    BlockTable(BlockTable&& other) noexcept;
    BlockTable& operator=(BlockTable&& other) noexcept;

    BlockTable fork() const;
    static BlockTable from_prefix(BlockPool& pool, const std::vector<BlockId>& ids);
    bool append(const Payload& token); // false = no free physical block; no change
    Payload read_token(std::size_t index) const;
    void clear() noexcept;

    const std::vector<BlockId>& blocks() const noexcept { return blocks_; }
    std::size_t token_count() const noexcept { return token_count_; }
    std::size_t copy_count() const noexcept { return copy_count_; }
    bool check_invariants() const;

private:
    BlockPool* pool_;
    std::vector<BlockId> blocks_;
    std::size_t token_count_ = 0;
    std::size_t copy_count_ = 0;
};

} // namespace kvcache
