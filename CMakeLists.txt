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

# Third-party libraries, vendored as pinned submodules under cetra/src/ext and
# built static from source so the versions are identical on every platform (see
# specs/8.7-vendor-dependencies.md). Consumers link the single name cetra_deps;
# the system libraries (OpenGL, X11, Apple frameworks, CRT) stay dynamic and are
# added below.
add_library(cetra_deps INTERFACE)

# Build the four vendored libraries static. Scoped before their add_subdirectory
# calls; Jolt and cimgui manage their own linkage further down.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# GLFW. On Linux it enables both the X11 and Wayland backends by default (chosen
# at runtime), which needs the X11 + Wayland dev headers on the build machine --
# system-level, not vendorable (the display-server protocol is the OS's). No
# backend override is needed: GLFW's own cmake_dependent_option turns both on for
# Linux and uses the native Cocoa/Win32 backend on macOS/Windows.
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
add_subdirectory(cetra/src/ext/glfw)

# GLEW (Perlmint/glew-cmake fork ships pre-generated sources + CMake).
# libglew_static carries GLEW_STATIC as a PUBLIC define.
set(glew-cmake_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(glew-cmake_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(ONLY_LIBS ON CACHE BOOL "" FORCE)
add_subdirectory(cetra/src/ext/glew)

# cglm -- header-only; the cglm_headers interface target is include-dirs only.
set(CGLM_USE_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(cetra/src/ext/cglm)

# Assimp. Let it choose zlib: the system copy on macOS/Linux, its own bundled
# copy on Windows (where there is no system zlib). Forcing the bundled zlib
# everywhere fails on modern macOS SDKs -- its vendored zlib is too old -- and
# zlib's on-disk format is stable, so this is not a version-pin concern the way
# the four libraries above are.
set(ASSIMP_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES      OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL            OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(cetra/src/ext/assimp)

target_link_libraries(cetra_deps INTERFACE
    glfw
    libglew_static
    cglm_headers
    assimp::assimp)

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

if(WIN32)
    # Expose M_PI from <math.h> (used in ~6 files); silence the deprecation of
    # POSIX-ish names like strdup; slim <windows.h>, with NOMINMAX so its min/max
    # macros don't collide with C++ std::min/max in the Jolt/ImGui units.
    target_compile_definitions(cetra_deps INTERFACE
        _USE_MATH_DEFINES
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
endif()

# Jolt defaults that only bite under MSVC; force them before its add_subdirectory
# so the cached values win in its scope. All inert on macOS/Linux.
if(MSVC)
    # Jolt forces the static CRT (/MT) while everything else defaults to dynamic
    # (/MD) -- a guaranteed LNK2038. Pin dynamic and stop Jolt overriding it. Our
    # vendored deps build static against this same dynamic CRT, so there are no
    # DLLs to ship next to the exe.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CACHE STRING "" FORCE)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    # Jolt's MSVC branch is /Wall /WX, which clang-cl maps to -Weverything
    # -Werror and cannot survive; and its LTO emits bitcode .lib link.exe can't
    # consume.
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
    # These default ON and emit AVX2/BMI, which a KVM guest's default CPU model
    # does not expose -- a SIGILL at the first physics tick, not a build error.
    # Re-enable once the guest is CPU-passthrough. SSE4.1/4.2 stay on (universal).
    foreach(_jolt_isa USE_AVX2 USE_AVX512 USE_F16C USE_FMADD USE_LZCNT USE_TZCNT)
        set(${_jolt_isa} OFF CACHE BOOL "" FORCE)
    endforeach()
endif()

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

# cimgui sets IMGUI_IMPL_API to __declspec(dllexport) on Windows even in the
# static config we force, which pushes the backend symbols into every consumer's
# export table. Rewrite it to plain C linkage. Inert off Windows.
if(WIN32)
    get_target_property(_cimgui_defs cimgui INTERFACE_COMPILE_DEFINITIONS)
    if(_cimgui_defs)
        list(FILTER _cimgui_defs EXCLUDE REGEX "^IMGUI_IMPL_API=")
        list(APPEND _cimgui_defs "IMGUI_IMPL_API=extern \"C\" ")
        set_target_properties(cimgui PROPERTIES INTERFACE_COMPILE_DEFINITIONS "${_cimgui_defs}")
    endif()
endif()

# Build the cetra library
add_subdirectory(cetra)

# Build apps if enabled
if(CETRA_BUILD_APPS)
    add_subdirectory(apps)
endif()
