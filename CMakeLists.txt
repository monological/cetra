cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(cetra
    VERSION 0.1
    DESCRIPTION "Cetra Graphics Engine"
    LANGUAGES C CXX)

# MSVC debug info as /Z7 (Embedded) rather than /Zi (ProgramDatabase): with
# Ninja, parallel compiles sharing one .pdb need /FS serialization and are a
# known source of flaky failures. Must be set before any target is created.
# No effect on non-MSVC toolchains.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
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
    # CMAKE_PREFIX_PATH only, deliberately: the global include_directories and
    # link_directories that used to live here put Homebrew's prefix on every
    # target's search path, which masked the fact that cetra's public headers
    # include <GL/glew.h>, <GLFW/glfw3.h> and <cglm/cglm.h> while those
    # dependencies were attached PRIVATE. Apps compiled by accident. They now
    # come through cetra_deps, linked PUBLIC.
    set(HOMEBREW_PREFIX "/opt/homebrew" CACHE PATH "Homebrew prefix")
    list(APPEND CMAKE_PREFIX_PATH ${HOMEBREW_PREFIX})
endif()

# Note: _POSIX_C_SOURCE is set per-target in cetra/CMakeLists.txt
# Setting it globally breaks JoltC build (alloca not declared)

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
    target_compile_options(cetra_warnings INTERFACE -Wall)
endif()

# Third-party dependencies, gathered onto one interface target so that consumers
# link a single name and the discovery mechanism can vary by platform.
#
# IMPORTED_TARGET is what makes this correct rather than merely tidy: the plain
# pkg_check_modules variables give bare library names with the search path in a
# separate _LIBRARY_DIRS variable that nothing here ever consumed, so linking
# only worked for libraries already on the linker's default path. PkgConfig::*
# carries include dirs, link dirs and libraries together. GLOBAL is required
# because cetra_deps is consumed from subdirectories.
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
