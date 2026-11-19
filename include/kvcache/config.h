#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kvcache {

enum class PolicyKind { LRU, FIFO };

// Run configuration. Field names match the BlockPool constructor arguments.
struct Config {
    std::size_t capacity = 1024;       // physical blocks in the pool
    std::size_t tokens_per_block = 16; // B
    std::size_t bytes_per_token = 64;  // P: fake KV payload bytes per token
    PolicyKind policy = PolicyKind::LRU;
    bool sharing = true;               // false = independent-allocation baseline
    std::uint64_t seed = 42;
};

// Reads a key=value file. Blank lines are skipped and '#' starts a comment.
// Keys not present keep the defaults above.
//
//   capacity=1024
//   tokens_per_block=16
//   bytes_per_token=64
//   policy=lru          # lru | fifo
//   sharing=on          # on | off
//   seed=42
//
// Throws std::runtime_error (with the line number) if the file cannot be opened,
// a key is unknown or repeated, or a value is invalid. The three sizes must be
// positive, so a loaded Config never makes the BlockPool constructor reject them.
Config load_config(const std::string& path);

} // namespace kvcache
