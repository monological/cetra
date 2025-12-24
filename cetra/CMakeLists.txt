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

file(GLOB SHADER_FILES ${SHADER_DIR}/*.glsl)

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
    PRIVATE
        ${GLFW_INCLUDE_DIRS}
        ${GLEW_INCLUDE_DIRS}
        ${CGLM_INCLUDE_DIRS}
        ${ASSIMP_INCLUDE_DIRS}
)

target_link_libraries(cetra
    PUBLIC
        ${GLFW_LIBRARIES}
        ${GLEW_LIBRARIES}
        ${CGLM_LIBRARIES}
        ${ASSIMP_LIBRARIES}
        m
        pthread
)

# Platform-specific frameworks
if(APPLE)
    target_link_libraries(cetra PUBLIC
        "-framework Cocoa"
        "-framework OpenGL"
        "-framework IOKit"
        "-framework CoreVideo"
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(cetra PUBLIC
        X11
        GL
        ncurses
    )
endif()

if(WIN32)
    target_link_libraries(cetra PUBLIC
        glu32
        gdi32
        opengl32
        kernel32
    )
endif()

target_compile_options(cetra PRIVATE -Wall -g)
target_compile_definitions(cetra PRIVATE _POSIX_C_SOURCE=200809L)

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
