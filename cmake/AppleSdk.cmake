# AppleSdk.cmake — one macOS SDK for the whole build, and survive SDK changes.
#
# Include before project(). On macOS the compiler (/usr/bin/c++, an xcrun shim),
# Homebrew's pkg-config and CMake's find modules can each pick a different SDK:
# pkg-config's built-in libcurl.pc points at the Command Line Tools SDK while the
# compiler uses Xcode's. When Command Line Tools updated to 27.0 its .tbd stubs
# became unreadable to Xcode 26.1's linker and three plugins stopped linking.
# CMake never noticed the SDK had changed, so the stale paths stayed in the cache.
#
# This module:
#   1. pins CMAKE_OSX_SYSROOT to `xcrun --show-sdk-path` (whatever DEVELOPER_DIR /
#      xcode-select selects) unless the user set it explicitly;
#   2. when that SDK differs from the one this build was configured with, clears
#      cached paths that point into any other SDK so find modules look again;
#   3. makes the build re-run CMake when the SDK itself changes (a Command Line
#      Tools or Xcode update replaces SDKSettings.json);
#   4. provides zm_link_system_curl(<target>), which on macOS links curl as
#      -lcurl from the pinned SDK instead of the pkg-config/FindCURL path.

if(NOT CMAKE_HOST_APPLE)  # APPLE is only defined after project()
    function(zm_link_system_curl target)
        find_package(CURL REQUIRED)
        target_link_libraries(${target} PRIVATE CURL::libcurl)
    endfunction()
    return()
endif()

execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE _zm_xcrun_sdk
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _zm_xcrun_rc)
if(NOT _zm_xcrun_rc EQUAL 0 OR NOT IS_DIRECTORY "${_zm_xcrun_sdk}")
    message(FATAL_ERROR "xcrun --show-sdk-path failed; install Xcode or the Command Line Tools")
endif()
# Resolve symlinks (Command Line Tools' MacOSX.sdk -> MacOSX27.0.sdk) so an update
# that retargets the link counts as a different SDK.
file(REAL_PATH "${_zm_xcrun_sdk}" _zm_sdk_real)

# Respect an explicit -DCMAKE_OSX_SYSROOT; otherwise follow xcrun every configure.
if(NOT ZM_OSX_SYSROOT_USER_SET)
    if(CMAKE_OSX_SYSROOT AND NOT CMAKE_OSX_SYSROOT STREQUAL "${ZM_OSX_SYSROOT_AUTO}")
        set(ZM_OSX_SYSROOT_USER_SET ON CACHE INTERNAL "CMAKE_OSX_SYSROOT was set by the user")
    else()
        set(CMAKE_OSX_SYSROOT "${_zm_xcrun_sdk}" CACHE PATH "macOS SDK (from xcrun)" FORCE)
        set(ZM_OSX_SYSROOT_AUTO "${_zm_xcrun_sdk}" CACHE INTERNAL "sysroot chosen by AppleSdk.cmake")
    endif()
endif()
file(REAL_PATH "${CMAKE_OSX_SYSROOT}" _zm_sysroot_real)

if(DEFINED ZM_CONFIGURED_SDK AND NOT ZM_CONFIGURED_SDK STREQUAL "${_zm_sysroot_real}")
    message(STATUS "macOS SDK changed: ${ZM_CONFIGURED_SDK} -> ${_zm_sysroot_real}; clearing cached SDK paths")
    get_cmake_property(_zm_cache_vars CACHE_VARIABLES)
    foreach(_v IN LISTS _zm_cache_vars)
        get_property(_type CACHE ${_v} PROPERTY TYPE)
        if(_type STREQUAL "INTERNAL" OR _type STREQUAL "STATIC")
            # pkg-config's per-module results (PC_*_PREFIX etc.) are INTERNAL.
            if(NOT _v MATCHES "^PC_|^pkgcfg_lib_|^__pkg_config")
                continue()
            endif()
        endif()
        if("${${_v}}" MATCHES "/SDKs/MacOSX[0-9.]*\\.sdk" AND NOT "${${_v}}" MATCHES "^${_zm_sysroot_real}|^${CMAKE_OSX_SYSROOT}")
            unset(${_v} CACHE)
        endif()
    endforeach()
    unset(_zm_cache_vars)
endif()
set(ZM_CONFIGURED_SDK "${_zm_sysroot_real}" CACHE INTERNAL "real path of the SDK this build was configured with")

# Re-run CMake if the SDK is updated in place, or `xcode-select -s` switches the
# developer dir (it rewrites /var/db/xcode_select_link). A DEVELOPER_DIR change in
# the environment can't be watched: re-run cmake (./build.sh does) after changing it.
foreach(_zm_dep "${_zm_sysroot_real}/SDKSettings.json" "/var/db/xcode_select_link")
    if(EXISTS "${_zm_dep}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_zm_dep}")
    endif()
endforeach()

message(STATUS "macOS SDK: ${CMAKE_OSX_SYSROOT} (${_zm_sysroot_real})")

# System curl from the pinned SDK. -lcurl is resolved by the linker against the
# sysroot CMake passes (-isysroot / -syslibroot), so there is no absolute path to
# go stale and no second SDK in the link.
function(zm_link_system_curl target)
    target_link_libraries(${target} PRIVATE curl)
endfunction()
