#include "kvcache/kv_cache_manager.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace kvcache {

namespace {

// splitmix64 finalizer (Steele, Lea and Flood, 2014). It is a one-to-one
// mapping of 64-bit values, so distinct inputs never collide in this step.
std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

std::unique_ptr<EvictionPolicy> make_policy(PolicyKind kind) {
    if (kind == PolicyKind::FIFO) return std::make_unique<FIFOPolicy>();
    return std::make_unique<LRUPolicy>();
}

} // namespace

std::uint64_t initial_prefix_hash(std::uint64_t seed) { return mix(seed); }

std::uint64_t next_prefix_hash(std::uint64_t previous, Token token) {
    // Unsigned arithmetic wraps, which is intended. The odd constant is the
    // usual splitmix64 increment; it keeps token 0 from leaving the sum unchanged.
    return mix(previous + 0x9e3779b97f4a7c15ULL + static_cast<std::uint32_t>(token));
}

Payload make_payload(std::uint64_t prefix_hash, std::size_t bytes_per_token) {
    Payload payload(bytes_per_token);
    for (std::size_t i = 0; i < bytes_per_token; ++i)
        payload[i] = static_cast<std::uint8_t>(prefix_hash >> (8 * (i % 8)));
    return payload;
}

KVCacheManager::KVCacheManager(const Config& config)
    : config_(config),
      pool_(std::make_unique<BlockPool>(config.capacity, config.tokens_per_block,
                                        config.bytes_per_token)),
      tree_(*pool_),
      policy_(make_policy(config.policy)) {}

KVCacheManager::~KVCacheManager() = default;
KVCacheManager::KVCacheManager(KVCacheManager&& other) noexcept = default;

KVCacheManager& KVCacheManager::operator=(KVCacheManager&& other) noexcept {
    if (this != &other) {
        // Our tables and our index must release before our pool is destroyed.
        requests_.clear();
        tree_ = std::move(other.tree_); // releases our index's references first
        policy_ = std::move(other.policy_);
        config_ = other.config_;
        pool_ = std::move(other.pool_);
        requests_ = std::move(other.requests_);
        stats_ = other.stats_;
    }
    return *this;
}

Payload KVCacheManager::next_payload(std::uint64_t& hash, Token token) const {
    hash = next_prefix_hash(hash, token);
    return make_payload(hash, pool_->bytes_per_token());
}

std::size_t KVCacheManager::blocks_for(std::size_t tokens) const {
    auto width = pool_->tokens_per_block();
    return tokens / width + (tokens % width != 0 ? 1 : 0);
}

void KVCacheManager::index_full_blocks(const RequestState& state) {
    if (!config_.sharing) return;
    const auto width = pool_->tokens_per_block();
    const auto full = state.tokens.size() / width;
    // Levels below `covered` are already indexed, by this request's blocks or by
    // equal blocks of another request (then ours stay private). Start at the
    // first uncovered level rather than only the newest block: an ancestor that
    // was indexed by someone else may have been evicted since, and
    // RadixTree::insert needs the parent prefix, so the chain is healed with
    // this request's own blocks from that level down.
    const auto covered = tree_.match(state.tokens).size();
    for (std::size_t level = covered; level < full; ++level) {
        BlockId block = state.table.blocks()[level];
        std::vector<Token> prefix(state.tokens.begin(),
                                  state.tokens.begin() + (level + 1) * width);
        if (!tree_.insert(prefix, block))
            throw std::logic_error("uncovered prefix was already indexed");
        policy_->on_insert(block); // starts not evictable: this request holds it
        // A cached leaf that just gained a child is no longer evictable.
        if (auto parent = tree_.parent(block)) policy_->set_evictable(*parent, false);
    }
}

OpResult KVCacheManager::fail(Status status, const std::string& message) {
    if (status == Status::Reject) ++stats_.rejected;
    else ++stats_.errors;
    OpResult result;
    result.status = status;
    result.message = message;
    return result;
}

void KVCacheManager::note_usage() {
    auto used = pool_->capacity() - pool_->free_count();
    stats_.peak_used_blocks = std::max(stats_.peak_used_blocks, used);
}

// ADMIT: create a request and store its prompt.
OpResult KVCacheManager::admit(const RequestId& id, const std::vector<Token>& prompt) {
    if (has_request(id)) return fail(Status::Error, "duplicate request ID '" + id + "'");

    // TODO(integration): with the prefix cache, look up the longest cached run of
    // full blocks here, start the table with BlockTable::from_prefix, and count
    // evictable blocks as available capacity.
    auto needed = blocks_for(prompt.size());
    if (needed > pool_->free_count())
        return fail(Status::Reject, "needs " + std::to_string(needed) + " blocks, " +
                                        std::to_string(pool_->free_count()) + " free");

    BlockTable table(*pool_);
    auto hash = initial_prefix_hash(config_.seed);
    for (Token token : prompt)
        if (!table.append(next_payload(hash, token)))
            throw std::logic_error("admit ran out of blocks after its capacity check");

    OpResult result;
    result.blocks = table.blocks();
    auto added = requests_.emplace(id, RequestState{prompt, hash, std::move(table)}).first;
    index_full_blocks(added->second);
    ++stats_.admitted;
    stats_.prompt_tokens += prompt.size();
    stats_.uncached_tokens += prompt.size();
    note_usage();
    return result;
}

// APPEND: add one generated token to a request.
OpResult KVCacheManager::append(const RequestId& id, Token token) {
    auto it = requests_.find(id);
    if (it == requests_.end()) return fail(Status::Error, "unknown request '" + id + "'");
    auto& state = it->second;

    // BlockTable::append takes a new block in three cases: the table is empty,
    // the tail is full, or the tail is partial but shared (copy-on-write).
    const auto& blocks = state.table.blocks();
    bool needs_block = blocks.empty() || pool_->full(blocks.back()) ||
                       pool_->references(blocks.back()) > 1;
    // TODO(integration): evict one cached block here instead of rejecting.
    if (needs_block && pool_->free_count() == 0)
        return fail(Status::Reject, "needs 1 block, 0 free");

    auto copies_before = state.table.copy_count();
    auto hash = state.prefix_hash;
    if (!state.table.append(next_payload(hash, token)))
        throw std::logic_error("append ran out of blocks after its capacity check");
    state.prefix_hash = hash;
    state.tokens.push_back(token);
    if (state.tokens.size() % pool_->tokens_per_block() == 0) index_full_blocks(state);

    OpResult result;
    result.blocks = state.table.blocks();
    result.cow_copies = state.table.copy_count() - copies_before;
    stats_.cow_copies += result.cow_copies;
    note_usage();
    return result;
}

// FORK: create new_id with the same tokens as src_id.
OpResult KVCacheManager::fork(const RequestId& new_id, const RequestId& src_id) {
    auto it = requests_.find(src_id);
    if (it == requests_.end()) return fail(Status::Error, "unknown request '" + src_id + "'");
    if (has_request(new_id)) return fail(Status::Error, "duplicate request ID '" + new_id + "'");
    const auto& source = it->second;

    BlockTable table(*pool_);
    auto hash = source.prefix_hash; // same tokens, so the same running hash
    if (config_.sharing) {
        // Share every block, including a partial tail. Needs no new blocks.
        table = source.table.fork();
    } else {
        // Baseline: an independent copy, one fresh block per source block.
        auto needed = source.table.blocks().size();
        if (needed > pool_->free_count())
            return fail(Status::Reject, "needs " + std::to_string(needed) + " blocks, " +
                                            std::to_string(pool_->free_count()) + " free");
        hash = initial_prefix_hash(config_.seed);
        for (Token token : source.tokens)
            if (!table.append(next_payload(hash, token)))
                throw std::logic_error("fork ran out of blocks after its capacity check");
    }

    OpResult result;
    result.blocks = table.blocks();
    requests_.emplace(new_id, RequestState{source.tokens, hash, std::move(table)});
    note_usage();
    return result;
}

// RELEASE: drop a request and its references.
OpResult KVCacheManager::release(const RequestId& id) {
    auto it = requests_.find(id);
    if (it == requests_.end()) return fail(Status::Error, "unknown request '" + id + "'");

    // A block whose only reference is this request goes back to the free list.
    // An indexed block also has the index's reference, so it stays cached.
    OpResult result;
    const std::vector<BlockId> blocks = it->second.table.blocks();
    for (BlockId block : blocks)
        if (pool_->references(block) == 1) result.freed.push_back(block);
    requests_.erase(it); // ~BlockTable releases every reference
    // A cached leaf that no request holds any more becomes evictable.
    for (BlockId block : blocks)
        if (tree_.contains(block)) policy_->set_evictable(block, tree_.is_evictable(block));
    return result;
}

bool KVCacheManager::has_request(const RequestId& id) const {
    return requests_.count(id) != 0;
}

Payload KVCacheManager::read_request(const RequestId& id) const {
    auto it = requests_.find(id);
    if (it == requests_.end()) throw std::out_of_range("unknown request '" + id + "'");
    const auto& table = it->second.table;
    Payload bytes;
    bytes.reserve(table.token_count() * pool_->bytes_per_token());
    for (std::size_t i = 0; i < table.token_count(); ++i) {
        Payload token = table.read_token(i);
        bytes.insert(bytes.end(), token.begin(), token.end());
    }
    return bytes;
}

std::size_t KVCacheManager::block_references(BlockId block) const {
    return pool_->allocated(block) ? pool_->references(block) : 0;
}

Stats KVCacheManager::stats() const {
    Stats stats = stats_;
    stats.used_blocks = pool_->capacity() - pool_->free_count();
    for (BlockId id = 0; id < pool_->capacity(); ++id)
        if (pool_->allocated(id))
            stats.fragmentation_slots += pool_->tokens_per_block() - pool_->used_tokens(id);
    return stats;
}

bool KVCacheManager::check_invariants() const {
    if (!pool_->check_invariants()) return false;
    // holders[b] = number of request tables that contain block b.
    std::vector<std::size_t> holders(pool_->capacity(), 0);
    for (const auto& entry : requests_) {
        const auto& state = entry.second;
        if (!state.table.check_invariants()) return false;
        if (state.table.token_count() != state.tokens.size()) return false;
        // Recompute every payload from the tokens alone and compare stored bytes.
        auto hash = initial_prefix_hash(config_.seed);
        for (std::size_t i = 0; i < state.tokens.size(); ++i)
            if (state.table.read_token(i) != next_payload(hash, state.tokens[i])) return false;
        if (hash != state.prefix_hash) return false;
        for (BlockId block : state.table.blocks()) ++holders[block];
    }
    // Every reference must belong to a request or to the index (one per indexed
    // block): no leaked and no missing counts.
    if (!tree_.check_invariants() || !policy_->check_invariants()) return false;
    std::size_t evictable = 0;
    for (BlockId id = 0; id < pool_->capacity(); ++id) {
        if (block_references(id) != holders[id] + (tree_.contains(id) ? 1 : 0)) return false;
        if (tree_.is_evictable(id)) ++evictable;
    }
    // The policy's evictable set must be exactly the tree's evictable leaves. The
    // policy has no per-block query, so compare the sizes and the next victim.
    if (policy_->evictable_count() != evictable) return false;
    auto victim = policy_->victim();
    if (victim && !tree_.is_evictable(*victim)) return false;
    if (!config_.sharing) {
        if (tree_.size() != 0) return false; // the baseline never uses the index
        for (std::size_t count : holders)
            if (count > 1) return false;
    }
    return true;
}

} // namespace kvcache
