# cmake/Findmpv.cmake
# Finds libmpv headers and libraries and creates the mpv::mpv imported target

include(FindPackageHandleStandardArgs)

# Look in local project dependencies if present
set(_MPV_SEARCH_PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/libmpv"
    "${CMAKE_SOURCE_DIR}/.deps/libmpv"
    "/usr/local"
    "/usr"
)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    set(_ORIG_PKG_CONFIG_PATH $ENV{PKG_CONFIG_PATH})
    set(ENV{PKG_CONFIG_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/.deps/libmpv/lib/pkgconfig:${_ORIG_PKG_CONFIG_PATH}")
    pkg_check_modules(PC_MPV QUIET mpv)
    set(ENV{PKG_CONFIG_PATH} "${_ORIG_PKG_CONFIG_PATH}")
endif()

find_path(MPV_INCLUDE_DIR
    NAMES mpv/client.h
    HINTS ${PC_MPV_INCLUDEDIR}
    PATHS ${_MPV_SEARCH_PATHS}
    PATH_SUFFIXES include
)

find_library(MPV_LIBRARY
    NAMES mpv
    HINTS ${PC_MPV_LIBDIR}
    PATHS ${_MPV_SEARCH_PATHS}
    PATH_SUFFIXES lib lib64
)

set(MPV_VERSION ${PC_MPV_VERSION})

find_package_handle_standard_args(mpv
    REQUIRED_VARS MPV_LIBRARY MPV_INCLUDE_DIR
    VERSION_VAR MPV_VERSION
)

if(mpv_FOUND AND NOT TARGET mpv::mpv)
    add_library(mpv::mpv UNKNOWN IMPORTED)
    set_target_properties(mpv::mpv PROPERTIES
        IMPORTED_LOCATION "${MPV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(MPV_INCLUDE_DIR MPV_LIBRARY)
