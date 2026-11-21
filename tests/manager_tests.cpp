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

int main() {
    try {
        payload_tests(); std::cout << "PASS prefix-dependent payloads\n";
        cow_tests(); std::cout << "PASS proposal example, fork and copy-on-write\n";
        baseline_tests(); std::cout << "PASS sharing=off baseline\n";
        failure_tests(); std::cout << "PASS errors, rejects and moves leave state intact\n";
        wiring_tests(); std::cout << "PASS index and policy wiring\n";
        indexing_tests(); std::cout << "PASS full blocks are indexed on fill\n";
        prefix_reuse_tests(); std::cout << "PASS hand-calculated prefix-reuse trace\n";
        release_tests(); std::cout << "PASS release marks unheld cached leaves evictable\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
