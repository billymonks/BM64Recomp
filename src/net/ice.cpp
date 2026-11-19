#include "bm64_ice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef BM64_HAS_LIBJUICE
#define BM64_HAS_LIBJUICE 0
#endif

#if BM64_HAS_LIBJUICE
#error "BM64_HAS_LIBJUICE=1 but libjuice is not in the tree. Add the library, then implement TURN gathering in src/net/ice.cpp (gather_turn)."
#endif

namespace bm64::ice {
namespace {
char candidate_[96] = "host 127.0.0.1:0";
}

const char* host_candidate(const char* bind_endpoint) {
    const char* bind = (bind_endpoint != nullptr && bind_endpoint[0] != '\0') ? bind_endpoint : "127.0.0.1:0";
    std::snprintf(candidate_, sizeof(candidate_), "host %s", bind);
    return candidate_;
}

const char* describe() {
    const char* turn = std::getenv("BM64_TURN_HOST");
    if (turn != nullptr && turn[0] != '\0') {
        return "direct UDP host candidate; BM64_TURN_HOST is set but libjuice is not built (BM64_HAS_LIBJUICE=0)";
    }
    return "direct UDP, host candidates only";
}

} // namespace bm64::ice
