#pragma once

#include "kvcache/types.h"

#include <string>
#include <vector>

namespace kvcache {

// Trace format: one command per line. Blank lines and lines starting with '#'
// are skipped.
//
//   ADMIT   <req> [<tok> ...]      e.g.  ADMIT r1 10 11 12
//   APPEND  <req> <tok>            e.g.  APPEND r2 13
//   FORK    <new_req> <src_req>    e.g.  FORK r2 r1
//   RELEASE <req>                  e.g.  RELEASE r1
//   STATS

enum class CommandType { Admit, Append, Fork, Release, Stats, Skip };

struct Command {
    CommandType type = CommandType::Skip;
    RequestId req;             // ADMIT/APPEND/RELEASE target; FORK: the new request
    RequestId src;             // FORK only: source request
    std::vector<Token> tokens; // ADMIT: prompt; APPEND: exactly one token
};

struct ParseResult {
    bool ok = true;
    Command cmd;
    std::string error; // filled when ok == false
};

// Parses one line. Never throws: bad input returns ok=false with a message.
ParseResult parse_line(const std::string& line);

} // namespace kvcache
