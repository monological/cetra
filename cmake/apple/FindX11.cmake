# Shadows CMake's own FindX11, for macOS builds only. This directory joins
# CMAKE_MODULE_PATH under if(APPLE) in the root CMakeLists and nowhere else --
# on Linux the real module must run, because GLX genuinely needs X11 there.
#
# Vendored glew-cmake asks for X11 on any non-Windows host
# (cetra/src/ext/glew/CMakeLists.txt:142), REQUIRED. macOS used to miss that
# branch: glew special-cases APPLE to link AGL instead -- but only below Darwin
# 25, since macOS 26 removed AGL. At or above it macOS falls through to the Unix
# branch, so the find is fatal on any Mac with neither XQuartz nor a Homebrew
# libx11 -- neither of which `brew install cmake ninja` installs. Upstream
# master carries the same gate, so there is no version to bump to.
#
# macOS wants no X11 at all: GL arrives through OpenGL.framework and GLFW builds
# its Cocoa backend (its own GLFW_BUILD_X11 is gated "UNIX;NOT APPLE"). So the
# honest answer to "find X11" here is "found, it contributes nothing", which is
# what the macOS link line should have carried all along. glew appends either
# ${X11_X11_LIB} ${X11_Xext_LIB} or the imported targets depending on
# USE_NAMESPACED_LIB; both append nothing here.

set(X11_FOUND TRUE)

set(X11_INCLUDE_DIR "")
set(X11_LIBRARIES "")
set(X11_X11_INCLUDE_PATH "")
set(X11_X11_LIB "")
set(X11_Xext_INCLUDE_PATH "")
set(X11_Xext_LIB "")

if(NOT TARGET X11::X11)
    add_library(X11::X11 INTERFACE IMPORTED)
endif()

if(NOT TARGET X11::Xext)
    add_library(X11::Xext INTERFACE IMPORTED)
endif()
