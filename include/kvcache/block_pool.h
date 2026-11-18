#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace kvcache {

using BlockId = std::size_t;
using Payload = std::vector<std::uint8_t>;

// One reference per request-table entry or cache-index owner.
class BlockPool {
public:
    BlockPool(std::size_t capacity, std::size_t tokens_per_block,
              std::size_t bytes_per_token);
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;
    BlockPool(BlockPool&&) = delete;
    BlockPool& operator=(BlockPool&&) = delete;

    std::optional<BlockId> allocate();
    std::optional<BlockId> clone(BlockId source);
    void retain(BlockId id);
    void release(BlockId id);
    void append(BlockId id, const Payload& token);
    Payload read_token(BlockId id, std::size_t index) const;

    bool allocated(BlockId id) const noexcept;
    std::size_t references(BlockId id) const;
    std::size_t used_tokens(BlockId id) const;
    bool full(BlockId id) const;
    std::size_t capacity() const noexcept { return blocks_.size(); }
    std::size_t free_count() const noexcept { return free_.size(); }
    std::size_t tokens_per_block() const noexcept { return tokens_per_block_; }
    std::size_t bytes_per_token() const noexcept { return bytes_per_token_; }
    std::size_t payload_capacity_bytes() const noexcept;
    bool check_invariants() const;

private:
    struct Block {
        explicit Block(std::size_t bytes) : data(bytes, 0) {}
        Payload data;
        std::size_t used = 0;
        std::size_t refs = 0;
    };
    const Block& get(BlockId id) const;
    Block& get(BlockId id);

    std::size_t tokens_per_block_;
    std::size_t bytes_per_token_;
    std::vector<Block> blocks_;
    std::vector<BlockId> free_;
};

} // namespace kvcache
