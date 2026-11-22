#include "reference_cache.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace reference {

ReferenceCache::ReferenceCache(const kvcache::Config& config) : config_(config) {
    if (!config.capacity || !config.tokens_per_block || !config.bytes_per_token)
        throw std::invalid_argument("reference dimensions must be positive");
}

bool ReferenceCache::prefix_of(const Tokens& prefix, const Tokens& tokens) {
    return prefix.size() <= tokens.size() &&
           std::equal(prefix.begin(), prefix.end(), tokens.begin());
}

bool ReferenceCache::is_indexed(Key key) const {
    for (const auto& entry : cache_)
        if (entry.second.block == key) return true;
    return false;
}

std::size_t ReferenceCache::references(Key key) const {
    std::size_t count = is_indexed(key) ? 1 : 0;
    for (const auto& entry : requests_)
        count += std::count(entry.second.blocks.begin(), entry.second.blocks.end(), key);
    return count;
}

bool ReferenceCache::is_leaf(const Tokens& prefix) const {
    for (const auto& entry : cache_)
        if (entry.first.size() > prefix.size() && prefix_of(prefix, entry.first))
            return false;
    return true;
}

std::size_t ReferenceCache::evictable_blocks() const {
    std::size_t count = 0;
    for (const auto& entry : cache_)
        if (is_leaf(entry.first) && references(entry.second.block) == 1) ++count;
    return count;
}

// Run on a disposable copy. If no leaf is available, the caller discards all
// trial evictions, including ranks and ownership, instead of counting capacity.
bool ReferenceCache::make_room(std::size_t needed, Result& result) {
    while (needed > config_.capacity - blocks_.size()) {
        auto oldest = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (references(it->second.block) != 1 || !is_leaf(it->first)) continue;
            if (oldest == cache_.end() || it->second.rank < oldest->second.rank)
                oldest = it;
        }
        if (oldest == cache_.end()) return false;
        Key victim = oldest->second.block;
        result.evicted.push_back(victim);
        cache_.erase(oldest);
        blocks_.erase(victim);
        ++counters_.evictions;
    }
    return true;
}

Key ReferenceCache::allocate(const Tokens& tokens) {
    Key key = next_key_++;
    blocks_.emplace(key, Block{tokens});
    return key;
}

void ReferenceCache::index_request(const Request& request) {
    if (!config_.sharing) return;
    const auto width = config_.tokens_per_block;
    for (std::size_t end = width; end <= request.tokens.size(); end += width) {
        Tokens prefix(request.tokens.begin(), request.tokens.begin() + end);
        if (cache_.count(prefix) == 0)
            cache_.emplace(std::move(prefix), Entry{request.blocks[end / width - 1], ++clock_});
    }
}

void ReferenceCache::note_usage() {
    counters_.peak_used_blocks = std::max(counters_.peak_used_blocks, blocks_.size());
}

Result ReferenceCache::failure(kvcache::Status status) {
    if (status == kvcache::Status::Reject) ++counters_.rejected;
    else ++counters_.errors;
    Result result;
    result.status = status;
    return result;
}

Result ReferenceCache::admit(const std::string& id, const Tokens& prompt) {
    if (requests_.count(id)) return failure(kvcache::Status::Error);
    ReferenceCache next = *this;
    Result result;

    // Search the entire expected history for the longest matching full prefix.
    std::size_t matched = 0;
    if (config_.sharing)
        for (const auto& entry : cache_)
            if (prefix_of(entry.first, prompt)) matched = std::max(matched, entry.first.size());

    Request request;
    const auto width = config_.tokens_per_block;
    for (std::size_t end = width; end <= matched; end += width) {
        Tokens prefix(prompt.begin(), prompt.begin() + end);
        auto& entry = next.cache_.at(prefix);
        request.blocks.push_back(entry.block);
        if (config_.policy == kvcache::PolicyKind::LRU) entry.rank = ++next.clock_;
    }
    // These physical blocks are held by the candidate request during eviction.
    next.requests_.emplace(id, request);
    auto& added = next.requests_.at(id);
    const auto total = prompt.size() / width + (prompt.size() % width != 0);
    if (!next.make_room(total - request.blocks.size(), result))
        return failure(kvcache::Status::Reject);

    for (std::size_t begin = matched; begin < prompt.size(); begin += width) {
        auto end = std::min(begin + width, prompt.size());
        added.blocks.push_back(next.allocate(Tokens(prompt.begin() + begin, prompt.begin() + end)));
    }
    added.tokens = prompt;
    next.index_request(added);
    result.blocks = added.blocks;
    result.cached_tokens = matched;
    ++next.counters_.admitted;
    next.counters_.prompt_tokens += prompt.size();
    next.counters_.cached_tokens += matched;
    next.counters_.uncached_tokens += prompt.size() - matched;
    next.note_usage();
    *this = std::move(next);
    return result;
}

Result ReferenceCache::append(const std::string& id, kvcache::Token token) {
    if (!requests_.count(id)) return failure(kvcache::Status::Error);
    ReferenceCache next = *this;
    auto& request = next.requests_.at(id);
    const auto width = config_.tokens_per_block;
    bool new_block = request.blocks.empty() ||
                     next.blocks_.at(request.blocks.back()).tokens.size() == width;
    bool copy_tail = !new_block && next.references(request.blocks.back()) > 1;
    Result result;
    if ((new_block || copy_tail) && !next.make_room(1, result))
        return failure(kvcache::Status::Reject);
    if (new_block) request.blocks.push_back(next.allocate({}));
    else if (copy_tail) {
        Tokens old = next.blocks_.at(request.blocks.back()).tokens;
        request.blocks.back() = next.allocate(old);
        result.cow_copies = 1;
        ++next.counters_.cow_copies;
    }
    next.blocks_.at(request.blocks.back()).tokens.push_back(token);
    request.tokens.push_back(token);
    if (request.tokens.size() % width == 0) next.index_request(request);
    result.blocks = request.blocks;
    next.note_usage();
    *this = std::move(next);
    return result;
}

Result ReferenceCache::fork(const std::string& new_id, const std::string& source) {
    if (!requests_.count(source) || requests_.count(new_id))
        return failure(kvcache::Status::Error);
    ReferenceCache next = *this;
    Request request = requests_.at(source);
    Result result;
    if (!config_.sharing) {
        if (!next.make_room(request.blocks.size(), result))
            return failure(kvcache::Status::Reject);
        for (auto& key : request.blocks) key = next.allocate(blocks_.at(key).tokens);
    }
    result.blocks = request.blocks;
    next.requests_.emplace(new_id, std::move(request));
    next.note_usage();
    *this = std::move(next);
    return result;
}

Result ReferenceCache::release(const std::string& id) {
    auto found = requests_.find(id);
    if (found == requests_.end()) return failure(kvcache::Status::Error);
    Result result;
    const auto keys = found->second.blocks;
    requests_.erase(found);
    for (Key key : keys) {
        if (references(key) == 0) {
            result.freed.push_back(key);
            blocks_.erase(key);
        }
    }
    return result;
}

Counters ReferenceCache::stats() const {
    Counters result = counters_;
    result.used_blocks = blocks_.size();
    for (const auto& entry : blocks_)
        result.fragmentation_slots += config_.tokens_per_block - entry.second.tokens.size();
    return result;
}

std::vector<std::uint8_t> ReferenceCache::read_request(const std::string& id) const {
    // Recompute from the independent token vector, never from stored SUT bytes
    // or the manager's payload helpers. This is the published fake-KV format.
    auto mix = [](std::uint64_t value) {
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31);
    };
    auto hash = mix(config_.seed);
    std::vector<std::uint8_t> bytes;
    for (auto token : requests_.at(id).tokens) {
        hash = mix(hash + UINT64_C(0x9e3779b97f4a7c15) + static_cast<std::uint32_t>(token));
        for (std::size_t byte = 0; byte < config_.bytes_per_token; ++byte)
            bytes.push_back(static_cast<std::uint8_t>(hash >> ((byte % 8) * 8)));
    }
    return bytes;
}

} // namespace reference
