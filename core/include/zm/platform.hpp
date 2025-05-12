#pragma once

#if defined(_WIN32)
#define ZM_PLUGIN_EXT ".dll"
#elif defined(__APPLE__)
#define ZM_PLUGIN_EXT ".dylib"
#else
#define ZM_PLUGIN_EXT ".so"
#endif
