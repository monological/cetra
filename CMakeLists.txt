# 3.25 matches CMakePresets.json, the way this project is meant to be built; it
# also makes CMP0141 (below) NEW by default.
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

project(cetra
    VERSION 0.1
    DESCRIPTION "Cetra Graphics Engine"
    LANGUAGES C CXX)

if(MSVC)
    # /Z7 (Embedded) over /Zi (ProgramDatabase): with Ninja, parallel compiles
    # sharing one .pdb need /FS serialization and flake. Read at target creation,
    # so it must precede any target.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(CETRA_BUILD_APPS "Build example applications" ON)
option(CETRA_BUILD_JOLTC "Build JoltC physics library" ON)

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Platform-specific settings
if(APPLE)
    # Prefix path for find_package / pkg-config. Deliberately not
    # include_directories/link_directories: dependency paths belong on
    # cetra_deps, not on every target globally.
    set(HOMEBREW_PREFIX "/opt/homebrew" CACHE PATH "Homebrew prefix")
    list(APPEND CMAKE_PREFIX_PATH ${HOMEBREW_PREFIX})
endif()

# Note: _POSIX_C_SOURCE is set on the cetra target, Linux-only, in
# cetra/CMakeLists.txt. Setting it globally breaks the JoltC build (alloca not
# declared), and on macOS it hides Darwin extensions.

# Warning/diagnostic flags, carried on an interface target so the library and
# every app share one definition.
#
# Branch on MSVC, not on CMAKE_C_COMPILER_ID: for clang-cl the compiler id is
# "Clang" but the driver takes MSVC-style flags, so keying off the id would feed
# -Wall to a /-style driver.
add_library(cetra_warnings INTERFACE)

if(MSVC)
    target_compile_options(cetra_warnings INTERFACE
        /W4
        /wd4100  # unreferenced formal parameter
        /wd4244  # conversion, possible loss of data
        /wd4267  # size_t -> smaller type
        /wd4996) # POSIX/CRT deprecation
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # clang-cl: quiet the MSVC-isms clang does not implement
        target_compile_options(cetra_warnings INTERFACE
            -Wno-unused-command-line-argument
            -Wno-microsoft-include)
    endif()
else()
    # -Wall only, not -Wextra: match the existing macOS warning surface. Raising
    # it is a separate cleanup, not this refactor.
    target_compile_options(cetra_warnings INTERFACE -Wall)
endif()

# Third-party dependencies on one interface target: consumers link a single
# name, and the discovery mechanism can vary by platform (vcpkg is planned).
#
# IMPORTED_TARGET so the PkgConfig::* targets carry include dirs, link dirs and
# libraries together -- the bare pkg_check_modules variables drop the link dirs.
# GLOBAL promotes them out of this directory scope so a subdirectory may name
# PkgConfig::GLFW directly.
add_library(cetra_deps INTERFACE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW   REQUIRED IMPORTED_TARGET GLOBAL glfw3)
pkg_check_modules(GLEW   REQUIRED IMPORTED_TARGET GLOBAL glew)
pkg_check_modules(CGLM   REQUIRED IMPORTED_TARGET GLOBAL cglm)
pkg_check_modules(ASSIMP REQUIRED IMPORTED_TARGET GLOBAL assimp)

target_link_libraries(cetra_deps INTERFACE
    PkgConfig::GLFW
    PkgConfig::GLEW
    PkgConfig::CGLM
    PkgConfig::ASSIMP)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(cetra_deps INTERFACE OpenGL::GL Threads::Threads)

if(UNIX)
    target_link_libraries(cetra_deps INTERFACE m)
endif()

if(APPLE)
    target_link_libraries(cetra_deps INTERFACE
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo")
    # Suppress duplicate library warnings from CMake's transitive linking
    target_link_options(cetra_deps INTERFACE "LINKER:-no_warn_duplicate_libraries")
endif()

target_compile_definitions(cetra_deps INTERFACE
    $<$<PLATFORM_ID:Darwin>:GL_SILENCE_DEPRECATION>)

# Build JoltC if enabled
if(CETRA_BUILD_JOLTC)
    # Enable debug renderer for JoltC (needed for DrawSettings, etc.)
    set(DEBUG_RENDERER_IN_DISTRIBUTION ON CACHE BOOL "" FORCE)
    add_subdirectory(cetra/src/ext/JoltC)
endif()

# Dear ImGui via cimgui (C bindings). cimgui's own CMake builds the ImGui core
# with IMGUI_IMPL_API=extern "C" (C linkage for the backend functions); we force
# it static and append the GLFW + OpenGL3 backends (absent from the stock target)
# plus the CIMGUI_USE_* switches that expose their C bindings in cimgui_impl.h.
# The cetra library links this; the PUBLIC defines/includes reach engine.c.
set(IMGUI_STATIC "yes" CACHE STRING "Build cimgui as a static library" FORCE)
add_subdirectory(cetra/src/ext/cimgui)
set(CIMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cetra/src/ext/cimgui)
target_sources(cimgui PRIVATE
    ${CIMGUI_DIR}/cimgui_impl.cpp
    ${CIMGUI_DIR}/imgui/backends/imgui_impl_glfw.cpp
    ${CIMGUI_DIR}/imgui/backends/imgui_impl_opengl3.cpp)
target_compile_definitions(cimgui PUBLIC CIMGUI_USE_GLFW CIMGUI_USE_OPENGL3)
target_include_directories(cimgui PUBLIC ${CIMGUI_DIR}/imgui/backends)
# Plain signature, deliberately: cimgui's own CMakeLists links with the plain
# form, and CMake hard-errors if plain and keyword forms are mixed on one target.
target_link_libraries(cimgui cetra_deps)

# Build the cetra library
add_subdirectory(cetra)

# Build apps if enabled
if(CETRA_BUILD_APPS)
    add_subdirectory(apps)
endif()
