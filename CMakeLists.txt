# 3.25 matches CMakePresets.json, the way this project is meant to be built; it
# also makes CMP0141 (below) NEW by default.
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

# On a Windows manifest build, chainload vcpkg's toolchain if VCPKG_ROOT is set
# and nothing else supplied one. The windows preset sets this explicitly too;
# this only rescues a bare `cmake -B` invocation. Must precede project().
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE
   AND CMAKE_HOST_WIN32
   AND DEFINED ENV{VCPKG_ROOT}
   AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain")
endif()

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

# Third-party dependencies on one interface target: consumers link the single
# name cetra_deps, and only the discovery block below varies by platform.
add_library(cetra_deps INTERFACE)

# pkg-config on macOS/Linux; find_package (vcpkg) on Windows. 'auto' picks the
# latter when a vcpkg toolchain is active, so the Windows path can also be
# exercised on macOS with -DCETRA_DEP_PROVIDER=cmake.
set(CETRA_DEP_PROVIDER "auto" CACHE STRING "Dependency discovery: auto | pkgconfig | cmake")
set_property(CACHE CETRA_DEP_PROVIDER PROPERTY STRINGS auto pkgconfig cmake)

set(_cetra_provider "${CETRA_DEP_PROVIDER}")
if(_cetra_provider STREQUAL "auto")
    if(WIN32 OR CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
        set(_cetra_provider "cmake")
    else()
        set(_cetra_provider "pkgconfig")
    endif()
endif()

if(_cetra_provider STREQUAL "pkgconfig")
    # IMPORTED_TARGET so PkgConfig::* carry include dirs, link dirs and libraries
    # together -- the bare pkg_check_modules variables drop the link dirs. GLOBAL
    # promotes them out of this directory scope so subdirectories may name them.
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
else()
    # vcpkg exports these config packages; the target names are their own.
    find_package(glfw3  CONFIG REQUIRED)
    find_package(GLEW   CONFIG REQUIRED)
    find_package(assimp CONFIG REQUIRED)
    find_package(cglm   CONFIG REQUIRED)
    target_link_libraries(cetra_deps INTERFACE
        glfw
        GLEW::GLEW
        assimp::assimp
        cglm::cglm)
endif()

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
