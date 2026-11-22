#pragma once

#include "kvcache/config.h"
#include "kvcache/types.h"

#include <map>

namespace reference {

using Key = std::size_t;
using Tokens = std::vector<kvcache::Token>;

struct Result {
    kvcache::Status status = kvcache::Status::Ok;
    std::vector<Key> blocks, freed, evicted;
    std::size_t cached_tokens = 0;
    std::size_t cow_copies = 0;
};

struct Request {
    Tokens tokens;
    std::vector<Key> blocks;
};

struct Block {
    Tokens tokens;
};

struct Counters {
    std::size_t admitted = 0, rejected = 0, errors = 0;
    std::size_t prompt_tokens = 0, cached_tokens = 0, uncached_tokens = 0;
    std::size_t cow_copies = 0, evictions = 0;
    std::size_t used_blocks = 0, peak_used_blocks = 0, fragmentation_slots = 0;
};

// Small, deliberately slow oracle: flat prefix records and exhaustive scans.
// Keys identify logical allocations, not the production pool's recycled IDs.
class ReferenceCache {
public:
    explicit ReferenceCache(const kvcache::Config& config);

    Result admit(const std::string& id, const Tokens& prompt);
    Result append(const std::string& id, kvcache::Token token);
    Result fork(const std::string& new_id, const std::string& source);
    Result release(const std::string& id);

    const std::map<std::string, Request>& requests() const { return requests_; }
    const std::map<Key, Block>& blocks() const { return blocks_; }
    std::size_t references(Key key) const;
    bool is_indexed(Key key) const;
    std::size_t indexed_blocks() const { return cache_.size(); }
    std::size_t evictable_blocks() const;
    Counters stats() const;
    std::vector<std::uint8_t> read_request(const std::string& id) const;

private:
    struct Entry {
        Key block;
        std::uint64_t rank;
    };

    static bool prefix_of(const Tokens& prefix, const Tokens& tokens);
    bool is_leaf(const Tokens& prefix) const;
    bool make_room(std::size_t needed, Result& result);
    Key allocate(const Tokens& tokens);
    void index_request(const Request& request);
    void note_usage();
    Result failure(kvcache::Status status);

    kvcache::Config config_;
    std::map<std::string, Request> requests_;
    std::map<Key, Block> blocks_;
    std::map<Tokens, Entry> cache_;
    Key next_key_ = 0;
    std::uint64_t clock_ = 0;
    Counters counters_;
};

} // namespace reference
