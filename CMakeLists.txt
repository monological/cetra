cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(cetra
    VERSION 0.1
    DESCRIPTION "Cetra Graphics Engine"
    LANGUAGES C CXX)

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
    add_compile_definitions(GL_SILENCE_DEPRECATION)
    set(HOMEBREW_PREFIX "/opt/homebrew" CACHE PATH "Homebrew prefix")
    list(APPEND CMAKE_PREFIX_PATH ${HOMEBREW_PREFIX})
    include_directories(${HOMEBREW_PREFIX}/include)
    link_directories(${HOMEBREW_PREFIX}/lib)
endif()

# Note: _POSIX_C_SOURCE is set per-target in cetra/CMakeLists.txt
# Setting it globally breaks JoltC build (alloca not declared)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GLEW REQUIRED glew)
pkg_check_modules(CGLM REQUIRED cglm)
pkg_check_modules(ASSIMP REQUIRED assimp)

# Build JoltC if enabled
if(CETRA_BUILD_JOLTC)
    # Enable debug renderer for JoltC (needed for DrawSettings, etc.)
    set(DEBUG_RENDERER_IN_DISTRIBUTION ON CACHE BOOL "" FORCE)
    add_subdirectory(cetra/src/ext/JoltC)
endif()

# Build the cetra library
add_subdirectory(cetra)

# Build apps if enabled
if(CETRA_BUILD_APPS)
    add_subdirectory(apps)
endif()
