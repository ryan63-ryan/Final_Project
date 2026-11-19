#pragma once

#include "kvcache/block_pool.h" // BlockId, Payload

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kvcache {

using Token = std::int32_t;    // one token ID from the trace
using RequestId = std::string; // request name from the trace, e.g. "r1"

// Outcome of one manager operation. Expected failures are reported here;
// exceptions are kept for programming errors, as in the storage module.
enum class Status {
    Ok,
    Reject, // valid request, but not enough free + evictable blocks; no state change
    Error   // invalid request: unknown or duplicate ID, etc.; no state change
};

// What one operation reports. The driver prints it as one output line.
struct OpResult {
    Status status = Status::Ok;
    std::vector<BlockId> blocks;   // request's block table after ADMIT/APPEND/FORK
    std::size_t cached_tokens = 0; // ADMIT: prompt tokens served from the prefix cache
    std::size_t cow_copies = 0;    // APPEND: 1 if a shared partial tail was copied
    std::vector<BlockId> freed;    // RELEASE: blocks returned to the free list
    std::vector<BlockId> evicted;  // any op: cached blocks evicted to make room
    std::string message;           // reason for Reject / Error
};

} // namespace kvcache
