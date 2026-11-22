#include "kvcache/kv_cache_manager.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

using namespace kvcache;

void check(bool ok, const char* condition, int line) {
    if (!ok) throw std::runtime_error("line " + std::to_string(line) + ": " + condition);
}
#define CHECK(condition) check((condition), #condition, __LINE__)

// Every test below builds its configuration here, and main() runs them all
// once per mode, so each test covers both ways of counting reclaimable blocks.
ReclaimableKind reclaimable_mode = ReclaimableKind::Count;

Config small_config(bool sharing) {
    Config config;
    config.reclaimable = reclaimable_mode;
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

// What a request holding `tokens` must read, computed from the tokens alone.
Payload expected_bytes(const std::vector<Token>& tokens, std::uint64_t seed = 42,
                       std::size_t width = 64) {
    Payload bytes;
    auto hash = initial_prefix_hash(seed);
    for (Token token : tokens) {
        hash = next_prefix_hash(hash, token);
        Payload payload = make_payload(hash, width);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    return bytes;
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

    // The fork carried the running hash: r2 equals a request admitted in one go,
    // which in fact finds both of r2's full blocks in the prefix cache.
    result = manager.admit("direct", {10, 11, 12, 13});
    CHECK(result.status == Status::Ok && result.blocks == std::vector<BlockId>({0, 2}));
    CHECK(result.cached_tokens == 4 && manager.read_request("direct") == r2);
    CHECK(r2 == expected_bytes({10, 11, 12, 13}));
    Stats stats = manager.stats();
    CHECK(stats.cow_copies == 1 && stats.admitted == 2 && stats.prompt_tokens == 7);
    CHECK(stats.cached_tokens == 4 && stats.uncached_tokens == 3);
    CHECK(stats.used_blocks == 2 && stats.peak_used_blocks == 3 && stats.fragmentation_slots == 0);
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

// sharing=off bypasses the prefix index completely: equal prompts are stored
// twice, nothing is cached after RELEASE and nothing can be evicted.
void baseline_bypass_tests() {
    using Blocks = std::vector<BlockId>;
    for (PolicyKind kind : {PolicyKind::LRU, PolicyKind::FIFO}) {
        Config config = small_config(false);
        config.policy = kind; // irrelevant when sharing=off
        KVCacheManager manager(config);
        OpResult result = manager.admit("a", {1, 2, 3, 4});
        CHECK(result.blocks == Blocks({0, 1}) && result.cached_tokens == 0);
        result = manager.admit("b", {1, 2, 3, 4});
        CHECK(result.blocks == Blocks({2, 3}) && result.cached_tokens == 0);
        CHECK(manager.indexed_blocks() == 0 && manager.evictable_blocks() == 0);
        for (BlockId block = 0; block < 4; ++block) CHECK(manager.block_references(block) == 1);
        // With sharing=on this prompt would be a full cache hit needing no block.
        result = manager.admit("c", {1, 2});
        CHECK(result.status == Status::Reject);
        CHECK(result.message == "needs 1 block, 0 free, 0 evictable");
        CHECK(manager.append("a", 5).status == Status::Reject);

        // RELEASE frees full blocks at once; there is no cache to hit afterwards.
        CHECK(manager.release("a").freed == Blocks({0, 1}));
        CHECK(manager.indexed_blocks() == 0 && manager.stats().used_blocks == 2);
        result = manager.admit("c", {1, 2});
        CHECK(result.status == Status::Ok && result.cached_tokens == 0);
        CHECK(result.evicted.empty() && manager.block_references(result.blocks.at(0)) == 1);
        // Filling a block by APPEND does not index it either.
        CHECK(manager.append("c", 3).status == Status::Ok);
        CHECK(manager.append("c", 4).status == Status::Ok && manager.indexed_blocks() == 0);
        // The stored bytes are the same as with sharing.
        CHECK(manager.read_request("b") == expected_bytes({1, 2, 3, 4}));
        CHECK(manager.read_request("c") == manager.read_request("b"));
        Stats stats = manager.stats();
        CHECK(stats.cached_tokens == 0 && stats.uncached_tokens == 10 && stats.evictions == 0);
        CHECK(stats.cow_copies == 0 && stats.rejected == 2 && manager.check_invariants());
    }
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
    // No cached prefix (first token differs from r1's): 3 blocks needed, 2 free.
    CHECK(manager.admit("big", {9, 2, 3, 4, 5}).status == Status::Reject);
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
    // The index moved too: blocks 0-1 (r1) and 2-3 (fill) are full, so each has
    // its request's reference plus the index's. Releasing fill leaves them cached.
    CHECK(target.indexed_blocks() == 4 && target.block_references(0) == 2);
    CHECK(target.block_references(3) == 2);
    CHECK(target.release("fill").freed.empty() && target.block_references(3) == 1);
    CHECK(target.evictable_blocks() == 1 && target.check_invariants());
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

// Full blocks enter the prefix index as requests fill them (B = 2).
void indexing_tests() {
    Config config = small_config(true);
    config.capacity = 8;
    KVCacheManager manager(config);
    // ADMIT: block 0 = [1,2] is full and indexed; block 1 = [3] is partial.
    CHECK(manager.admit("r1", {1, 2, 3}).blocks == std::vector<BlockId>({0, 1}));
    CHECK(manager.is_indexed(0) && !manager.is_indexed(1) && manager.indexed_blocks() == 1);
    CHECK(manager.block_references(0) == 2 && manager.block_references(1) == 1);
    // APPEND fills block 1, which is indexed under block 0.
    CHECK(manager.append("r1", 4).status == Status::Ok);
    CHECK(manager.is_indexed(1) && manager.block_references(1) == 2);
    CHECK(manager.evictable_blocks() == 0 && manager.check_invariants());

    // Two requests fill the same prefix independently: the first block to
    // become full is indexed, the second stays private.
    CHECK(manager.admit("a", {7}).blocks == std::vector<BlockId>({2}));
    CHECK(manager.admit("b", {7}).blocks == std::vector<BlockId>({3}));
    CHECK(manager.append("a", 8).status == Status::Ok);
    CHECK(manager.append("b", 8).status == Status::Ok);
    CHECK(manager.is_indexed(2) && !manager.is_indexed(3));
    CHECK(manager.block_references(2) == 2 && manager.block_references(3) == 1);
    CHECK(manager.read_request("a") == manager.read_request("b"));

    // RELEASE keeps indexed blocks cached; only the leaf becomes evictable.
    OpResult result = manager.release("r1");
    CHECK(result.status == Status::Ok && result.freed.empty());
    CHECK(manager.block_references(0) == 1 && manager.block_references(1) == 1);
    CHECK(manager.indexed_blocks() == 3 && manager.evictable_blocks() == 1);
    CHECK(manager.release("a").freed.empty() && manager.evictable_blocks() == 2);

    // b holds private block 3 for [7,8]; its next full block [9,10] is indexed
    // under cached block 2, which then stops being a leaf and is not evictable.
    CHECK(manager.append("b", 9).blocks == std::vector<BlockId>({3, 4}));
    CHECK(!manager.is_indexed(4) && manager.evictable_blocks() == 2);
    CHECK(manager.append("b", 10).status == Status::Ok);
    CHECK(manager.is_indexed(4) && manager.block_references(4) == 2);
    CHECK(manager.evictable_blocks() == 1 && manager.check_invariants());
    // Releasing b frees its private block; block 4 is now the evictable leaf.
    CHECK(manager.release("b").freed == std::vector<BlockId>({3}));
    CHECK(manager.indexed_blocks() == 4 && manager.evictable_blocks() == 2);
    CHECK(manager.stats().used_blocks == 4 && manager.check_invariants());

    // A fork shares blocks; the block it fills by copy-on-write is indexed too.
    CHECK(manager.admit("f1", {20, 21, 22}).status == Status::Ok);
    CHECK(manager.fork("f2", "f1").status == Status::Ok);
    result = manager.append("f2", 23);
    CHECK(result.status == Status::Ok && result.cow_copies == 1);
    CHECK(manager.is_indexed(result.blocks.at(1)) && manager.indexed_blocks() == 6);
    CHECK(manager.block_references(result.blocks.at(0)) == 3 && manager.check_invariants());
}

// Hand-calculated prefix-reuse trace. B = 2, capacity 8, blocks are handed out
// in the order 0, 1, 2, ... while none has been freed.
//
//   op                         match    cached  blocks    new blocks
//   ADMIT p1 1 2 3 4 5         -        0       [0,1,2]   0=[1,2] 1=[3,4] 2=[5]
//   ADMIT p2 1 2 3 4 9 9       [0,1]    4       [0,1,3]   3=[9,9]
//   ADMIT p3 1 2 7             [0]      2       [0,4]     4=[7]
//   ADMIT p4 5 2 3 4           -        0       [5,6]     5=[5,2] 6=[3,4]
//   ADMIT p5 1 2 3 4           [0,1]    4       [0,1]     none
//   APPEND p5 5                                 [0,1,7]   7=[5]
//
// Indexed (full) blocks: 0, 1, 3, 5, 6. Partial tails 2, 4, 7 are never shared,
// even though blocks 2 and 7 hold the same token after the same prefix.
// References = requests holding the block + 1 if indexed:
//   0: p1 p2 p3 p5 + index = 5     1: p1 p2 p5 + index = 4     3: p2 + index = 2
void prefix_reuse_tests() {
    Config config = small_config(true);
    config.capacity = 8;
    KVCacheManager manager(config);
    using Blocks = std::vector<BlockId>;

    OpResult result = manager.admit("p1", {1, 2, 3, 4, 5});
    CHECK(result.blocks == Blocks({0, 1, 2}) && result.cached_tokens == 0);
    result = manager.admit("p2", {1, 2, 3, 4, 9, 9});
    CHECK(result.blocks == Blocks({0, 1, 3}) && result.cached_tokens == 4);
    result = manager.admit("p3", {1, 2, 7});
    CHECK(result.blocks == Blocks({0, 4}) && result.cached_tokens == 2);
    // Same tokens 2 3 4 at the same positions, but a different first token:
    // nothing may be reused, and block 6 = [3,4] differs from block 1 = [3,4].
    result = manager.admit("p4", {5, 2, 3, 4});
    CHECK(result.blocks == Blocks({5, 6}) && result.cached_tokens == 0);
    // A prompt that is cached completely needs no new block.
    result = manager.admit("p5", {1, 2, 3, 4});
    CHECK(result.blocks == Blocks({0, 1}) && result.cached_tokens == 4);
    CHECK(manager.stats().used_blocks == 7);
    result = manager.append("p5", 5);
    CHECK(result.blocks == Blocks({0, 1, 7}) && result.cow_copies == 0);

    CHECK(manager.block_references(0) == 5 && manager.block_references(1) == 4);
    CHECK(manager.block_references(3) == 2 && manager.block_references(2) == 1);
    CHECK(manager.indexed_blocks() == 5 && manager.evictable_blocks() == 0);
    for (BlockId block : {0, 1, 3, 5, 6}) CHECK(manager.is_indexed(block));
    for (BlockId block : {2, 4, 7}) CHECK(!manager.is_indexed(block));

    // Every request reads exactly what it would read without any sharing.
    CHECK(manager.read_request("p1") == expected_bytes({1, 2, 3, 4, 5}));
    CHECK(manager.read_request("p2") == expected_bytes({1, 2, 3, 4, 9, 9}));
    CHECK(manager.read_request("p3") == expected_bytes({1, 2, 7}));
    CHECK(manager.read_request("p4") == expected_bytes({5, 2, 3, 4}));
    CHECK(manager.read_request("p5") == expected_bytes({1, 2, 3, 4, 5}));
    Stats stats = manager.stats();
    CHECK(stats.prompt_tokens == 22 && stats.cached_tokens == 10 && stats.uncached_tokens == 12);
    CHECK(stats.used_blocks == 8 && stats.fragmentation_slots == 3);
    CHECK(manager.check_invariants());

    // Reject with a partial hit: needs 1 block beyond [0,1], none free and none
    // evictable. The pins taken for the match are dropped again.
    result = manager.admit("p6", {1, 2, 3, 4, 8});
    CHECK(result.status == Status::Reject && !manager.has_request("p6"));
    CHECK(manager.block_references(0) == 5 && manager.block_references(1) == 4);
    CHECK(manager.stats().cached_tokens == 10 && manager.check_invariants());

    // The cache outlives the requests that filled it. Releasing p1, p2 and p5
    // frees only their partial tails (2 and 7); 0, 1, 3 stay cached and block 3
    // becomes the evictable leaf. p7 then reuses all three.
    CHECK(manager.release("p1").freed == Blocks({2}));
    CHECK(manager.release("p2").freed.empty());
    CHECK(manager.release("p5").freed == Blocks({7}));
    CHECK(manager.block_references(0) == 2 && manager.block_references(1) == 1);
    CHECK(manager.block_references(3) == 1 && manager.evictable_blocks() == 1);
    result = manager.admit("p7", {1, 2, 3, 4, 9, 9, 1});
    CHECK(result.status == Status::Ok && result.cached_tokens == 6);
    CHECK(result.blocks.size() == 4 && result.blocks[0] == 0 && result.blocks[1] == 1);
    CHECK(result.blocks[2] == 3 && manager.evictable_blocks() == 0);
    CHECK(manager.read_request("p7") == expected_bytes({1, 2, 3, 4, 9, 9, 1}));
    CHECK(manager.check_invariants());
}

// RELEASE tells the policy which cached blocks became evictable: only indexed
// leaves that no request holds any more (B = 2).
void release_tests() {
    Config config = small_config(true);
    config.capacity = 8;
    KVCacheManager manager(config);
    using Blocks = std::vector<BlockId>;
    CHECK(manager.admit("a", {1, 2, 3, 4}).blocks == Blocks({0, 1}));
    CHECK(manager.fork("b", "a").blocks == Blocks({0, 1}));
    CHECK(manager.block_references(1) == 3);
    // b still holds both blocks: nothing is freed and nothing is evictable.
    CHECK(manager.release("a").freed.empty() && manager.evictable_blocks() == 0);
    // Last holder gone: leaf 1 is evictable; its parent 0 is not (it has a child).
    CHECK(manager.release("b").freed.empty() && manager.evictable_blocks() == 1);
    CHECK(manager.block_references(0) == 1 && manager.block_references(1) == 1);
    CHECK(manager.check_invariants());

    // Holding only the parent leaves the leaf evictable, before and after.
    OpResult result = manager.admit("c", {1, 2});
    CHECK(result.blocks == Blocks({0}) && result.cached_tokens == 2);
    CHECK(manager.evictable_blocks() == 1);
    CHECK(manager.release("c").freed.empty() && manager.evictable_blocks() == 1);
    // Holding the leaf pins it; releasing makes it evictable again.
    CHECK(manager.admit("d", {1, 2, 3, 4}).cached_tokens == 4);
    CHECK(manager.evictable_blocks() == 0 && manager.check_invariants());
    CHECK(manager.release("d").freed.empty() && manager.evictable_blocks() == 1);

    // A shared partial tail is not indexed: it is freed by its last holder.
    CHECK(manager.admit("e", {8}).blocks == Blocks({2}));
    CHECK(manager.fork("f", "e").blocks == Blocks({2}));
    CHECK(manager.release("e").freed.empty());
    CHECK(manager.release("f").freed == Blocks({2}));
    CHECK(manager.indexed_blocks() == 2 && manager.evictable_blocks() == 1);
    CHECK(manager.stats().used_blocks == 2 && manager.check_invariants());
}

// Hand-calculated eviction trace, run once per policy. B = 2, capacity 4.
// The free list is a stack: the block freed last is allocated first.
// "rank" is the policy's clock value; the evictable block with the smallest
// rank is the victim. Only LRU gives a block a new rank when it is reused.
//
//  #  op                   LRU                              FIFO
//  1  ADMIT a 1 2          blocks [0]                       same
//  2  ADMIT b 3 4          blocks [1]                       same
//  3  ADMIT c 5 6          blocks [2]                       same
//  4  RELEASE a, b, c      evictable {0,1,2}, free {3}      same
//                          ranks 0:1 1:2 2:3                ranks 0:1 1:2 2:3
//  5  ADMIT d 1 2          hit [0], cached 2; rank 0:4      hit [0], cached 2; rank 0:1
//  6  RELEASE d            evictable {0,1,2}                evictable {0,1,2}
//  7  ADMIT e 7 8 9 10     needs 2, free 1: evict 1         needs 2, free 1: evict 0
//                          blocks [1,3]; ranks 1:5 3:6      blocks [0,3]; ranks 0:4 3:5
//  8  ADMIT f 1 2          hit [0], cached 2; rank 0:7      miss: evict 1, blocks [1]
//                                                           cached 0; rank 1:6
//  9  RELEASE e            evictable {2,3}                  evictable {2,3}
//                          (1 = [7,8] has child 3)          (0 = [7,8] has child 3)
// 10  ADMIT g 20..24       needs 3, free 0, evictable 2,    needs 3, free 0, evictable 2,
//                          reclaimable 3 (2, 3, then 1)     reclaimable 3 (2, 3, then 0)
//                          evict 2, 3, 1; blocks [1,3,2]    evict 2, 3, 0; blocks [0,3,2]
//
// Totals: LRU 4 evictions, 4 cached tokens; FIFO 5 evictions, 2 cached tokens.
// Step 8 is the difference: LRU kept the block that d had just reused.
// Step 10 needs the cascade: only two leaves are evictable, the third block
// becomes one after its child is evicted.
void eviction_trace_tests() {
    using Blocks = std::vector<BlockId>;
    for (PolicyKind kind : {PolicyKind::LRU, PolicyKind::FIFO}) {
        const bool lru = kind == PolicyKind::LRU;
        Config config = small_config(true);
        config.policy = kind;
        KVCacheManager manager(config);

        CHECK(manager.admit("a", {1, 2}).blocks == Blocks({0}));
        CHECK(manager.admit("b", {3, 4}).blocks == Blocks({1}));
        CHECK(manager.admit("c", {5, 6}).blocks == Blocks({2}));
        for (const char* id : {"a", "b", "c"}) CHECK(manager.release(id).freed.empty());
        CHECK(manager.evictable_blocks() == 3 && manager.stats().used_blocks == 3);

        OpResult result = manager.admit("d", {1, 2});
        CHECK(result.blocks == Blocks({0}) && result.cached_tokens == 2);
        CHECK(result.evicted.empty() && manager.evictable_blocks() == 2);
        CHECK(manager.release("d").freed.empty() && manager.evictable_blocks() == 3);

        result = manager.admit("e", {7, 8, 9, 10});
        CHECK(result.status == Status::Ok && result.cached_tokens == 0);
        CHECK(result.evicted == (lru ? Blocks({1}) : Blocks({0})));
        CHECK(result.blocks == (lru ? Blocks({1, 3}) : Blocks({0, 3})));
        CHECK(manager.evictable_blocks() == 2 && manager.check_invariants());

        result = manager.admit("f", {1, 2});
        CHECK(result.status == Status::Ok);
        CHECK(result.cached_tokens == (lru ? 2u : 0u));
        CHECK(result.evicted == (lru ? Blocks({}) : Blocks({1})));
        CHECK(result.blocks == (lru ? Blocks({0}) : Blocks({1})));
        CHECK(manager.read_request("f") == expected_bytes({1, 2}));
        CHECK(manager.evictable_blocks() == 1); // block 2 = [5,6]

        CHECK(manager.release("e").freed.empty());
        CHECK(manager.evictable_blocks() == 2 && manager.stats().used_blocks == 4);
        CHECK(manager.check_invariants());

        // Three blocks are needed but only two leaves are evictable right now.
        result = manager.admit("g", {20, 21, 22, 23, 24});
        CHECK(result.status == Status::Ok && result.cached_tokens == 0);
        CHECK(result.evicted == (lru ? Blocks({2, 3, 1}) : Blocks({2, 3, 0})));
        CHECK(result.blocks == (lru ? Blocks({1, 3, 2}) : Blocks({0, 3, 2})));
        CHECK(manager.read_request("g") == expected_bytes({20, 21, 22, 23, 24}));
        CHECK(manager.read_request("f") == expected_bytes({1, 2}));

        Stats stats = manager.stats();
        CHECK(stats.evictions == (lru ? 4u : 5u) && stats.cached_tokens == (lru ? 4u : 2u));
        CHECK(stats.prompt_tokens == 19 && stats.rejected == 0 && stats.used_blocks == 4);
        CHECK(manager.indexed_blocks() == 3 && manager.evictable_blocks() == 0);
        CHECK(manager.check_invariants());
    }
}

// Capacity checks with eviction: all-or-nothing, and a matched prefix is never
// evicted to make room for the request that matched it.
void eviction_capacity_tests() {
    using Blocks = std::vector<BlockId>;
    Config config = small_config(true);
    config.capacity = 2;
    KVCacheManager manager(config);
    CHECK(manager.admit("a", {1, 2, 3, 4}).blocks == Blocks({0, 1}));
    // Everything is held: no room, by ADMIT or by APPEND.
    CHECK(manager.admit("x", {9}).status == Status::Reject);
    CHECK(manager.append("a", 5).status == Status::Reject);
    CHECK(manager.release("a").freed.empty() && manager.evictable_blocks() == 1);

    // h matches both cached blocks and needs a third. The only cached blocks
    // are the ones it uses, so it is rejected and nothing is evicted.
    OpResult result = manager.admit("h", {1, 2, 3, 4, 5});
    CHECK(result.status == Status::Reject && result.evicted.empty());
    CHECK(result.message == "needs 1 block, 0 free, 0 evictable");
    CHECK(manager.indexed_blocks() == 2 && manager.evictable_blocks() == 1);
    CHECK(manager.block_references(0) == 1 && manager.block_references(1) == 1);
    CHECK(manager.stats().evictions == 0 && manager.check_invariants());
    // Needs 3 with 2 reclaimable: rejected before a single block is evicted.
    result = manager.admit("big", {9, 9, 9, 9, 9});
    CHECK(result.status == Status::Reject);
    CHECK(result.message == "needs 3 blocks, 0 free, 2 evictable");
    CHECK(manager.indexed_blocks() == 2 && manager.stats().evictions == 0);

    // i reuses block 0 and takes the place of the evicted leaf 1.
    result = manager.admit("i", {1, 2, 9});
    CHECK(result.status == Status::Ok && result.cached_tokens == 2);
    CHECK(result.evicted == Blocks({1}) && result.blocks == Blocks({0, 1}));
    CHECK(manager.read_request("i") == expected_bytes({1, 2, 9}));
    // i fills block 1; its next token needs a block, and none is free or
    // evictable while i itself holds both. APPEND evicts as well: once i is
    // released, k's append takes the cached leaf [9,9].
    CHECK(manager.append("i", 9).status == Status::Ok);
    CHECK(manager.append("i", 9).status == Status::Reject);
    CHECK(manager.release("i").freed.empty() && manager.evictable_blocks() == 1);
    CHECK(manager.admit("k", {1, 2}).blocks == Blocks({0}));
    result = manager.append("k", 7);
    CHECK(result.status == Status::Ok && result.evicted == Blocks({1}));
    CHECK(result.blocks == Blocks({0, 1}) && manager.stats().evictions == 2);
    CHECK(manager.read_request("k") == expected_bytes({1, 2, 7}));
    CHECK(manager.check_invariants());
}

// An ancestor indexed by another request is evicted while this request still
// holds a private copy of it; the next block it fills must heal the chain.
// B = 4, capacity 3.
void heal_tests() {
    using Blocks = std::vector<BlockId>;
    Config config = small_config(true);
    config.capacity = 3;
    config.tokens_per_block = 4;
    KVCacheManager manager(config);
    CHECK(manager.admit("r1", {1, 2, 3}).blocks == Blocks({0}));
    CHECK(manager.admit("r2", {1, 2, 3}).blocks == Blocks({1}));
    CHECK(manager.append("r1", 4).status == Status::Ok); // block 0 is indexed
    CHECK(manager.append("r2", 4).status == Status::Ok); // block 1 stays private
    CHECK(manager.is_indexed(0) && !manager.is_indexed(1));
    CHECK(manager.release("r1").freed.empty() && manager.evictable_blocks() == 1);

    // Fill the pool, then r2's next token evicts block 0 and reuses it.
    CHECK(manager.admit("x", {9}).blocks == Blocks({2}));
    OpResult result = manager.append("r2", 5);
    CHECK(result.status == Status::Ok && result.evicted == Blocks({0}));
    CHECK(result.blocks == Blocks({1, 0}) && manager.indexed_blocks() == 0);
    CHECK(manager.check_invariants());

    // r2 fills its second block. Prefix [1,2,3,4] is no longer indexed, so both
    // of r2's blocks are inserted: its private block 1, then block 0 under it.
    CHECK(manager.append("r2", 6).status == Status::Ok);
    CHECK(manager.append("r2", 7).status == Status::Ok && manager.indexed_blocks() == 0);
    CHECK(manager.append("r2", 8).status == Status::Ok);
    CHECK(manager.indexed_blocks() == 2 && manager.is_indexed(1) && manager.is_indexed(0));
    CHECK(manager.block_references(1) == 2 && manager.block_references(0) == 2);
    CHECK(manager.evictable_blocks() == 0 && manager.check_invariants());
    CHECK(manager.read_request("r2") == expected_bytes({1, 2, 3, 4, 5, 6, 7, 8}));

    // The healed chain is usable: a new request finds both blocks.
    result = manager.admit("y", {1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(result.status == Status::Ok && result.cached_tokens == 8);
    CHECK(result.blocks == Blocks({1, 0}) && manager.check_invariants());
}

// Seeded random traces on a small pool with a tiny token alphabet, so prefixes
// collide and eviction runs constantly. After every operation the invariants
// hold and the touched request reads what its tokens alone determine; a Reject
// changes nothing.
void random_tests() {
    const char* names[] = {"q0", "q1", "q2", "q3", "q4", "q5"};
    std::size_t operations = 0;
    for (PolicyKind kind : {PolicyKind::LRU, PolicyKind::FIFO})
        for (bool sharing : {true, false})
            for (unsigned seed = 1; seed <= 20; ++seed) {
                Config config = small_config(sharing);
                config.capacity = 10;
                config.bytes_per_token = 8;
                config.policy = kind;
                KVCacheManager manager(config);
                std::mt19937 random(seed);
                std::map<std::string, std::vector<Token>> model;
                auto pick = [&](unsigned n) { return static_cast<unsigned>(random() % n); };
                for (int step = 0; step < 300; ++step, ++operations) {
                    std::string id = names[pick(6)];
                    Stats before = manager.stats();
                    auto indexed = manager.indexed_blocks();
                    OpResult result;
                    unsigned op = pick(10);
                    if (!model.count(id) && op < 7) {
                        std::vector<Token> prompt(pick(8));
                        for (Token& token : prompt) token = static_cast<Token>(pick(2));
                        result = manager.admit(id, prompt);
                        if (result.status == Status::Ok) model[id] = prompt;
                    } else if (!model.count(id)) {
                        std::string source = names[pick(6)];
                        result = manager.fork(id, source);
                        CHECK((result.status == Status::Error) == !model.count(source));
                        if (result.status == Status::Ok) model[id] = model[source];
                    } else if (op < 7) {
                        Token token = static_cast<Token>(pick(2));
                        result = manager.append(id, token);
                        if (result.status == Status::Ok) model[id].push_back(token);
                    } else {
                        result = manager.release(id);
                        CHECK(result.status == Status::Ok);
                        model.erase(id);
                    }
                    CHECK(manager.check_invariants());
                    Stats after = manager.stats();
                    if (result.status != Status::Ok) {
                        CHECK(result.evicted.empty() && after.evictions == before.evictions);
                        CHECK(after.used_blocks == before.used_blocks);
                        CHECK(manager.indexed_blocks() == indexed);
                    }
                    CHECK(after.evictions == before.evictions + result.evicted.size());
                    CHECK(after.cached_tokens + after.uncached_tokens == after.prompt_tokens);
                    if (!sharing) CHECK(after.evictions == 0 && after.cached_tokens == 0);
                    for (const auto& entry : model)
                        CHECK(manager.read_request(entry.first) ==
                              expected_bytes(entry.second, 42, 8));
                    CHECK(manager.has_request(id) == (model.count(id) != 0));
                }
            }
    CHECK(operations == 24000);
}

bool same_result(const OpResult& a, const OpResult& b) {
    return a.status == b.status && a.blocks == b.blocks && a.cached_tokens == b.cached_tokens &&
           a.cow_copies == b.cow_copies && a.freed == b.freed && a.evicted == b.evicted &&
           a.message == b.message;
}

// reclaimable=scan and reclaimable=count must be indistinguishable from outside.
// Two managers that differ only in that key replay the same seeded operations;
// every result (status, block IDs, evicted and freed lists, reject message) and
// every counter must be equal after every step. Small pool, two-token alphabet:
// constant eviction, many rejects with partial prefix hits.
void mode_equivalence_tests() {
    const char* names[] = {"q0", "q1", "q2", "q3", "q4", "q5"};
    std::size_t operations = 0, rejects = 0, evictions = 0;
    for (PolicyKind kind : {PolicyKind::LRU, PolicyKind::FIFO})
        for (unsigned seed = 101; seed <= 130; ++seed) {
            Config config = small_config(true);
            config.capacity = 10;
            config.bytes_per_token = 8;
            config.policy = kind;
            config.reclaimable = ReclaimableKind::Scan;
            KVCacheManager scan(config);
            config.reclaimable = ReclaimableKind::Count;
            KVCacheManager count(config);
            std::mt19937 random(seed);
            auto pick = [&](unsigned n) { return static_cast<unsigned>(random() % n); };
            for (int step = 0; step < 300; ++step, ++operations) {
                std::string id = names[pick(6)];
                unsigned op = pick(10);
                OpResult a, b;
                if (!scan.has_request(id) && op < 7) {
                    std::vector<Token> prompt(pick(9));
                    for (Token& token : prompt) token = static_cast<Token>(pick(2));
                    a = scan.admit(id, prompt);
                    b = count.admit(id, prompt);
                } else if (!scan.has_request(id)) {
                    std::string source = names[pick(6)];
                    a = scan.fork(id, source);
                    b = count.fork(id, source);
                } else if (op < 7) {
                    Token token = static_cast<Token>(pick(2));
                    a = scan.append(id, token);
                    b = count.append(id, token);
                } else {
                    a = scan.release(id);
                    b = count.release(id);
                }
                CHECK(same_result(a, b));
                CHECK(scan.has_request(id) == count.has_request(id));
                if (scan.has_request(id)) CHECK(scan.read_request(id) == count.read_request(id));
                Stats sa = scan.stats(), sb = count.stats();
                CHECK(sa.admitted == sb.admitted && sa.rejected == sb.rejected);
                CHECK(sa.cached_tokens == sb.cached_tokens && sa.evictions == sb.evictions);
                CHECK(sa.used_blocks == sb.used_blocks && sa.cow_copies == sb.cow_copies);
                CHECK(scan.indexed_blocks() == count.indexed_blocks());
                CHECK(scan.evictable_blocks() == count.evictable_blocks());
                CHECK(scan.check_invariants() && count.check_invariants());
                if (a.status == Status::Reject) ++rejects;
                evictions += a.evicted.size();
            }
        }
    // The comparison only means something if the slow path ran a lot.
    CHECK(operations == 18000 && rejects > 500 && evictions > 2000);
}

// The reclaimable key: default, both values, and a bad value with its line number.
void config_tests() {
    const char* path = "manager_tests_config.tmp";
    auto load = [&](const std::string& text) {
        { std::ofstream out(path); out << text; }
        try {
            Config config = load_config(path);
            std::remove(path);
            return config;
        } catch (...) {
            std::remove(path);
            throw;
        }
    };
    CHECK(Config().reclaimable == ReclaimableKind::Count);
    CHECK(load("capacity=8\n").reclaimable == ReclaimableKind::Count);
    CHECK(load("reclaimable=count\n").reclaimable == ReclaimableKind::Count);
    CHECK(load("capacity=8\nreclaimable = scan  # old path\n").reclaimable == ReclaimableKind::Scan);
    std::string message;
    try { load("capacity=8\nreclaimable=fast\n"); } catch (const std::runtime_error& e) { message = e.what(); }
    CHECK(message == "config line 2: reclaimable must be scan or count");
    message.clear();
    try { load("reclaimable=scan\nreclaimable=count\n"); } catch (const std::runtime_error& e) { message = e.what(); }
    CHECK(message == "config line 2: repeated key 'reclaimable'");
}

int main() {
    try {
        for (ReclaimableKind mode : {ReclaimableKind::Count, ReclaimableKind::Scan}) {
            reclaimable_mode = mode;
            const char* tag = mode == ReclaimableKind::Count ? " [reclaimable=count]\n"
                                                             : " [reclaimable=scan]\n";
            payload_tests(); std::cout << "PASS prefix-dependent payloads" << tag;
            cow_tests(); std::cout << "PASS proposal example, fork and copy-on-write" << tag;
            baseline_tests(); std::cout << "PASS sharing=off baseline" << tag;
            baseline_bypass_tests(); std::cout << "PASS sharing=off bypasses the prefix index" << tag;
            failure_tests(); std::cout << "PASS errors, rejects and moves leave state intact" << tag;
            wiring_tests(); std::cout << "PASS index and policy wiring" << tag;
            indexing_tests(); std::cout << "PASS full blocks are indexed on fill" << tag;
            prefix_reuse_tests(); std::cout << "PASS hand-calculated prefix-reuse trace" << tag;
            release_tests(); std::cout << "PASS release marks unheld cached leaves evictable" << tag;
            eviction_trace_tests(); std::cout << "PASS hand-calculated LRU and FIFO eviction trace" << tag;
            eviction_capacity_tests(); std::cout << "PASS eviction capacity checks are all-or-nothing" << tag;
            heal_tests(); std::cout << "PASS evicted ancestor is healed from the request's own blocks" << tag;
            random_tests(); std::cout << "PASS 24000 seeded manager operations" << tag;
        }
        mode_equivalence_tests();
        std::cout << "PASS 18000 operations give identical results with scan and count\n";
        config_tests(); std::cout << "PASS reclaimable config key\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
