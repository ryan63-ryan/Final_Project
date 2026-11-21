#pragma once

#include "kvcache/block_table.h"
#include "kvcache/config.h"
#include "kvcache/eviction_policy.h"
#include "kvcache/radix_tree.h"
#include "kvcache/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace kvcache {

// Fake KV payloads. Real KV entries depend on every token before them, so the
// bytes stored for token i are derived from the whole prefix token_1..token_i:
//
//   h_0 = initial_prefix_hash(seed)
//   h_i = next_prefix_hash(h_{i-1}, token_i)
//   payload_i = make_payload(h_i, bytes_per_token)
//
// Equal prefixes give equal bytes, so sharing a block is safe. Different
// prefixes give different bytes, so a wrong prefix match ([5,7] reused for
// [9,7]) changes what a request reads and the tests can catch it.
std::uint64_t initial_prefix_hash(std::uint64_t seed);
// For a fixed previous hash, different tokens always give different results.
std::uint64_t next_prefix_hash(std::uint64_t previous, Token token);
// The eight little-endian bytes of the hash, repeated (or cut) to the size.
Payload make_payload(std::uint64_t prefix_hash, std::size_t bytes_per_token);

struct Stats {
    std::size_t admitted = 0;        // ADMITs that succeeded
    std::size_t rejected = 0;        // operations refused for lack of capacity
    std::size_t errors = 0;          // operations with an invalid request ID
    std::size_t prompt_tokens = 0;   // prompt tokens of admitted requests
    std::size_t cached_tokens = 0;   // of those, served from the prefix cache
    std::size_t uncached_tokens = 0; // of those, written fresh (recomputation proxy)
    std::size_t cow_copies = 0;
    std::size_t evictions = 0;
    std::size_t used_blocks = 0;     // allocated blocks right now
    std::size_t peak_used_blocks = 0;
    std::size_t fragmentation_slots = 0; // empty token slots inside allocated blocks
};

// Facade over the storage module: maps request IDs to block tables and turns
// trace operations into BlockPool / BlockTable calls.
//
// Every operation is all-or-nothing. Capacity is checked before anything is
// changed, so Reject and Error leave the manager exactly as it was.
//
// Current scope: no prefix cache yet (RadixTree / EvictionPolicy are not merged).
//   sharing=on   FORK shares blocks and APPEND copies on write; ADMIT always
//                allocates fresh blocks (cached_tokens is 0).
//   sharing=off  baseline: FORK copies every token into fresh blocks, so no
//                block is ever shared.
class KVCacheManager {
public:
    explicit KVCacheManager(const Config& config);
    ~KVCacheManager();
    KVCacheManager(const KVCacheManager&) = delete;
    KVCacheManager& operator=(const KVCacheManager&) = delete;
    // A moved-from manager may only be destroyed or assigned to.
    KVCacheManager(KVCacheManager&& other) noexcept;
    KVCacheManager& operator=(KVCacheManager&& other) noexcept;

    OpResult admit(const RequestId& id, const std::vector<Token>& prompt);
    OpResult append(const RequestId& id, Token token);
    OpResult fork(const RequestId& new_id, const RequestId& src_id);
    OpResult release(const RequestId& id);

    bool has_request(const RequestId& id) const;
    // All payload bytes of a request, in token order. Tests compare this with
    // ReferenceCache. Throws std::out_of_range for an unknown request.
    Payload read_request(const RequestId& id) const;

    Stats stats() const; // O(capacity): scans the pool for fragmentation
    // Prefix-cache state, for tests and the demo.
    std::size_t indexed_blocks() const noexcept { return tree_.size(); }
    std::size_t evictable_blocks() const { return policy_->evictable_count(); }
    bool is_indexed(BlockId block) const { return tree_.contains(block); }
    // Pool references of a block (requests + 1 if indexed); 0 if it is free.
    std::size_t block_references(BlockId block) const;
    const Config& config() const noexcept { return config_; }
    bool check_invariants() const;

private:
    struct RequestState {
        std::vector<Token> tokens; // every token so far (prompt + appends)
        std::uint64_t prefix_hash; // h_i of the last token; h_0 if there are none
        BlockTable table;          // owns one pool reference per block
    };

    // Hashes one more token into `hash` and returns the payload to store for it.
    Payload next_payload(std::uint64_t& hash, Token token) const;
    std::size_t blocks_for(std::size_t tokens) const;
    OpResult fail(Status status, const std::string& message);
    void note_usage();

    Config config_;
    // The pool is declared before tree_ and requests_ so the index and the
    // tables release their references first. unique_ptr keeps its address
    // stable when the manager is moved.
    std::unique_ptr<BlockPool> pool_;
    RadixTree tree_;                         // prefix index; stays empty when sharing=off
    std::unique_ptr<EvictionPolicy> policy_; // ranks the blocks in tree_
    std::unordered_map<RequestId, RequestState> requests_;
    Stats stats_;
};

} // namespace kvcache
