#pragma once
// Cross-platform dynamic-loader shim: POSIX dlopen/dlsym/dlclose/dlerror vs the
// Win32 LoadLibrary/GetProcAddress/FreeLibrary family. Handles are void* on both
// (HMODULE is a pointer). ZM_PLUGIN_EXT is the platform plugin file extension.

#include <string>

#if defined(_WIN32)
#define ZM_PLUGIN_EXT ".dll"
#elif defined(__APPLE__)
#define ZM_PLUGIN_EXT ".dylib"
#else
#define ZM_PLUGIN_EXT ".so"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
inline void* zm_dlopen(const char* path) {
    return reinterpret_cast<void*>(::LoadLibraryA(path));
}
inline void* zm_dlsym(void* h, const char* sym) {
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(h), sym));
}
inline void zm_dlclose(void* h) { ::FreeLibrary(reinterpret_cast<HMODULE>(h)); }
inline std::string zm_dlerror() {
    DWORD e = ::GetLastError();
    if (!e) return "(no error)";
    char* msg = nullptr;
    ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, e, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string s = msg ? msg : "(unknown error)";
    if (msg) ::LocalFree(msg);
    return s;
}
#else
#include <dlfcn.h>
inline void* zm_dlopen(const char* path) { return ::dlopen(path, RTLD_NOW); }
inline void* zm_dlsym(void* h, const char* sym) { return ::dlsym(h, sym); }
inline void zm_dlclose(void* h) { ::dlclose(h); }
inline std::string zm_dlerror() { const char* e = ::dlerror(); return e ? e : "(no error)"; }
#endif
