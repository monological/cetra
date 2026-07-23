# Cetra Graphics Engine Library

# Create include directory structure for apps (cetra/xxx.h includes)
# This creates a symlink so apps can use #include "cetra/common.h"
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include)
if(NOT EXISTS ${CMAKE_BINARY_DIR}/include/cetra)
    file(CREATE_LINK
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/include/cetra
        SYMBOLIC
    )
endif()

# Shader header generation
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/src/shader_strings.h)

# Recursive so shared chunks under include/ are dependencies too: without them
# a chunk edit would not retrigger generation and the build would quietly keep
# the stale expansion baked into shader_strings.h. CONFIGURE_DEPENDS re-globs
# at build time, so a brand-new shader or chunk is picked up without a manual
# reconfigure. (pbr_frag.glsl.old stays excluded -- it does not match *.glsl.)
file(GLOB_RECURSE SHADER_FILES CONFIGURE_DEPENDS ${SHADER_DIR}/*.glsl)

add_custom_command(
    OUTPUT ${SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND} -E echo "Generating shader header..."
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_shader_header.py ${SHADER_DIR} ${SHADER_HEADER}
    DEPENDS ${SHADER_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/gen_shader_header.py
    COMMENT "Generating shader_strings.h from GLSL files"
    VERBATIM
)

add_custom_target(shaders DEPENDS ${SHADER_HEADER})

# Collect source files
file(GLOB CETRA_SOURCES
    src/*.c
    src/ext/cwalk.c
    src/ext/cJSON.c
    src/ext/log.c
    src/ext/progressbar.c
    src/game/*.c
)

# Create the static library
add_library(cetra STATIC ${CETRA_SOURCES})

# Ensure shaders are generated before building
add_dependencies(cetra shaders)

target_include_directories(cetra
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/ext>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/game>
        $<INSTALL_INTERFACE:include/cetra>
)

# PUBLIC: engine.h and other public headers include <GL/glew.h>,
# <GLFW/glfw3.h> and <cglm/cglm.h>, so consumers need these include paths.
target_link_libraries(cetra PUBLIC cetra_deps)
target_link_libraries(cetra PRIVATE cetra_warnings)
target_compile_definitions(cetra PRIVATE _POSIX_C_SOURCE=200809L)

# Link JoltC physics library if enabled
if(CETRA_BUILD_JOLTC)
    target_link_libraries(cetra PUBLIC joltc)
    target_include_directories(cetra PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/JoltC)
endif()

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
