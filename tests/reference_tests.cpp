#include "reference_cache.h"
#include "kvcache/kv_cache_manager.h"
#include "kvcache/trace.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace kvcache;
using reference::Key;
using reference::Tokens;

namespace {

struct Coverage {
    std::size_t operations = 0, rejected = 0, errors = 0;
    std::size_t hits = 0, copies = 0, evictions = 0;
} coverage;

struct Command {
    std::string op, id, source;
    Tokens tokens;
};

std::string describe(const Command& command) {
    std::ostringstream out;
    out << command.op << ' ' << command.id;
    if (command.op == "FORK") out << ' ' << command.source;
    for (Token token : command.tokens) out << ' ' << token;
    return out.str();
}

// The model decides first. Physical IDs are only names: bind a newly predicted
// logical block to its observed ID, then enforce that binding until it is freed.
class Comparison {
public:
    Comparison(Config config, const std::string& label)
        : config_(config), label_(label), expected_(config), actual_(config) {}

    OpResult admit(const std::string& id, const Tokens& tokens) {
        return run({"ADMIT", id, "", tokens});
    }
    OpResult append(const std::string& id, Token token) {
        return run({"APPEND", id, "", {token}});
    }
    OpResult fork(const std::string& id, const std::string& source) {
        return run({"FORK", id, source, {}});
    }
    OpResult release(const std::string& id) {
        return run({"RELEASE", id, "", {}});
    }
    const std::map<std::string, reference::Request>& requests() const {
        return expected_.requests();
    }
    const KVCacheManager& manager() const { return actual_; }

    void require(bool condition, const std::string& message) const {
        if (condition) return;
        std::ostringstream out;
        out << label_ << ": " << message << "\ncapacity=" << config_.capacity
            << " tokens_per_block=" << config_.tokens_per_block
            << " bytes_per_token=" << config_.bytes_per_token
            << " sharing=" << config_.sharing
            << " policy=" << (config_.policy == PolicyKind::LRU ? "lru" : "fifo")
            << " payload_seed=" << config_.seed << "\nTrace through failing operation:\n";
        for (const auto& line : trace_) out << line << '\n';
        throw std::runtime_error(out.str());
    }

private:
    std::vector<BlockId> physical(const std::vector<Key>& keys) const {
        std::vector<BlockId> ids;
        for (Key key : keys) {
            auto found = names_.find(key);
            require(found != names_.end(), "unbound logical block in result");
            ids.push_back(found->second);
        }
        return ids;
    }

    OpResult run(const Command& command) {
        trace_.push_back(describe(command));
        seen_.insert(command.id);
        if (command.op == "FORK") seen_.insert(command.source);
        reference::Result expected;
        OpResult actual;
        try {
            if (command.op == "ADMIT") expected = expected_.admit(command.id, command.tokens);
            if (command.op == "APPEND") expected = expected_.append(command.id, command.tokens.front());
            if (command.op == "FORK") expected = expected_.fork(command.id, command.source);
            if (command.op == "RELEASE") expected = expected_.release(command.id);
            if (command.op == "ADMIT") actual = actual_.admit(command.id, command.tokens);
            if (command.op == "APPEND") actual = actual_.append(command.id, command.tokens.front());
            if (command.op == "FORK") actual = actual_.fork(command.id, command.source);
            if (command.op == "RELEASE") actual = actual_.release(command.id);
        } catch (const std::exception& error) {
            require(false, std::string("unexpected exception: ") + error.what());
        }

        require(actual.status == expected.status, "operation status differs");
        require(actual.cached_tokens == expected.cached_tokens, "prefix hit differs");
        require(actual.cow_copies == expected.cow_copies, "copy-on-write count differs");
        require(actual.evicted == physical(expected.evicted), "eviction order differs");
        require(actual.freed == physical(expected.freed), "released blocks differ");
        require(actual.blocks.size() == expected.blocks.size(), "request block count differs");

        // Retire old names before binding new blocks: the pool can recycle IDs.
        for (auto it = names_.begin(); it != names_.end();) {
            if (!expected_.blocks().count(it->first)) it = names_.erase(it);
            else ++it;
        }
        for (std::size_t i = 0; i < expected.blocks.size(); ++i) {
            Key key = expected.blocks[i];
            if (!names_.count(key)) {
                require(actual.blocks[i] < config_.capacity, "physical block ID out of range");
                for (const auto& entry : names_)
                    require(entry.second != actual.blocks[i], "distinct logical blocks alias");
                names_.emplace(key, actual.blocks[i]);
            }
            require(names_.at(key) == actual.blocks[i], "block sharing or COW identity differs");
        }
        check_state();
        ++coverage.operations;
        coverage.rejected += actual.status == Status::Reject;
        coverage.errors += actual.status == Status::Error;
        coverage.hits += actual.cached_tokens;
        coverage.copies += actual.cow_copies;
        coverage.evictions += actual.evicted.size();
        return actual;
    }

    void check_state() const {
        require(names_.size() == expected_.blocks().size(), "allocated block count differs");
        for (const auto& id : seen_) {
            bool exists = expected_.requests().count(id) != 0;
            require(actual_.has_request(id) == exists, "request presence differs: " + id);
            if (exists)
                require(actual_.read_request(id) == expected_.read_request(id),
                        "request bytes differ: " + id);
        }
        std::vector<std::size_t> references(config_.capacity, 0);
        std::vector<bool> indexed(config_.capacity, false);
        for (const auto& name : names_) {
            references[name.second] = expected_.references(name.first);
            indexed[name.second] = expected_.is_indexed(name.first);
        }
        for (BlockId id = 0; id < config_.capacity; ++id) {
            require(actual_.block_references(id) == references[id], "reference count differs");
            require(actual_.is_indexed(id) == indexed[id], "cache membership differs");
        }
        require(actual_.indexed_blocks() == expected_.indexed_blocks(), "index size differs");
        require(actual_.evictable_blocks() == expected_.evictable_blocks(), "eligible leaf count differs");
        auto actual = actual_.stats();
        auto expected = expected_.stats();
#define CHECK_STAT(field) require(actual.field == expected.field, "statistic differs: " #field)
        CHECK_STAT(admitted); CHECK_STAT(rejected); CHECK_STAT(errors);
        CHECK_STAT(prompt_tokens); CHECK_STAT(cached_tokens); CHECK_STAT(uncached_tokens);
        CHECK_STAT(cow_copies); CHECK_STAT(evictions); CHECK_STAT(used_blocks);
        CHECK_STAT(peak_used_blocks); CHECK_STAT(fragmentation_slots);
#undef CHECK_STAT
        require(actual_.check_invariants(), "implementation invariant failed");
    }

    Config config_;
    std::string label_;
    reference::ReferenceCache expected_;
    KVCacheManager actual_;
    std::map<Key, BlockId> names_;
    std::set<std::string> seen_;
    std::vector<std::string> trace_;
};

// main() runs the hand and random traces once per mode (count, then scan).
ReclaimableKind reclaimable_mode = ReclaimableKind::Count;

Config config(std::size_t capacity, std::size_t width, bool sharing = true,
              PolicyKind policy = PolicyKind::LRU) {
    Config result;
    result.reclaimable = reclaimable_mode;
    result.capacity = capacity;
    result.tokens_per_block = width;
    result.bytes_per_token = 8;
    result.sharing = sharing;
    result.policy = policy;
    return result;
}

void hand_traces() {
    {
        Config cfg = config(4, 2);
        cfg.bytes_per_token = 64;
        cfg.seed = std::numeric_limits<std::uint64_t>::max();
        Comparison h(cfg, "token and seed boundaries, repeated payload bytes");
        Token high = std::numeric_limits<Token>::max();
        h.admit("a", {0, high, 0});
        h.require(h.admit("b", {0, high, 0}).cached_tokens == 2,
                  "partial tail must not be a prefix hit");
        h.fork("c", "a");
        h.require(h.append("c", high).cow_copies == 1, "shared partial tail copied");
        h.release("a"); h.release("b"); h.release("c");
    }
    {
        Comparison h(config(4, 2), "proposal example");
        auto parent = h.admit("r1", {10, 11, 12});
        h.require(parent.status == Status::Ok && parent.cached_tokens == 0, "initial admit");
        auto child = h.fork("r2", "r1");
        h.require(child.blocks == parent.blocks, "fork must share");
        auto appended = h.append("r2", 13);
        h.require(appended.cow_copies == 1 && appended.blocks[0] == parent.blocks[0] &&
                  appended.blocks[1] != parent.blocks[1], "private tail after COW");
        h.require(h.release("r1").freed == std::vector<BlockId>{parent.blocks[1]}, "parent-first release");
        h.require(h.admit("r3", {10, 11, 12, 13}).cached_tokens == 4, "both full blocks cached");
        h.release("r2"); h.release("r3");
    }
    for (bool sharing : {false, true}) {
        Comparison h(config(1, 2, sharing), "COW capacity and baseline");
        h.admit("p", {1});
        auto forked = h.fork("c", "p");
        h.require(forked.status == (sharing ? Status::Ok : Status::Reject), "fork capacity");
        if (sharing) {
            h.require(h.append("c", 2).status == Status::Reject, "COW must reject at capacity");
            h.release("p");
            auto appended = h.append("c", 2);
            h.require(appended.status == Status::Ok && appended.cow_copies == 0, "tail now exclusive");
            h.release("c");
        } else {
            h.release("p");
        }
        h.admit("empty", {}); h.fork("empty-child", "empty");
        h.release("empty"); h.release("empty-child");
        h.append("missing", 0); h.release("missing"); h.fork("new", "missing");
    }
    for (PolicyKind policy : {PolicyKind::LRU, PolicyKind::FIFO}) {
        Comparison h(config(2, 1, true, policy), "LRU/FIFO and rejected-hit rollback");
        auto first = h.admit("a", {1});
        auto second = h.admit("b", {2});
        h.release("a"); h.release("b");
        h.require(h.admit("too-big", {1, 3, 4}).status == Status::Reject, "oversized admission");
        auto next = h.admit("x", {9});
        h.require(next.evicted == first.blocks, "rejected hit must not refresh LRU");
        auto x_blocks = next.blocks;
        h.release("x");
        h.require(h.admit("hit", {2}).cached_tokens == 1, "successful hit");
        h.release("hit");
        next = h.admit("last", {8});
        h.require(next.evicted == (policy == PolicyKind::LRU ? x_blocks : second.blocks),
                  "successful hit changes LRU but not FIFO");
    }
    {
        Comparison h(config(3, 2), "private duplicate pins cached ancestor through descendant");
        auto a = h.admit("a", {1});
        auto b = h.admit("b", {1});
        h.require(a.blocks != b.blocks, "partial prefixes are private");
        h.append("a", 2); h.append("b", 2); h.release("a");
        h.append("b", 3); h.append("b", 4);
        h.require(h.admit("x", {9}).status == Status::Reject, "pinned descendant protects ancestor");
        h.release("b");
        h.require(h.admit("whole", {1, 2, 3, 4}).cached_tokens == 4, "canonical prefix reused");
    }
    {
        Comparison h(config(3, 4), "restore evicted ancestor from private duplicate");
        h.admit("a", {1, 2, 3}); h.admit("b", {1, 2, 3});
        h.append("a", 4); h.append("b", 4); h.release("a");
        h.admit("x", {9});
        h.require(h.append("b", 5).evicted.size() == 1, "evict canonical ancestor");
        h.append("b", 6); h.append("b", 7); h.append("b", 8);
        h.require(h.admit("hit", {1, 2, 3, 4, 5, 6, 7, 8}).cached_tokens == 8, "healed full prefix");
        h.release("b"); h.release("hit"); h.release("x");
    }
    {
        Comparison h(config(3, 1), "cascading leaf eviction and branch divergence");
        auto old = h.admit("old", {1, 2, 3}); h.release("old");
        auto replaced = h.admit("new", {8, 9, 10});
        auto reverse = old.blocks; std::reverse(reverse.begin(), reverse.end());
        h.require(replaced.evicted == reverse, "leaf-to-root cascade");
        h.release("new");
        h.admit("branch", {8, 9, 11});
        h.admit("branch", {}); h.fork("branch", "branch");
        h.require(h.admit("no-room", {8, 7, 6}).status == Status::Reject, "temporary pins on reject");
        h.release("branch");
    }
}

void random_traces(unsigned seed, std::size_t steps) {
    for (bool sharing : {false, true})
        for (PolicyKind policy : {PolicyKind::LRU, PolicyKind::FIFO})
            for (std::size_t width : {1, 2, 4, 16})
                for (std::size_t capacity : {1, 3, 8}) {
                    Config cfg = config(capacity, width, sharing, policy);
                    cfg.bytes_per_token = capacity == 1 ? 1 : capacity == 3 ? 8 : 64;
                    cfg.seed = seed;
                    Comparison h(cfg, "random seed=" + std::to_string(seed));
                    std::mt19937 random(seed);
                    auto pick = [&](std::size_t count) { return static_cast<std::size_t>(random()) % count; };
                    for (std::size_t step = 0; step < steps; ++step) {
                        std::string id = "r" + std::to_string(pick(6));
                        auto operation = pick(100);
                        // Mostly valid lifecycles, plus deliberate duplicate/unknown IDs.
                        auto active_id = [&]() {
                            if (h.requests().empty()) return id;
                            auto it = h.requests().begin();
                            std::advance(it, pick(h.requests().size()));
                            return it->first;
                        };
                        if (pick(5) != 0) {
                            if (operation < 38 || (operation >= 65 && operation < 82)) {
                                std::vector<std::string> unused;
                                for (int i = 0; i < 6; ++i) {
                                    std::string name = "r" + std::to_string(i);
                                    if (!h.requests().count(name)) unused.push_back(name);
                                }
                                if (!unused.empty()) id = unused[pick(unused.size())];
                            } else id = active_id();
                        }
                        auto found = h.requests().find(id);
                        if (operation < 38) {
                            Tokens prompt;
                            // Reuse an expected request's prefix, then branch or extend.
                            if (!h.requests().empty() && pick(2)) {
                                auto source = h.requests().begin();
                                std::advance(source, pick(h.requests().size()));
                                prompt = source->second.tokens;
                                prompt.resize(pick(prompt.size() + 1));
                            }
                            std::size_t length = pick(4 * width + 2);
                            bool common_prefix = pick(2) != 0;
                            while (prompt.size() < length) {
                                auto token = common_prefix && prompt.size() < width ?
                                             prompt.size() % 3 : pick(3);
                                prompt.push_back(static_cast<Token>(token));
                            }
                            if (prompt.size() > length) prompt.resize(length);
                            h.admit(id, prompt);
                        } else if (operation < 65) {
                            // Bound live lengths so pressure tests stay small and quick.
                            if (found != h.requests().end() && found->second.tokens.size() > 5 * width)
                                h.release(id);
                            else h.append(id, static_cast<Token>(pick(3)));
                        } else if (operation < 82) {
                            std::string source = pick(5) ? active_id() : "r" + std::to_string(pick(6));
                            h.fork(id, source);
                        } else h.release(id);
                    }
                    // Every run finishes all live lifecycles, in both release orders.
                    std::vector<std::string> live;
                    for (const auto& entry : h.requests()) live.push_back(entry.first);
                    if (seed % 2) std::reverse(live.begin(), live.end());
                    for (const auto& id : live) h.release(id);
                }
}

void parser_checks() {
    for (const std::string line : {"UNKNOWN r", "ADMIT", "ADMIT r -1", "ADMIT r 1x",
             "ADMIT r 2147483648", "APPEND r", "APPEND r 1 2", "FORK r",
             "FORK r r", "RELEASE", "RELEASE r extra", "STATS extra"}) {
        auto result = parse_line(line);
        if (result.ok || result.error.empty())
            throw std::runtime_error("malformed command accepted: " + line);
    }
    auto empty = parse_line("ADMIT empty");
    auto prompt = parse_line("ADMIT r 0 2147483647");
    auto forked = parse_line("FORK child parent");
    if (!empty.ok || empty.cmd.type != CommandType::Admit || !empty.cmd.tokens.empty() ||
        !prompt.ok || prompt.cmd.tokens != Tokens{0, std::numeric_limits<Token>::max()} ||
        !forked.ok || forked.cmd.req != "child" || forked.cmd.src != "parent" ||
        parse_line("   ").cmd.type != CommandType::Skip ||
        parse_line(" # comment").cmd.type != CommandType::Skip)
        throw std::runtime_error("valid trace command parsed incorrectly");
}

} // namespace

int main() {
    try {
        parser_checks();
        std::cout << "PASS malformed commands and valid trace boundaries\n";
        for (ReclaimableKind mode : {ReclaimableKind::Count, ReclaimableKind::Scan}) {
            reclaimable_mode = mode;
            hand_traces();
            for (unsigned seed : {17U, 538U, 2026U, 991U}) random_traces(seed, 400);
        }
        std::cout << "PASS hand-calculated ReferenceCache traces (reclaimable=count and scan)\n";
        if (coverage.rejected == 0 || coverage.errors == 0 || coverage.hits == 0 ||
            coverage.copies == 0 || coverage.evictions == 0)
            throw std::runtime_error("incomplete differential test coverage");
        std::cout << "PASS " << coverage.operations << " differential operations; "
                  << coverage.rejected << " rejects, " << coverage.errors << " errors, "
                  << coverage.hits << " hit tokens, " << coverage.copies << " COW copies, "
                  << coverage.evictions << " evictions\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
