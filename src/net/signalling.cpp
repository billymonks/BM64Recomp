#include "bm64_signalling.h"

#include <cstdlib>

#ifndef BM64_LINK_DISCORD
#define BM64_LINK_DISCORD 0
#endif

#if BM64_LINK_DISCORD
#error "BM64_LINK_DISCORD=1 but the Discord Social SDK is not in the tree. Add it, then open the lobby in src/net/signalling.cpp. The application id is kApplicationId or BM64_DISCORD_APP_ID."
#endif

namespace bm64::signalling {
namespace {
const char* published_ = "";
}

void publish_candidate(const char* candidate) {
    published_ = candidate != nullptr ? candidate : "";
    (void)published_;
}

const char* describe() {
    const char* app = std::getenv("BM64_DISCORD_APP_ID");
    bool from_env = app != nullptr && app[0] != '\0';
    bool from_source = kApplicationId[0] != '\0';
    if (from_env || from_source) {
        return "Discord application id is set, but the Social SDK is not linked (BM64_LINK_DISCORD=0); the lobby stays on loopback";
    }
    return "loopback signalling";
}

} // namespace bm64::signalling
