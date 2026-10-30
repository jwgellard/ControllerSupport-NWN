cmake_minimum_required(VERSION 3.15)
project(nwn_controller LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "nwmain.exe is 64-bit; configure with -A x64")
endif()

# Static CRT so the DLL has no runtime dependencies of its own.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Dear ImGui 1.91.5 (MIT, third_party/imgui/LICENSE.txt) for the overlay. Vendored
# code: built without our warning level.
set(IMGUI_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_opengl3.cpp
)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")

set(MOD_SOURCES
    src/controller.cpp
    src/diag.cpp
    src/game.cpp
    src/inject.cpp
    src/config.cpp
    src/overlay.cpp
    src/panelnav.cpp
    src/quickbar.cpp
    src/sdl_api.cpp
    src/text.cpp
    src/log.cpp
)

function(nwnc_setup target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE third_party/SDL2/include third_party/imgui)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${target} PRIVATE opengl32)
endfunction()

add_library(nwn_controller SHARED
    src/proxy.cpp
    ${MOD_SOURCES}
    ${IMGUI_SOURCES}
    src/version.def
)
set_target_properties(nwn_controller PROPERTIES OUTPUT_NAME version)
nwnc_setup(nwn_controller)
set_source_files_properties(src/proxy.cpp ${MOD_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4;/permissive-")

# Offline end-to-end test: hooks the official SDL2.dll in place of nwmain.exe.
option(NWNC_BUILD_TESTS "Build the test harness" OFF)
if(NWNC_BUILD_TESTS)
    add_executable(nwnc_harness
        tests/harness.cpp
        ${MOD_SOURCES}
        ${IMGUI_SOURCES}
    )
    nwnc_setup(nwnc_harness)
    set_source_files_properties(tests/harness.cpp PROPERTIES COMPILE_OPTIONS "/W4;/permissive-")
endif()
