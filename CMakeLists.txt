cmake_minimum_required(VERSION 3.25...4.3)
project(mpvx
    VERSION 0.1.0
    DESCRIPTION "Modern C++26 and Slint frontend for libmpv"
    LANGUAGES CXX
)

# Standard C++26 setup
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers (clangd)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Configure runtime library path (RPATH) to resolve dependencies automatically
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Options
option(MPVX_BUILD_TESTS "Build mpvx unit tests" ON)
option(MPVX_ENABLE_WARNINGS "Enable strict compiler warnings" ON)

# Project dependencies managed via CPM and Find modules
include(cmake/Dependencies.cmake)

# Warning flags definition (applied to our own code)
add_library(mpvx_warnings INTERFACE)
if(MPVX_ENABLE_WARNINGS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mpvx_warnings INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wnon-virtual-dtor
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wnull-dereference
            -Wformat=2
        )
    endif()
endif()

# ==============================================================================
# mpvx_core Library (Playback and Media Management)
# ==============================================================================
add_library(mpvx_core
    src/player.cpp
    src/renderer.cpp
)

target_include_directories(mpvx_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mpvx_core
    PUBLIC
        mpv::mpv
        Slint::Slint
        mpvx_warnings
        ${MPVX_EXTRA_DEPS}
)

target_compile_features(mpvx_core PUBLIC cxx_std_26)

# ==============================================================================
# mpvx Application Executable
# ==============================================================================
add_executable(mpvx
    src/main.cpp
)

target_link_libraries(mpvx
    PRIVATE
        mpvx_core
        Slint::Slint
        mpvx_warnings
)

target_compile_features(mpvx PRIVATE cxx_std_26)

# Compile Slint UI templates and link into executable
slint_target_sources(mpvx
    ui/app-window.slint
)

# Linker RPATH & rpath-link configuration for Linux
if(UNIX AND NOT APPLE)
    set(_DEPS_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/libmpv/lib")
    get_target_property(_SLINT_LOC Slint::Slint IMPORTED_LOCATION)
    if(_SLINT_LOC)
        get_filename_component(_SLINT_DIR "${_SLINT_LOC}" DIRECTORY)
        set_target_properties(mpvx PROPERTIES
            BUILD_RPATH "${_SLINT_DIR};${_DEPS_LIB_DIR}"
            INSTALL_RPATH "${_SLINT_DIR};${_DEPS_LIB_DIR}"
        )
        target_link_options(mpvx PRIVATE
            "-Wl,--disable-new-dtags"
            "-Wl,-rpath,${_DEPS_LIB_DIR}"
            "-Wl,-rpath,${_SLINT_DIR}"
            "-Wl,-rpath-link,${_DEPS_LIB_DIR}"
            "-Wl,-rpath-link,${_SLINT_DIR}"
        )
    endif()
endif()

# ==============================================================================
# Testing
# ==============================================================================
if(MPVX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ==============================================================================
# Formatting & Tooling
# ==============================================================================
find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ALL_SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i -style=file ${ALL_SOURCE_FILES}
        COMMENT "Formatting all C++ source files with clang-format"
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run -Werror -style=file ${ALL_SOURCE_FILES}
        COMMENT "Checking C++ source file formatting with clang-format"
    )
endif()
