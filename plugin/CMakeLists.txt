cmake_minimum_required(VERSION 3.21)
project(gasfree_plugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32 AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

option(GASFREE_BUILD_VST3 "Also build VST3 via clap-wrapper (requires VST3 SDK or CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON)" OFF)
option(GASFREE_BUILD_AUV2 "Also build AUv2 on macOS via clap-wrapper" OFF)
option(GASFREE_INSTALL_LOCAL "Copy built .clap plugin to local user CLAP directory after build" ON)
option(GASFREE_BUILD_TESTS "Build DSP and CLAP host verification tests" ON)

# Path to the gasfree web app directory (defaults to ../app inside the fork, or ../gasfree/app standalone)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../app/index.html")
    set(_GASFREE_DEFAULT_APP "${CMAKE_CURRENT_SOURCE_DIR}/../app")
else()
    set(_GASFREE_DEFAULT_APP "${CMAKE_CURRENT_SOURCE_DIR}/../gasfree/app")
endif()
set(GASFREE_APP_DIR "${_GASFREE_DEFAULT_APP}" CACHE PATH "Path to gasfree app directory")

# ---- Dependencies ----
set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap" CACHE PATH "" FORCE)

add_library(gasfree_dsp STATIC
    src/dsp/rack_engine.cpp
    src/dsp/fft_convolver.cpp
    src/dsp/subharm_dsp.cpp
    src/dsp/lfo_dsp.cpp
    src/dsp/comp_dsp.cpp
    src/dsp/delay_dsp.cpp
    src/dsp/reverb_dsp.cpp
    src/dsp/mixer_dsp.cpp
    src/dsp/minimix_dsp.cpp
    src/dsp/hyper_dsp.cpp
)

target_include_directories(gasfree_dsp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include"
)

target_compile_definitions(gasfree_dsp PUBLIC
    GASFREE_DEFAULT_APP_DIR="${GASFREE_APP_DIR}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(gasfree_dsp PRIVATE -Wall -Wextra -Wno-unused-parameter)
elseif(WIN32)
    target_compile_definitions(gasfree_dsp PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ---- Platform GUI + Plugin implementation library ----
set(GUI_SOURCES
    src/gui/resource_provider.cpp
)

if(UNIX AND NOT APPLE)
    list(APPEND GUI_SOURCES src/gui/webview_gui_linux.cpp)
elseif(APPLE)
    list(APPEND GUI_SOURCES src/gui/webview_gui_mac.mm)
elseif(WIN32)
    list(APPEND GUI_SOURCES src/gui/webview_gui_win.cpp)
endif()

add_library(gasfree_impl STATIC
    src/clap_plugin.cpp
    ${GUI_SOURCES}
)

target_link_libraries(gasfree_impl PUBLIC gasfree_dsp)

target_include_directories(gasfree_impl PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/choc"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include"
)

target_compile_definitions(gasfree_impl PRIVATE
    GASFREE_BRIDGE_JS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/plugin-bridge.js"
)

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(WEBKIT2GTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    target_link_libraries(gasfree_impl PUBLIC
        PkgConfig::GTK3
        PkgConfig::WEBKIT2GTK
        PkgConfig::X11
        pthread
        dl
    )
elseif(APPLE)
    target_link_libraries(gasfree_impl PUBLIC
        "-framework Cocoa"
        "-framework WebKit"
    )
elseif(WIN32)
    target_link_libraries(gasfree_impl PUBLIC
        ole32
        oleaut32
        shlwapi
        shell32
        user32
        gdi32
        advapi32
        uuid
        version
    )
endif()

# ---- CLAP (and optional VST3 / AUv2 via clap-wrapper) ----
set(GASFREE_FORMATS CLAP)
if(GASFREE_BUILD_VST3)
    list(APPEND GASFREE_FORMATS VST3)
endif()
if(APPLE AND GASFREE_BUILD_AUV2)
    list(APPEND GASFREE_FORMATS AUV2)
endif()

if(GASFREE_BUILD_VST3 OR (APPLE AND GASFREE_BUILD_AUV2))
    add_subdirectory(third_party/clap-wrapper EXCLUDE_FROM_ALL)
    make_clapfirst_plugins(
        TARGET_NAME gasfree
        IMPL_TARGET gasfree_impl
        OUTPUT_NAME "GASFREE"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin_entry.cpp"
        BUNDLE_IDENTIFIER "app.gasfree.plugin"
        BUNDLE_VERSION "${PROJECT_VERSION}"
        COPY_AFTER_BUILD ${GASFREE_INSTALL_LOCAL}
        PLUGIN_FORMATS ${GASFREE_FORMATS}
    )
    if(APPLE)
        foreach(_bundle_tgt IN ITEMS gasfree_clap gasfree_vst3 gasfree_auv2)
            if(TARGET ${_bundle_tgt})
                add_custom_command(TARGET ${_bundle_tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${_bundle_tgt}>/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory "${GASFREE_APP_DIR}" "$<TARGET_BUNDLE_CONTENT_DIR:${_bundle_tgt}>/Resources/app"
                    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/assets/plugin-bridge.js" "$<TARGET_BUNDLE_CONTENT_DIR:${_bundle_tgt}>/Resources/plugin-bridge.js"
                    COMMENT "Bundling gasfree web UI into ${_bundle_tgt} Contents/Resources"
                )
            endif()
        endforeach()
    endif()
else()
    # Direct CLAP build (zero extra SDK download needed)
    add_library(gasfree_clap MODULE
        src/plugin_entry.cpp
    )
    target_link_libraries(gasfree_clap PRIVATE gasfree_impl)
    set_target_properties(gasfree_clap PROPERTIES
        OUTPUT_NAME "GASFREE"
        PREFIX ""
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    if(APPLE)
        set_target_properties(gasfree_clap PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "clap"
            MACOSX_BUNDLE_GUI_IDENTIFIER "app.gasfree.plugin"
            MACOSX_BUNDLE_BUNDLE_NAME "GASFREE"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        )
        # Ship the web UI inside the bundle so the .clap works on any Mac
        # (found at runtime via Contents/Resources by resource_provider.cpp).
        add_custom_command(TARGET gasfree_clap POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:gasfree_clap>/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${GASFREE_APP_DIR}" "$<TARGET_BUNDLE_CONTENT_DIR:gasfree_clap>/Resources/app"
            COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/assets/plugin-bridge.js" "$<TARGET_BUNDLE_CONTENT_DIR:gasfree_clap>/Resources/plugin-bridge.js"
            COMMENT "Bundling gasfree web UI into GASFREE.clap/Contents/Resources"
        )
    else()
        set_target_properties(gasfree_clap PROPERTIES
            SUFFIX ".clap"
        )
    endif()

    if(GASFREE_INSTALL_LOCAL AND UNIX AND NOT APPLE)
        add_custom_command(TARGET gasfree_clap POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{HOME}/.clap"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:gasfree_clap>" "$ENV{HOME}/.clap/GASFREE.clap"
            COMMENT "Installing GASFREE.clap to ~/.clap/"
        )
    endif()
endif()

# ---- Tests ----
if(GASFREE_BUILD_TESTS)
    add_executable(test_dsp tests/test_dsp.cpp)
    target_link_libraries(test_dsp PRIVATE gasfree_dsp)

    add_executable(test_clap_host tests/test_clap_host.cpp)
    target_include_directories(test_clap_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include")
    if(UNIX)
        target_link_libraries(test_clap_host PRIVATE dl pthread)
    endif()

    if(APPLE)
        add_executable(test_gui_mac tests/test_gui_mac.mm)
        target_include_directories(test_gui_mac PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include")
        target_compile_options(test_gui_mac PRIVATE -fobjc-arc)
        target_link_libraries(test_gui_mac PRIVATE "-framework Cocoa" "-framework WebKit")
    endif()
endif()
