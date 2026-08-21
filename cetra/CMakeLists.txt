# Cetra Graphics Engine Library

# A stale in-tree shader_strings.h (this file used to be generated into src/,
# and it is gitignored, so old checkouts still have one) would silently shadow
# the build-tree copy below -- a quoted #include finds the including file's own
# directory first. Refuse to build until it is gone. Must run before the header
# glob, which would otherwise forward it too.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/shader_strings.h)
    message(FATAL_ERROR
        "Stale cetra/src/shader_strings.h will shadow the generated header. "
        "Delete it and re-configure.")
endif()

# Public include tree at ${CMAKE_BINARY_DIR}/include so apps can #include
# "cetra/foo.h". Holds a forwarding header per real header, plus the generated
# shader_strings.h (below).
set(CETRA_PUBLIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/include)

# Forwarding headers replace a CREATE_LINK symlink (which needs Developer Mode
# on Windows). Each include/cetra/<rel> just #includes the real header by
# absolute path; a quoted include inside that header still resolves against its
# own directory, so nested includes need no extra -I, and editing a real header
# needs no reconfigure since the forwarder never changes. A NEW header does need
# a reconfigure -- same as the source glob below. src/ext is shallow on purpose:
# the JoltC/cimgui subtrees come through their own targets.
file(GLOB _cetra_public_headers RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/procedural/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/*.h)
foreach(_rel IN LISTS _cetra_public_headers)
    file(CONFIGURE
        OUTPUT  "${CETRA_PUBLIC_INCLUDE_DIR}/cetra/${_rel}"
        CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/${_rel}\"\n")
endforeach()

# Shader header generation. Generated into the include tree, not the source
# tree, so a read-only checkout builds and two build dirs never fight over one
# file. gen_shader_header.py is stdlib-only and emits its own MSVC pragmas.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_HEADER ${CETRA_PUBLIC_INCLUDE_DIR}/cetra/shader_strings.h)

# Recursive so an edit to a shared chunk under shaders/include/ retriggers
# generation, not just top-level shaders. CONFIGURE_DEPENDS re-globs at build
# time, so a new shader is picked up without a manual reconfigure.
# (pbr_frag.glsl.old stays excluded -- not *.glsl.)
file(GLOB_RECURSE SHADER_FILES CONFIGURE_DEPENDS ${SHADER_DIR}/*.glsl)

add_custom_command(
    OUTPUT ${SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND} -E echo "Generating shader header..."
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_shader_header.py ${SHADER_DIR} ${SHADER_HEADER}
    DEPENDS ${SHADER_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/gen_shader_header.py
    COMMENT "Generating shader_strings.h from GLSL files"
    VERBATIM
)

add_custom_target(cetra_shaders DEPENDS ${SHADER_HEADER})

# Collect source files
file(GLOB CETRA_SOURCES
    src/*.c
    src/*.cpp
    src/ext/cwalk.c
    src/ext/cJSON.c
    src/ext/log.c
    src/ext/progressbar.c
    src/game/*.c
    src/procedural/*.c
)

# Create the static library
add_library(cetra STATIC ${CETRA_SOURCES})

# Ensure shaders are generated before building
add_dependencies(cetra cetra_shaders)

target_include_directories(cetra
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/ext>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/game>
        # Forwarding-header tree, for apps' #include "cetra/foo.h".
        $<BUILD_INTERFACE:${CETRA_PUBLIC_INCLUDE_DIR}>
        # Generated shader_strings.h lives at include/cetra/, so shader.h's bare
        # #include "shader_strings.h" resolves here (for the library and apps).
        $<BUILD_INTERFACE:${CETRA_PUBLIC_INCLUDE_DIR}/cetra>
        $<INSTALL_INTERFACE:include/cetra>
)

# PUBLIC: engine.h and other public headers include <GL/glew.h>,
# <GLFW/glfw3.h> and <cglm/cglm.h>, so consumers need these include paths.
target_link_libraries(cetra PUBLIC cetra_deps)
target_link_libraries(cetra PRIVATE cetra_warnings)
# Linux only. glibc needs it for the POSIX APIs used here; on macOS it pins
# __DARWIN_C_LEVEL down and hides Darwin extensions (which is why util.c has to
# re-enable _DARWIN_C_SOURCE), and on Windows it is meaningless.
target_compile_definitions(cetra PRIVATE
    $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>)

# Link JoltC physics library if enabled
if(CETRA_BUILD_JOLTC)
    target_link_libraries(cetra PUBLIC joltc)
    target_include_directories(cetra PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/JoltC)
endif()

# meshoptimizer (defined at the top level). PRIVATE: only import.c calls it, and
# a mesh's LOD chain is index offsets on Mesh by the time anything else looks.
target_link_libraries(cetra PRIVATE meshoptimizer)

# Dear ImGui via cimgui (defined at the top level). PUBLIC so all apps that
# link cetra also get the GUI backend.
target_link_libraries(cetra PUBLIC cimgui)
target_include_directories(cetra PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/cimgui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/cimgui/imgui/backends)

# Install targets
install(TARGETS cetra
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# Install headers
install(DIRECTORY src/
    DESTINATION include/cetra
    FILES_MATCHING PATTERN "*.h"
)
