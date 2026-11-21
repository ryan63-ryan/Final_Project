#include "kvcache/eviction_policy.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace kvcache {

std::uint64_t RankedPolicy::next_rank() {
    if (clock_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("eviction clock overflow");
    return clock_ + 1; // committed by the caller once nothing else can throw
}

void RankedPolicy::require_tracked(BlockId block) const {
    if (rank_.count(block) == 0) throw std::invalid_argument("block is not tracked");
}

void RankedPolicy::on_insert(BlockId block) {
    if (rank_.count(block) != 0) throw std::invalid_argument("block is already tracked");
    auto rank = next_rank();
    rank_.emplace(block, rank);
    clock_ = rank;
}

void RankedPolicy::refresh(BlockId block) {
    auto it = rank_.find(block);
    if (it == rank_.end()) throw std::invalid_argument("block is not tracked");
    auto rank = next_rank();
    evictable_.update(block, rank); // no-op (false) when not evictable; cannot throw
    it->second = rank;
    clock_ = rank;
}

void RankedPolicy::on_erase(BlockId block) {
    require_tracked(block);
    evictable_.erase(block);
    rank_.erase(block);
}

void RankedPolicy::set_evictable(BlockId block, bool evictable) {
    auto it = rank_.find(block);
    if (it == rank_.end()) throw std::invalid_argument("block is not tracked");
    if (!evictable) evictable_.erase(block);
    else if (!evictable_.contains(block)) evictable_.push(block, it->second);
}

std::optional<BlockId> RankedPolicy::victim() const {
    if (evictable_.empty()) return std::nullopt;
    return evictable_.top().key;
}

bool RankedPolicy::check_invariants() const {
    if (!evictable_.check_invariants() || evictable_.size() > rank_.size()) return false;
    std::unordered_set<std::uint64_t> ranks;
    std::size_t in_heap = 0;
    for (const auto& entry : rank_) {
        if (entry.second == 0 || entry.second > clock_ || !ranks.insert(entry.second).second)
            return false;
        if (evictable_.contains(entry.first)) ++in_heap;
    }
    if (in_heap != evictable_.size()) return false; // heap keys are all tracked
    if (!evictable_.empty() && evictable_.top().value != rank_.at(evictable_.top().key))
        return false;
    return true;
}

std::optional<BlockId> evict_one(RadixTree& tree, EvictionPolicy& policy) {
    auto victim = policy.victim();
    if (!victim) return std::nullopt;
    BlockId block = *victim;
    if (!tree.is_evictable(block))
        throw std::logic_error("policy victim is not an unreferenced radix leaf");
    auto parent = tree.parent(block);
    tree.erase(block); // succeeds: the block is an indexed leaf
    policy.on_erase(block);
    // Erasing the last child may make the parent an evictable leaf. If this
    // throws, the parent is left marked not evictable: safe, merely not reusable.
    if (parent) policy.set_evictable(*parent, tree.is_evictable(*parent));
    return block;
}

} // namespace kvcache
