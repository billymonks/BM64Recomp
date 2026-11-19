#ifndef __BM64_SIGNALLING_H__
#define __BM64_SIGNALLING_H__

// Lobby signalling. Loopback does not need an account.
// Drop a Discord application id in kApplicationId when one exists, or set
// BM64_DISCORD_APP_ID. Either one is ignored until CMake is configured with
// -DBM64_LINK_DISCORD=ON and the Social SDK is added in src/net/signalling.cpp.
namespace bm64::signalling {

inline constexpr char kApplicationId[] = "";

const char* describe();
// Loopback keeps the peer list from BM64_NET_PEER. The candidate is not sent anywhere.
void publish_candidate(const char* candidate);

} // namespace bm64::signalling

#endif
