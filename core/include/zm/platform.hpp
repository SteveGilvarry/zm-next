#pragma once

#include <string>

#if defined(_WIN32)
#define ZM_PLUGIN_EXT ".dll"
#elif defined(__APPLE__)
#define ZM_PLUGIN_EXT ".dylib"
#else
#define ZM_PLUGIN_EXT ".so"
#endif

namespace zm {

// Directory plugins are loaded from: "<dir of the running executable>/plugins"
// when that directory exists (zm-core at build/zm-core, plugins at
// build/plugins/...), so a worker spawned from any working directory finds
// them; otherwise "plugins", relative to the working directory as before.
// Resolved once per process.
const std::string& plugins_dir();

}  // namespace zm
