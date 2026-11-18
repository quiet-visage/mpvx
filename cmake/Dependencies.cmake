# cmake/Dependencies.cmake

include("${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")

# 1. libmpv Dependency
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
find_package(mpv QUIET)

if(NOT mpv_FOUND)
    # Automatically run setup-deps.sh if libmpv is not yet downloaded
    message(STATUS "libmpv not found on system. Checking/running local dependency setup...")
    execute_process(
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup-deps.sh"
        RESULT_VARIABLE _SETUP_RESULT
    )
    if(NOT _SETUP_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to run setup-deps.sh to obtain libmpv")
    endif()
    find_package(mpv REQUIRED)
endif()

message(STATUS "Found libmpv: ${MPV_LIBRARY} (includes: ${MPV_INCLUDE_DIR})")

# Look for input device libraries (required by Slint on Linux)
set(MPVX_EXTRA_DEPS "")
foreach(_dep input mtdev evdev wacom)
    find_library(LIB_${_dep} NAMES ${_dep} HINTS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/libmpv/lib")
    if(LIB_${_dep} AND NOT TARGET mpvx::${_dep})
        add_library(mpvx::${_dep} UNKNOWN IMPORTED)
        set_target_properties(mpvx::${_dep} PROPERTIES
            IMPORTED_LOCATION "${LIB_${_dep}}"
        )
        list(APPEND MPVX_EXTRA_DEPS mpvx::${_dep})
    endif()
endforeach()

# 2. Slint GUI Toolkit
# First check if Slint is installed locally on the system
find_package(Slint QUIET)

if(NOT Slint_FOUND)
    message(STATUS "Slint not found locally on system. Using CPM to fetch Slint C++ SDK...")

    set(SLINT_VERSION "1.19.0")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(SLINT_URL "https://github.com/slint-ui/slint/releases/download/v${SLINT_VERSION}/Slint-cpp-${SLINT_VERSION}-Linux-x86_64.tar.gz")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(SLINT_URL "https://github.com/slint-ui/slint/releases/download/v${SLINT_VERSION}/Slint-cpp-${SLINT_VERSION}-Linux-arm64.tar.gz")
    else()
        message(FATAL_ERROR "Unsupported platform for prebuilt Slint binary. Please install Slint or Rust toolchain.")
    endif()

    set(_PREV_SLINT "${CMAKE_SOURCE_DIR}/build/dev-debug/_deps/slintprebuilt-src")
    if(EXISTS "${_PREV_SLINT}/lib/cmake/Slint/SlintConfig.cmake")
        set(SlintPrebuilt_SOURCE_DIR "${_PREV_SLINT}")
    else()
        CPMAddPackage(
            NAME SlintPrebuilt
            URL "${SLINT_URL}"
            DOWNLOAD_ONLY YES
        )
    endif()

    list(PREPEND CMAKE_PREFIX_PATH "${SlintPrebuilt_SOURCE_DIR}")
    set(Slint_DIR "${SlintPrebuilt_SOURCE_DIR}/lib/cmake/Slint")
    find_package(Slint REQUIRED CONFIG)
endif()

message(STATUS "Found Slint: ${Slint_DIR}")

# 3. Test dependencies (Catch2)
if(MPVX_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.7.1
        SYSTEM YES
    )
endif()
