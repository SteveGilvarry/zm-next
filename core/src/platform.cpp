#include "zm/platform.hpp"

#include <filesystem>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <cstdint>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace zm {

namespace {

namespace fs = std::filesystem;

// Directory holding the running executable, or empty if it can't be determined.
fs::path executable_dir() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        fs::path canon = fs::canonical(fs::path(buf), ec);
        return ec ? fs::path(buf).parent_path() : canon.parent_path();
    }
#elif defined(_WIN32)
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return fs::path(std::string(buf, n)).parent_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return {};
}

}  // namespace

const std::string& plugins_dir() {
    static const std::string dir = [] {
        const fs::path exe = executable_dir();
        std::error_code ec;
        if (!exe.empty() && fs::is_directory(exe / "plugins", ec)) return (exe / "plugins").string();
        return std::string("plugins");
    }();
    return dir;
}

}  // namespace zm
