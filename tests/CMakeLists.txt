add_executable(mpvx_tests
    test_player.cpp
    test_controls_ui.cpp
)

target_link_libraries(mpvx_tests
    PRIVATE
        mpvx_core
        Slint::Slint
        Catch2::Catch2WithMain
        mpvx_warnings
)

target_compile_features(mpvx_tests PRIVATE cxx_std_26)

slint_target_sources(mpvx_tests
    ../ui/app-window.slint
)

if(UNIX AND NOT APPLE)
    set(_DEPS_LIB_DIR "${CMAKE_SOURCE_DIR}/.deps/libmpv/lib")
    get_target_property(_SLINT_LOC Slint::Slint IMPORTED_LOCATION)
    if(_SLINT_LOC)
        get_filename_component(_SLINT_DIR "${_SLINT_LOC}" DIRECTORY)
        set_target_properties(mpvx_tests PROPERTIES
            BUILD_RPATH "${_SLINT_DIR};${_DEPS_LIB_DIR}"
            INSTALL_RPATH "${_SLINT_DIR};${_DEPS_LIB_DIR}"
        )
        target_link_options(mpvx_tests PRIVATE
            "-Wl,--disable-new-dtags"
            "-Wl,-rpath,${_DEPS_LIB_DIR}"
            "-Wl,-rpath,${_SLINT_DIR}"
            "-Wl,-rpath-link,${_DEPS_LIB_DIR}"
            "-Wl,-rpath-link,${_SLINT_DIR}"
        )
    endif()
endif()

include(CTest)
if(EXISTS "${Catch2_SOURCE_DIR}/extras/Catch.cmake")
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(mpvx_tests
        PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${_DEPS_LIB_DIR}:${_SLINT_DIR}:$ENV{LD_LIBRARY_PATH}"
    )
else()
    add_test(NAME mpvx_unit_tests COMMAND mpvx_tests)
endif()
