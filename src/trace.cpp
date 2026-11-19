// trace.cpp - parses one trace line into a Command.
// Rule: never throw on bad input; return ok=false with a message instead.
#include "kvcache/trace.h"

#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace kvcache {

namespace {

// "42" -> 42. Rejects "", "4x", "-1", and values that do not fit in Token.
bool parse_token(const std::string& s, Token& out) {
    if (s.empty()) return false;                                   // TODO 1
    for (char c : s) {                                             // TODO 2
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    long long v = 0;
    try {                                                          // TODO 3
        v = std::stoll(s);
    } catch (const std::out_of_range&) {
        return false;
    }
    if (v > std::numeric_limits<Token>::max()) return false;       // TODO 4
    out = static_cast<Token>(v);
    return true;
}

ParseResult fail(const std::string& msg) {
    ParseResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

} // namespace

ParseResult parse_line(const std::string& line) {
    ParseResult r;

    // Split into words: "ADMIT r1 10 11" -> {"ADMIT","r1","10","11"}
    std::istringstream in(line);
    std::vector<std::string> w;
    for (std::string x; in >> x;) w.push_back(x);

    // TODO 5: blank line or comment
    if (w.empty() || w[0][0] == '#') {
        r.cmd.type = CommandType::Skip;
        return r;
    }

    const std::string& op = w[0];

    if (op == "ADMIT") {                                           // TODO 6-7
        if (w.size() < 2) return fail("ADMIT needs a request ID");
        r.cmd.type = CommandType::Admit;
        r.cmd.req = w[1];
        for (std::size_t i = 2; i < w.size(); ++i) {               // empty prompt allowed
            Token t = 0;
            if (!parse_token(w[i], t)) return fail("bad token '" + w[i] + "'");
            r.cmd.tokens.push_back(t);
        }
    } else if (op == "APPEND") {                                   // TODO 8
        if (w.size() != 3) return fail("APPEND needs: APPEND <req> <token>");
        Token t = 0;
        if (!parse_token(w[2], t)) return fail("bad token '" + w[2] + "'");
        r.cmd.type = CommandType::Append;
        r.cmd.req = w[1];
        r.cmd.tokens.push_back(t);
    } else if (op == "FORK") {                                     // TODO 9
        if (w.size() != 3) return fail("FORK needs: FORK <new_req> <src_req>");
        if (w[1] == w[2]) return fail("FORK onto itself '" + w[1] + "'");
        r.cmd.type = CommandType::Fork;
        r.cmd.req = w[1];
        r.cmd.src = w[2];
    } else if (op == "RELEASE") {                                  // TODO 10
        if (w.size() != 2) return fail("RELEASE needs: RELEASE <req>");
        r.cmd.type = CommandType::Release;
        r.cmd.req = w[1];
    } else if (op == "STATS") {                                    // TODO 11
        if (w.size() != 1) return fail("STATS takes no arguments");
        r.cmd.type = CommandType::Stats;
    } else {                                                       // TODO 12
        return fail("unknown command '" + op + "'");
    }
    return r;
}

} // namespace kvcache
