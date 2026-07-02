cmake_minimum_required(VERSION 3.20)
project(NukeGUI CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(NUKE_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")

# imgui is VENDORED STATICALLY into this plugin (its own context; symbols stay internal — no dependency
# on the editor's NukeImGui.dll, no shared context). Compile the same imgui sources directly in.
add_library(NukeGUI SHARED
    src/NukeGUI.cpp
    ${NUKE_ROOT}/NukeImGui/imgui/imgui.cpp
    ${NUKE_ROOT}/NukeImGui/imgui/imgui_draw.cpp
    ${NUKE_ROOT}/NukeImGui/imgui/imgui_tables.cpp
    ${NUKE_ROOT}/NukeImGui/imgui/imgui_widgets.cpp
)

# Consumer of the engine DLL: NUKEENGINE_API symbols are dllimport, link NukeEngine.lib.
# NOTE: do NOT define IMGUI_API here — imgui compiles with the default (empty) linkage so its symbols
# are internal to NukeGUI.dll and never clash with the editor's exported NukeImGui imgui.
target_compile_definitions(NukeGUI PRIVATE
    WIN32 _WINDOWS NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    GLM_ENABLE_EXPERIMENTAL
    BOOST_ALL_DYN_LINK
)

target_include_directories(NukeGUI PRIVATE
    ${NUKE_ROOT}/NukeEngine
    ${NUKE_ROOT}/NukeEngine/include
    ${NUKE_ROOT}/NukeEngine/deps
    ${NUKE_ROOT}/NukeEngine/deps/glm
    ${NUKE_ROOT}/NukeImGui/imgui          # imgui.h (vendored)
    ${VCPKG_INSTALLED}/include            # boost + nlohmann
)

target_link_directories(NukeGUI PRIVATE
    ${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>     # NukeEngine.lib
    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib
)

target_link_libraries(NukeGUI PRIVATE NukeEngine)

# Deploy the plugin DLL into the editor's modules/ folder.
set(MODULES_DIR "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/modules")
add_custom_command(TARGET NukeGUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODULES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:NukeGUI>" "${MODULES_DIR}/"
    COMMENT "Deploying NukeGUI.dll to modules/"
)
