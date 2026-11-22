#include "kvcache/config.h"

#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace kvcache {

namespace {

std::string trim(const std::string& text) {
    const char* spaces = " \t\r\n";
    auto first = text.find_first_not_of(spaces);
    if (first == std::string::npos) return "";
    auto last = text.find_last_not_of(spaces);
    return text.substr(first, last - first + 1);
}

// Accepts only plain decimal digits, so "-1", "+4", "4x" and "" are all invalid.
bool parse_unsigned(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    for (char c : text)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    try {
        out = std::stoull(text);
    } catch (const std::out_of_range&) {
        return false;
    }
    return true;
}

bool parse_size(const std::string& text, std::size_t& out) {
    std::uint64_t value;
    if (!parse_unsigned(text, value) || value == 0 ||
        value > std::numeric_limits<std::size_t>::max())
        return false;
    out = static_cast<std::size_t>(value);
    return true;
}

} // namespace

Config load_config(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open config file '" + path + "'");

    Config config;
    std::set<std::string> seen;
    std::string line;
    int line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        auto fail = [&](const std::string& message) {
            throw std::runtime_error("config line " + std::to_string(line_no) + ": " + message);
        };

        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;

        auto equals = line.find('=');
        if (equals == std::string::npos) fail("expected key=value");
        std::string key = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        if (!seen.insert(key).second) fail("repeated key '" + key + "'");

        if (key == "capacity") {
            if (!parse_size(value, config.capacity)) fail("capacity must be a positive integer");
        } else if (key == "tokens_per_block") {
            if (!parse_size(value, config.tokens_per_block))
                fail("tokens_per_block must be a positive integer");
        } else if (key == "bytes_per_token") {
            if (!parse_size(value, config.bytes_per_token))
                fail("bytes_per_token must be a positive integer");
        } else if (key == "policy") {
            if (value == "lru") config.policy = PolicyKind::LRU;
            else if (value == "fifo") config.policy = PolicyKind::FIFO;
            else fail("policy must be lru or fifo");
        } else if (key == "sharing") {
            if (value == "on") config.sharing = true;
            else if (value == "off") config.sharing = false;
            else fail("sharing must be on or off");
        } else if (key == "reclaimable") {
            if (value == "count") config.reclaimable = ReclaimableKind::Count;
            else if (value == "scan") config.reclaimable = ReclaimableKind::Scan;
            else fail("reclaimable must be scan or count");
        } else if (key == "seed") {
            if (!parse_unsigned(value, config.seed)) fail("seed must be a non-negative integer");
        } else {
            fail("unknown key '" + key + "'");
        }
    }
    return config;
}

} // namespace kvcache
