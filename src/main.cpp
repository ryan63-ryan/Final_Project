// kvcache: trace driver for the paged KV-cache manager.
//
//   kvcache --config <cfg> --trace <trace> [--csv <out.csv>] [--no-check]
//
// Reads the trace one command per line, applies it to a KVCacheManager and
// prints one result line per command, for example:
//   ADMIT r1 -> OK blocks=[0,1] cached=0
//   APPEND r2 -> OK blocks=[0,2] cow=1
//   ADMIT r3 -> REJECT needs 3 blocks, 1 free, 0 evictable
//
// Every manager operation is timed with std::chrono::steady_clock. Only the
// manager call is inside the timed region: parsing, invariant checks and
// printing are not. The manager's invariants are checked after every operation
// (cost grows with the pool and the live requests) unless --no-check is given,
// which experiments use; they are always checked once at the end.
// With --csv, one row describing the run (configuration, counters, latency per
// operation type, total times, peak RSS) is appended to the file; the header
// is written first if the file is new or empty.

#include "kvcache/config.h"
#include "kvcache/kv_cache_manager.h"
#include "kvcache/trace.h"

#include <sys/resource.h>

#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace kvcache;

namespace {

const char* kUsage =
    "usage: kvcache --config <cfg> --trace <trace> [--csv <out.csv>] [--no-check]";

using Clock = std::chrono::steady_clock;

std::uint64_t nanoseconds(Clock::duration duration) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

struct Options {
    std::string config_path;
    std::string trace_path;
    std::string csv_path; // empty = no CSV
    bool check = true;    // check_invariants after every operation
};

Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--no-check") {
            options.check = false;
            continue;
        }
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

// Latency samples of one operation type, in nanoseconds, in constant memory.
// A long trace has millions of operations; storing every sample would add tens
// of megabytes to the peak RSS that the same run reports. Samples below 128 ns
// are counted exactly; larger ones fall into 64 buckets per power of two, so a
// reported percentile is within 1/128 (under 0.8%) of the true sample.
class LatencyHistogram {
public:
    void add(std::uint64_t ns) {
        ++buckets_[bucket_of(ns)];
        ++count_;
        sum_ += ns;
    }
    std::uint64_t count() const { return count_; }
    std::uint64_t sum() const { return sum_; }
    double mean() const { return count_ == 0 ? 0.0 : static_cast<double>(sum_) / count_; }

    // Nearest-rank percentile, 0 < fraction <= 1: the value of the bucket that
    // holds the ceil(fraction * count)-th smallest sample. 0 if there are none.
    std::uint64_t percentile(double fraction) const {
        if (count_ == 0) return 0;
        auto rank = static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count_)));
        if (rank < 1) rank = 1;
        std::uint64_t seen = 0;
        for (std::size_t index = 0; index < buckets_.size(); ++index) {
            seen += buckets_[index];
            if (seen >= rank) return value_of(index);
        }
        return value_of(buckets_.size() - 1);
    }

private:
    static constexpr std::size_t kExact = 128; // 2^7 one-nanosecond buckets
    static constexpr std::size_t kSub = 64;    // buckets per power of two above that
    static constexpr std::size_t kBuckets = kExact + (64 - 7) * kSub;

    static std::size_t bucket_of(std::uint64_t ns) {
        if (ns < kExact) return static_cast<std::size_t>(ns);
        unsigned exponent = 7; // largest e with 2^e <= ns
        while (exponent < 63 && (ns >> (exponent + 1)) != 0) ++exponent;
        auto sub = static_cast<std::size_t>((ns >> (exponent - 6)) & (kSub - 1));
        return kExact + (exponent - 7) * kSub + sub;
    }
    // Middle of the bucket's range.
    static std::uint64_t value_of(std::size_t index) {
        if (index < kExact) return index;
        unsigned exponent = 7 + static_cast<unsigned>((index - kExact) / kSub);
        std::uint64_t sub = (index - kExact) % kSub;
        std::uint64_t width = std::uint64_t{1} << (exponent - 6);
        return (kSub + sub) * width + width / 2;
    }

    std::array<std::uint64_t, kBuckets> buckets_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
};

// Order of the per-operation columns in the CSV.
enum Op { kAdmit, kAppend, kFork, kRelease, kOpCount };
const char* const kOpNames[kOpCount] = {"admit", "append", "fork", "release"};

struct RunReport {
    std::size_t bad_lines = 0;   // trace lines that could not be parsed
    std::size_t skipped_ops = 0; // commands skipped because their request was rejected
    std::array<LatencyHistogram, kOpCount> latency; // every executed operation
    std::uint64_t total_op_ns = 0;                  // sum of all timed manager calls
};

// Peak resident set size of this process in KiB, or 0 if it is not available.
std::uint64_t peak_rss_kib() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss) / 1024; // macOS reports bytes
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss); // Linux reports KiB
#endif
}

std::string base_name(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Appends one row for this run. Writes the header first if the file is empty.
void write_csv(const Options& options, const Config& config, const Stats& stats,
               const RunReport& report, std::uint64_t total_driver_ns) {
    const std::string& path = options.csv_path;
    bool empty = true;
    {
        std::ifstream existing(path);
        empty = !existing || existing.peek() == std::ifstream::traits_type::eof();
    }
    std::ofstream out(path, std::ios::app);
    if (!out) throw std::runtime_error("cannot open csv file '" + path + "'");
    if (empty) {
        out << "capacity,tokens_per_block,bytes_per_token,policy,sharing,seed,trace,checked,"
               "admitted,rejected,errors,skipped_ops,prompt_tokens,cached_tokens,"
               "uncached_tokens,cow_copies,evictions,used_blocks,peak_used_blocks,"
               "fragmentation_slots";
        for (const char* name : kOpNames)
            out << ',' << name << "_ops," << name << "_median_ns," << name << "_p99_ns,"
                << name << "_mean_ns";
        out << ",total_op_ns,total_driver_ns,peak_rss_kib,reclaimable\n";
    }
    out << config.capacity << ',' << config.tokens_per_block << ',' << config.bytes_per_token
        << ',' << (config.policy == PolicyKind::LRU ? "lru" : "fifo") << ','
        << (config.sharing ? "on" : "off") << ',' << config.seed << ','
        << base_name(options.trace_path) << ',' << (options.check ? 1 : 0) << ','
        << stats.admitted << ',' << stats.rejected << ',' << stats.errors << ','
        << report.skipped_ops << ',' << stats.prompt_tokens << ',' << stats.cached_tokens
        << ',' << stats.uncached_tokens << ',' << stats.cow_copies << ',' << stats.evictions
        << ',' << stats.used_blocks << ',' << stats.peak_used_blocks << ','
        << stats.fragmentation_slots;
    for (const LatencyHistogram& latency : report.latency)
        out << ',' << latency.count() << ',' << latency.percentile(0.5) << ','
            << latency.percentile(0.99) << ',' << latency.mean();
    out << ',' << report.total_op_ns << ',' << total_driver_ns << ',' << peak_rss_kib() << ','
        << (config.reclaimable == ReclaimableKind::Count ? "count" : "scan") << '\n';
    if (!out) throw std::runtime_error("cannot write csv file '" + path + "'");
}

RunReport run_trace(std::istream& trace, KVCacheManager& manager, bool check) {
    // Proposal rule: once a request is rejected at creation (ADMIT, or FORK as
    // the new request), every later command that names it is skipped.
    std::unordered_set<RequestId> rejected;
    RunReport report;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(trace, line)) {
        ++line_no;
        ParseResult parsed = parse_line(line);
        if (!parsed.ok) {
            // One bad line must not stop the whole trace.
            std::cerr << "line " << line_no << ": error: " << parsed.error << '\n';
            ++report.bad_lines;
            continue;
        }
        const Command& command = parsed.cmd;
        if (command.type == CommandType::Skip) continue;
        if (command.type == CommandType::Stats) {
            print_stats(std::cout, manager.stats());
            continue;
        }
        if (rejected.count(command.req) || rejected.count(command.src)) {
            // A fork of a rejected request is never created either, so the
            // commands of the fork are skipped as well.
            if (command.type == CommandType::Fork) rejected.insert(command.req);
            ++report.skipped_ops;
            continue;
        }

        // Only the manager call sits between the two clock readings.
        OpResult result;
        std::string label;
        Op op = kAdmit;
        Clock::time_point start, stop;
        switch (command.type) {
        case CommandType::Admit:
            label = "ADMIT " + command.req;
            start = Clock::now();
            result = manager.admit(command.req, command.tokens);
            stop = Clock::now();
            op = kAdmit;
            break;
        case CommandType::Append: {
            label = "APPEND " + command.req;
            Token token = command.tokens.at(0);
            start = Clock::now();
            result = manager.append(command.req, token);
            stop = Clock::now();
            op = kAppend;
            break;
        }
        case CommandType::Fork:
            label = "FORK " + command.req + " " + command.src;
            start = Clock::now();
            result = manager.fork(command.req, command.src);
            stop = Clock::now();
            op = kFork;
            break;
        case CommandType::Release:
            label = "RELEASE " + command.req;
            start = Clock::now();
            result = manager.release(command.req);
            stop = Clock::now();
            op = kRelease;
            break;
        default:
            continue;
        }
        auto elapsed = nanoseconds(stop - start);
        report.latency[op].add(elapsed);
        report.total_op_ns += elapsed;
        if (check && !manager.check_invariants())
            throw std::logic_error("manager invariants violated after line " +
                                   std::to_string(line_no));
        print_result(std::cout, label, command.type, result);
        bool creates = command.type == CommandType::Admit || command.type == CommandType::Fork;
        if (creates && result.status == Status::Reject) rejected.insert(command.req);
    }
    return report;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options = parse_arguments(argc, argv);
        Config config = load_config(options.config_path);
        std::ifstream trace(options.trace_path);
        if (!trace) throw std::runtime_error("cannot open trace file '" + options.trace_path + "'");

        // Driver time: building the manager and replaying the whole trace,
        // including parsing, printing and per-operation checks.
        auto start = Clock::now();
        KVCacheManager manager(config);
        RunReport report = run_trace(trace, manager, options.check);
        auto total_driver_ns = nanoseconds(Clock::now() - start);
        if (!manager.check_invariants()) throw std::logic_error("manager invariants violated");
        if (!options.csv_path.empty())
            write_csv(options, config, manager.stats(), report, total_driver_ns);
        return report.bad_lines == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
