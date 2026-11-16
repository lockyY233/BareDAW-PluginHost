cmake_minimum_required(VERSION 3.16)
project(BareDAWPluginHost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BAREDAW_PLUGIN_HOST_BUILD_TESTS "Build BareDAW PluginHost tests" OFF)
option(BAREDAW_ENABLE_CLAP "Enable CLAP plugin hosting" ON)
option(BAREDAW_ENABLE_VST3 "Enable VST3 plugin hosting" OFF)
set(BAREDAW_VST3_SDK_DIR "" CACHE PATH
    "Existing VST3 SDK checkout; empty fetches the pinned official SDK")

if (EMSCRIPTEN)
    message(FATAL_ERROR "BareDAW PluginHost is native-only")
endif()

set(_baredaw_submodule_core "${CMAKE_CURRENT_SOURCE_DIR}/external/BareDAW")
set(BAREDAW_CORE_SOURCE_DIR "${_baredaw_submodule_core}" CACHE PATH
    "BareDAW source directory")
get_filename_component(BAREDAW_CORE_SOURCE_DIR "${BAREDAW_CORE_SOURCE_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if (NOT EXISTS "${BAREDAW_CORE_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "BareDAW was not found at ${BAREDAW_CORE_SOURCE_DIR}. "
        "Run 'git submodule update --init --recursive' or set BAREDAW_CORE_SOURCE_DIR.")
endif()

if (NOT TARGET BareDAW::Core)
    set(BAREDAW_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BAREDAW_CORE_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/baredaw")
endif()

if (BAREDAW_ENABLE_VST3)
    include(FetchContent)
    if (APPLE)
        enable_language(OBJCXX)
        if (NOT DEFINED ENV{XCODE_VERSION})
            execute_process(COMMAND xcodebuild -version
                RESULT_VARIABLE _baredaw_xcode_result
                OUTPUT_QUIET ERROR_QUIET)
            if (NOT _baredaw_xcode_result EQUAL 0)
                set(ENV{XCODE_VERSION} "15.0")
                set(XCODE_VERSION "15.0" CACHE STRING
                    "VST3 SDK compatibility value for Apple Command Line Tools" FORCE)
            endif()
        endif()
    endif()
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
    set(SMTG_VSTSDK_GENERATE_MACOS_IOS_COLLECTION_TARGETS OFF CACHE BOOL "" FORCE)
    if (BAREDAW_VST3_SDK_DIR)
        set(BAREDAW_VST3_SDK_ROOT "${BAREDAW_VST3_SDK_DIR}")
        add_subdirectory("${BAREDAW_VST3_SDK_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/vst3sdk-build" EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(vst3sdk
            GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
            GIT_TAG v3.8.1_build_84
            GIT_SHALLOW TRUE
            GIT_SUBMODULES base cmake pluginterfaces public.sdk)
        FetchContent_MakeAvailable(vst3sdk)
        set(BAREDAW_VST3_SDK_ROOT "${vst3sdk_SOURCE_DIR}")
    endif()
endif()

add_library(baredaw_plugin_host STATIC
    src/bridge/plugin-host/PluginManager.cpp
    $<$<BOOL:${BAREDAW_ENABLE_CLAP}>:src/bridge/plugin-host/clap/ClapAdapter.cpp>
    $<$<BOOL:${BAREDAW_ENABLE_VST3}>:src/bridge/plugin-host/vst3/Vst3Adapter.cpp>
    $<$<PLATFORM_ID:Windows>:src/bridge/plugin-host/loader/Win32LoadLibrary.cpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/bridge/plugin-host/loader/PosixDlopen.cpp>)
add_library(BareDAW::PluginHost ALIAS baredaw_plugin_host)
target_include_directories(baredaw_plugin_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(baredaw_plugin_host PUBLIC BareDAW::Core)
target_compile_definitions(baredaw_plugin_host PUBLIC
    BAREDAW_PLUGIN_ABI_VERSION_MAJOR=1
    BAREDAW_PLUGIN_ABI_VERSION_MINOR=0
    BAREDAW_ENABLE_CLAP=$<BOOL:${BAREDAW_ENABLE_CLAP}>
    BAREDAW_ENABLE_VST3=$<BOOL:${BAREDAW_ENABLE_VST3}>)
if (BAREDAW_ENABLE_VST3)
    target_sources(baredaw_plugin_host PRIVATE
        "${BAREDAW_VST3_SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp"
        $<$<PLATFORM_ID:Darwin>:${BAREDAW_VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm>
        $<$<PLATFORM_ID:Windows>:${BAREDAW_VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp>
        $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<NOT:$<PLATFORM_ID:Windows>>>:${BAREDAW_VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp>)
    target_link_libraries(baredaw_plugin_host PRIVATE sdk_hosting)
    if (APPLE)
        set_source_files_properties(
            "${BAREDAW_VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm"
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(baredaw_plugin_host PUBLIC "-framework Foundation")
    endif()
endif()
target_compile_options(baredaw_plugin_host PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-rtti>)

add_executable(baredaw_plugin_scan_probe
    src/bridge/plugin-host/PluginScanProbe.cpp)
target_link_libraries(baredaw_plugin_scan_probe PRIVATE
    BareDAW::Core BareDAW::PluginHost)
target_compile_options(baredaw_plugin_scan_probe PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-rtti>)
set_target_properties(baredaw_plugin_scan_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if (BAREDAW_PLUGIN_HOST_BUILD_TESTS)
    enable_testing()
    add_test(NAME baredaw_plugin_host_isolation
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_host_isolation.sh")
    if (BAREDAW_ENABLE_CLAP)
        set(BAREDAW_PLUGIN_FIXTURES_DIR "${CMAKE_BINARY_DIR}/plugin-fixtures")
        add_library(baredaw_fake_clap_fixture SHARED
            tests/fixtures/fake_clap_plugin.cpp)
        target_include_directories(baredaw_fake_clap_fixture PRIVATE third_party/clap)
        target_compile_options(baredaw_fake_clap_fixture PRIVATE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-rtti>)
        set_target_properties(baredaw_fake_clap_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${BAREDAW_PLUGIN_FIXTURES_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${BAREDAW_PLUGIN_FIXTURES_DIR}"
            NO_SONAME ON)
        add_executable(baredaw_plugin_host_smoke tests/native/plugin_host_smoke.cpp)
        target_link_libraries(baredaw_plugin_host_smoke PRIVATE
            BareDAW::Core BareDAW::PluginHost)
        target_compile_options(baredaw_plugin_host_smoke PRIVATE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG;-fno-rtti>)
        add_dependencies(baredaw_plugin_host_smoke baredaw_fake_clap_fixture)
        add_test(NAME baredaw_plugin_host_smoke
            COMMAND baredaw_plugin_host_smoke "${BAREDAW_PLUGIN_FIXTURES_DIR}")
    endif()

    find_program(BAREDAW_TEST_FAUST_EXECUTABLE faust)
    if (BAREDAW_TEST_FAUST_EXECUTABLE)
        set(BAREDAW_FAUST_PLUGIN_FIXTURES
            "${CMAKE_BINARY_DIR}/faust-plugin-fixtures")
        set(_baredaw_post_build_outputs "")
        foreach(_name IN ITEMS gain invert softclip onepole dcblock)
            set(_output
                "${BAREDAW_FAUST_PLUGIN_FIXTURES}/post_${_name}${CMAKE_SHARED_LIBRARY_SUFFIX}")
            add_custom_command(OUTPUT "${_output}"
                COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-faust-device.sh"
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/faust/post-build/${_name}.dsp"
                    "post.${_name}" "${_output}" "${CMAKE_CXX_COMPILER}"
                DEPENDS
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/faust/post-build/${_name}.dsp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-faust-device.sh"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/faust/PostBuildPluginTemplate.cpp.in"
                VERBATIM)
            list(APPEND _baredaw_post_build_outputs "${_output}")
        endforeach()
        add_custom_target(baredaw_faust_plugin_fixtures
            DEPENDS ${_baredaw_post_build_outputs})
        add_executable(baredaw_faust_post_build_smoke
            tests/native/faust_post_build_smoke.cpp)
        target_link_libraries(baredaw_faust_post_build_smoke PRIVATE
            BareDAW::Core BareDAW::PluginHost)
        target_compile_options(baredaw_faust_post_build_smoke PRIVATE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG;-fno-rtti>)
        add_dependencies(baredaw_faust_post_build_smoke
            baredaw_faust_plugin_fixtures)
        add_test(NAME baredaw_faust_post_build_smoke
            COMMAND baredaw_faust_post_build_smoke
                "${BAREDAW_FAUST_PLUGIN_FIXTURES}")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS baredaw_plugin_host EXPORT BareDAWPluginHostTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY src/bridge/plugin-host DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/bridge"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

