#ifndef __BM64_ICE_H__
#define __BM64_ICE_H__

// NAT traversal beside the direct UDP transport.
// BM64_HAS_LIBJUICE is off unless CMake is configured with -DBM64_HAS_LIBJUICE=ON,
// and that configuration does not link a library yet: src/net/ice.cpp fails the build
// so a TURN client is not silently pretended. Without it, BM64_TURN_HOST is recorded
// and the session publishes a host candidate (the bound UDP endpoint).
namespace bm64::ice {

const char* describe();
// Remembers the bound endpoint as the host candidate the lobby would publish.
const char* host_candidate(const char* bind_endpoint);

} // namespace bm64::ice

#endif
