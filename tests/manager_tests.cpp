#include "kvcache/kv_cache_manager.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace kvcache;

void check(bool ok, const char* condition, int line) {
    if (!ok) throw std::runtime_error("line " + std::to_string(line) + ": " + condition);
}
#define CHECK(condition) check((condition), #condition, __LINE__)

Config small_config(bool sharing) {
    Config config;
    config.capacity = 4;
    config.tokens_per_block = 2;
    config.bytes_per_token = 64;
    config.sharing = sharing;
    return config;
}

// Bytes of token `index` inside the result of read_request.
Payload token_bytes(const Payload& bytes, std::size_t index, std::size_t width) {
    return Payload(bytes.begin() + index * width, bytes.begin() + (index + 1) * width);
}

void payload_tests() {
    auto start = initial_prefix_hash(42);
    CHECK(initial_prefix_hash(42) == start && initial_prefix_hash(43) != start);
    CHECK(next_prefix_hash(start, 0) != start);
    CHECK(next_prefix_hash(start, 5) != next_prefix_hash(start, 9));
    // Same last token, different first token: the hashes must still differ.
    auto after_5_7 = next_prefix_hash(next_prefix_hash(start, 5), 7);
    auto after_9_7 = next_prefix_hash(next_prefix_hash(start, 9), 7);
    CHECK(after_5_7 != after_9_7);
    CHECK(make_payload(after_5_7, 64) != make_payload(after_9_7, 64));

    Payload bytes = make_payload(0x0807060504030201ULL, 11);
    CHECK(bytes == Payload({1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3}));
    CHECK(make_payload(start, 3).size() == 3);

    // The same check through the manager: token 7 sits at position 1 of both
    // prompts, yet the stored bytes differ because the prefixes differ.
    KVCacheManager manager(small_config(true));
    CHECK(manager.admit("a", {5, 7}).status == Status::Ok);
    CHECK(manager.admit("b", {9, 7}).status == Status::Ok);
    Payload a = manager.read_request("a"), b = manager.read_request("b");
    CHECK(a.size() == 2 * 64 && b.size() == 2 * 64);
    CHECK(token_bytes(a, 0, 64) != token_bytes(b, 0, 64));
    CHECK(token_bytes(a, 1, 64) != token_bytes(b, 1, 64));
    CHECK(token_bytes(a, 1, 64) == make_payload(after_5_7, 64));
    CHECK(token_bytes(b, 1, 64) == make_payload(after_9_7, 64));
    // Equal prefixes still give equal bytes, which is what makes sharing safe.
    CHECK(manager.release("b").status == Status::Ok);
    CHECK(manager.admit("c", {5, 7}).status == Status::Ok);
    CHECK(manager.read_request("c") == a);
    // A different seed changes every payload.
    Config other = small_config(true);
    other.seed = 7;
    KVCacheManager reseeded(other);
    CHECK(reseeded.admit("a", {5, 7}).status == Status::Ok);
    CHECK(reseeded.read_request("a") != a);
    CHECK(manager.check_invariants() && reseeded.check_invariants());
}

// The proposal example (tests/golden/example.trace), checked step by step.
void cow_tests() {
    KVCacheManager manager(small_config(true));
    OpResult result = manager.admit("r1", {10, 11, 12});
    CHECK(result.status == Status::Ok && result.blocks == std::vector<BlockId>({0, 1}));
    CHECK(result.cached_tokens == 0);
    Payload r1_before = manager.read_request("r1");

    result = manager.fork("r2", "r1");
    CHECK(result.status == Status::Ok && result.blocks == std::vector<BlockId>({0, 1}));
    CHECK(manager.read_request("r2") == r1_before);

    result = manager.append("r2", 13);
    CHECK(result.status == Status::Ok && result.blocks == std::vector<BlockId>({0, 2}));
    CHECK(result.cow_copies == 1);
    // Copy-on-write: r1 reads exactly the bytes it read before r2 appended.
    CHECK(manager.read_request("r1") == r1_before);
    Payload r2 = manager.read_request("r2");
    CHECK(r2.size() == 4 * 64);
    CHECK(Payload(r2.begin(), r2.begin() + 3 * 64) == r1_before);
    CHECK(manager.check_invariants());

    result = manager.release("r1");
    CHECK(result.status == Status::Ok && result.freed == std::vector<BlockId>({1}));
    CHECK(!manager.has_request("r1") && manager.read_request("r2") == r2);

    // The fork carried the running hash: r2 equals a request admitted in one go.
    CHECK(manager.admit("direct", {10, 11, 12, 13}).status == Status::Ok);
    CHECK(manager.read_request("direct") == r2);
    Stats stats = manager.stats();
    CHECK(stats.cow_copies == 1 && stats.admitted == 2 && stats.prompt_tokens == 7);
    CHECK(stats.used_blocks == 4 && stats.peak_used_blocks == 4 && stats.fragmentation_slots == 0);
    CHECK(manager.check_invariants());
}

// sharing=off: the same trace never shares a block and never copies on write.
void baseline_tests() {
    KVCacheManager manager(small_config(false));
    CHECK(manager.admit("r1", {10, 11, 12}).blocks == std::vector<BlockId>({0, 1}));
    Payload r1_before = manager.read_request("r1");
    OpResult result = manager.fork("r2", "r1");
    CHECK(result.status == Status::Ok && result.blocks == std::vector<BlockId>({2, 3}));
    CHECK(manager.read_request("r2") == r1_before);
    result = manager.append("r2", 13);
    CHECK(result.status == Status::Ok && result.cow_copies == 0);
    CHECK(result.blocks == std::vector<BlockId>({2, 3}));
    CHECK(manager.read_request("r1") == r1_before);
    CHECK(manager.release("r1").freed == std::vector<BlockId>({0, 1}));
    // With no free block left, a baseline fork is rejected without any change.
    CHECK(manager.admit("r3", {1, 2, 3}).status == Status::Ok);
    CHECK(manager.fork("r4", "r3").status == Status::Reject && !manager.has_request("r4"));
    CHECK(manager.stats().peak_used_blocks == 4 && manager.check_invariants());
}

// Error and Reject must leave the manager exactly as it was.
void failure_tests() {
    KVCacheManager manager(small_config(true));
    CHECK(manager.admit("r1", {1, 2, 3}).status == Status::Ok);
    Payload r1 = manager.read_request("r1");

    CHECK(manager.admit("r1", {4}).status == Status::Error);
    CHECK(manager.append("nobody", 4).status == Status::Error);
    CHECK(manager.fork("r2", "nobody").status == Status::Error);
    CHECK(manager.fork("r1", "r1").status == Status::Error);
    CHECK(manager.release("nobody").status == Status::Error);
    CHECK(manager.admit("big", {1, 2, 3, 4, 5}).status == Status::Reject);
    CHECK(!manager.has_request("big") && !manager.has_request("r2"));
    CHECK(manager.read_request("r1") == r1 && manager.stats().used_blocks == 2);

    OpResult empty = manager.admit("empty", {});
    CHECK(empty.status == Status::Ok && empty.blocks.empty());
    CHECK(manager.read_request("empty").empty());
    CHECK(manager.admit("fill", {6, 7, 8, 9}).status == Status::Ok);
    CHECK(manager.append("empty", 5).status == Status::Reject); // needs a first block
    CHECK(manager.append("fill", 5).status == Status::Reject);  // full tail
    CHECK(manager.append("r1", 4).status == Status::Ok);        // room in its own tail
    CHECK(manager.read_request("empty").empty());

    Stats stats = manager.stats();
    CHECK(stats.errors == 5 && stats.rejected == 3 && stats.admitted == 3);
    CHECK(stats.used_blocks == 4 && stats.fragmentation_slots == 0);
    bool threw = false;
    try { manager.read_request("nobody"); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw && manager.check_invariants());

    // Moving a manager keeps every request readable.
    Payload fill = manager.read_request("fill");
    KVCacheManager moved(std::move(manager));
    CHECK(moved.read_request("fill") == fill && moved.check_invariants());
    KVCacheManager target(small_config(true));
    CHECK(target.admit("old", {1, 2, 3}).status == Status::Ok);
    target = std::move(moved);
    CHECK(!target.has_request("old") && target.read_request("fill") == fill);
    CHECK(target.check_invariants());
    // Blocks 0-1 hold r1, 2-3 hold fill; nothing is free or over-counted.
    CHECK(target.block_references(0) == 1 && target.block_references(3) == 1);
    CHECK(target.release("fill").status == Status::Ok && target.block_references(3) == 0);
}

// Both policies and both sharing modes construct, and an empty prefix cache
// satisfies the manager's index and policy invariants.
void wiring_tests() {
    for (PolicyKind kind : {PolicyKind::LRU, PolicyKind::FIFO})
        for (bool sharing : {true, false}) {
            Config config = small_config(sharing);
            config.policy = kind;
            KVCacheManager manager(config);
            CHECK(manager.indexed_blocks() == 0 && manager.evictable_blocks() == 0);
            CHECK(manager.admit("r", {1}).status == Status::Ok);
            CHECK(!manager.is_indexed(0) && manager.block_references(0) == 1);
            CHECK(manager.block_references(1) == 0 && manager.check_invariants());
        }
}

int main() {
    try {
        payload_tests(); std::cout << "PASS prefix-dependent payloads\n";
        cow_tests(); std::cout << "PASS proposal example, fork and copy-on-write\n";
        baseline_tests(); std::cout << "PASS sharing=off baseline\n";
        failure_tests(); std::cout << "PASS errors, rejects and moves leave state intact\n";
        wiring_tests(); std::cout << "PASS index and policy wiring\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
