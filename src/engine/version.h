// vgraph engine: version constants.
// Pure C++17. No Vertica includes in src/engine.
#ifndef VGRAPH_ENGINE_VERSION_H
#define VGRAPH_ENGINE_VERSION_H

#include <cstdint>

namespace vgraph {

// Library version. Change it here only.
constexpr const char *LIBRARY_VERSION = "0.1.0";

// Snapshot binary format version. See docs/format.md.
constexpr std::int32_t FORMAT_VERSION = 1;

// Build flags are passed by the Makefile. The fallback keeps other builds working.
#ifndef VGRAPH_BUILD_FLAGS
#define VGRAPH_BUILD_FLAGS "unknown"
#endif
constexpr const char *BUILD_FLAGS = VGRAPH_BUILD_FLAGS;

} // namespace vgraph

#endif
