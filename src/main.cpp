// kvcache: trace driver for the paged KV-cache manager.
//
//   kvcache --config <cfg> --trace <trace> [--csv <out.csv>]
//
// Reads the trace one command per line, applies it to a KVCacheManager and
// prints one result line per command, for example:
//   ADMIT r1 -> OK blocks=[0,1] cached=0
//   APPEND r2 -> OK blocks=[0,2] cow=1
//   ADMIT r3 -> REJECT needs 3 blocks, 1 free, 0 evictable
// With --csv, the final statistics are also written as one CSV row.

#include "kvcache/config.h"
#include "kvcache/kv_cache_manager.h"
#include "kvcache/trace.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace kvcache;

namespace {

const char* kUsage = "usage: kvcache --config <cfg> --trace <trace> [--csv <out.csv>]";

struct Options {
    std::string config_path;
    std::string trace_path;
    std::string csv_path; // empty = no CSV
};

Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        std::string* target = flag == "--config" ? &options.config_path
                            : flag == "--trace"  ? &options.trace_path
                            : flag == "--csv"    ? &options.csv_path
                            : nullptr;
        if (!target || i + 1 == argc) throw std::invalid_argument(kUsage);
        *target = argv[++i];
    }
    if (options.config_path.empty() || options.trace_path.empty())
        throw std::invalid_argument(kUsage);
    return options;
}

void print_list(std::ostream& out, const char* name, const std::vector<BlockId>& ids) {
    out << ' ' << name << "=[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) out << ',';
        out << ids[i];
    }
    out << ']';
}

// Prints "<label> -> OK ..." / "REJECT <reason>" / "ERROR <reason>".
void print_result(std::ostream& out, const std::string& label, CommandType type,
                  const OpResult& result) {
    out << label << " -> ";
    if (result.status != Status::Ok) {
        out << (result.status == Status::Reject ? "REJECT " : "ERROR ") << result.message << '\n';
        return;
    }
    out << "OK";
    if (type == CommandType::Release) print_list(out, "freed", result.freed);
    else print_list(out, "blocks", result.blocks);
    if (type == CommandType::Admit) out << " cached=" << result.cached_tokens;
    if (type == CommandType::Append) out << " cow=" << result.cow_copies;
    if (!result.evicted.empty()) print_list(out, "evicted", result.evicted);
    out << '\n';
}

void print_stats(std::ostream& out, const Stats& stats) {
    out << "STATS admitted=" << stats.admitted << " rejected=" << stats.rejected
        << " errors=" << stats.errors << " prompt_tokens=" << stats.prompt_tokens
        << " cached_tokens=" << stats.cached_tokens
        << " uncached_tokens=" << stats.uncached_tokens << " cow=" << stats.cow_copies
        << " evictions=" << stats.evictions << " used=" << stats.used_blocks
        << " peak=" << stats.peak_used_blocks
        << " fragmentation=" << stats.fragmentation_slots << '\n';
}

void write_csv(const std::string& path, const Config& config, const Stats& stats) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot open csv file '" + path + "'");
    out << "capacity,tokens_per_block,bytes_per_token,policy,sharing,seed,"
           "admitted,rejected,errors,prompt_tokens,cached_tokens,uncached_tokens,"
           "cow_copies,evictions,used_blocks,peak_used_blocks,fragmentation_slots\n"
        << config.capacity << ',' << config.tokens_per_block << ',' << config.bytes_per_token
        << ',' << (config.policy == PolicyKind::LRU ? "lru" : "fifo") << ','
        << (config.sharing ? "on" : "off") << ',' << config.seed << ',' << stats.admitted
        << ',' << stats.rejected << ',' << stats.errors << ',' << stats.prompt_tokens << ','
        << stats.cached_tokens << ',' << stats.uncached_tokens << ',' << stats.cow_copies
        << ',' << stats.evictions << ',' << stats.used_blocks << ','
        << stats.peak_used_blocks << ',' << stats.fragmentation_slots << '\n';
    if (!out) throw std::runtime_error("cannot write csv file '" + path + "'");
}

// Returns the number of trace lines that could not be parsed.
std::size_t run_trace(std::istream& trace, KVCacheManager& manager) {
    // Proposal rule: once a request is rejected at creation (ADMIT, or FORK as
    // the new request), every later command that names it is skipped.
    std::unordered_set<RequestId> rejected;
    std::size_t bad_lines = 0;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(trace, line)) {
        ++line_no;
        ParseResult parsed = parse_line(line);
        if (!parsed.ok) {
            // One bad line must not stop the whole trace.
            std::cerr << "line " << line_no << ": error: " << parsed.error << '\n';
            ++bad_lines;
            continue;
        }
        const Command& command = parsed.cmd;
        if (command.type == CommandType::Skip) continue;
        if (command.type == CommandType::Stats) {
            print_stats(std::cout, manager.stats());
            continue;
        }
        if (rejected.count(command.req) || rejected.count(command.src)) continue;

        OpResult result;
        std::string label;
        switch (command.type) {
        case CommandType::Admit:
            label = "ADMIT " + command.req;
            result = manager.admit(command.req, command.tokens);
            break;
        case CommandType::Append:
            label = "APPEND " + command.req;
            result = manager.append(command.req, command.tokens.at(0));
            break;
        case CommandType::Fork:
            label = "FORK " + command.req + " " + command.src;
            result = manager.fork(command.req, command.src);
            break;
        case CommandType::Release:
            label = "RELEASE " + command.req;
            result = manager.release(command.req);
            break;
        default:
            continue;
        }
        print_result(std::cout, label, command.type, result);
        bool creates = command.type == CommandType::Admit || command.type == CommandType::Fork;
        if (creates && result.status == Status::Reject) rejected.insert(command.req);
    }
    return bad_lines;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options = parse_arguments(argc, argv);
        Config config = load_config(options.config_path);
        std::ifstream trace(options.trace_path);
        if (!trace) throw std::runtime_error("cannot open trace file '" + options.trace_path + "'");

        KVCacheManager manager(config);
        std::size_t bad_lines = run_trace(trace, manager);
        if (!manager.check_invariants()) throw std::logic_error("manager invariants violated");
        if (!options.csv_path.empty()) write_csv(options.csv_path, config, manager.stats());
        return bad_lines == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
