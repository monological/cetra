cmake_minimum_required(VERSION 3.16)
project(engine LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-strict-prototypes")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

include(ExternalProject)

set(GLFW_VERSION "3.3.8")
set(CGLM_VERSION "v0.9.1")
set(ASSIMP_VERSION "v5.3.1")

add_definitions(-DGLEW_STATIC)
set(GLEW_DIR "${CMAKE_BINARY_DIR}/glew-cmake")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-strict-prototypes")

# Define the external install location
set(EXTERNAL_INSTALL_LOCATION ${CMAKE_BINARY_DIR}/external)

set(CMAKE_INSTALL_RPATH "${EXTERNAL_INSTALL_LOCATION}/lib;@loader_path")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# needed by progress bar
set(CURSES_NEED_NCURSES TRUE)
find_package(Curses)
include_directories(${CURSES_INCLUDE_DIRS})

# Add cglm as an external project
ExternalProject_Add(
    cglm
    GIT_REPOSITORY https://github.com/recp/cglm
    GIT_TAG ${CGLM_VERSION}
    INSTALL_DIR "${EXTERNAL_INSTALL_LOCATION}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DCMAKE_C_FLAGS=-Wno-strict-prototypes
    STEP_TARGETS build
    EXCLUDE_FROM_ALL TRUE
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND find <SOURCE_DIR> -type f -exec dos2unix {} +
    BUILD_ALWAYS 1
)

# Add GLFW as an external project
ExternalProject_Add(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG ${GLFW_VERSION}
    INSTALL_DIR "${EXTERNAL_INSTALL_LOCATION}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DCMAKE_C_FLAGS=-Wno-strict-prototypes
    STEP_TARGETS build
    EXCLUDE_FROM_ALL TRUE
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND find <SOURCE_DIR> -type f -exec dos2unix {} +
    BUILD_ALWAYS 1
)

ExternalProject_Add(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp
    GIT_TAG ${ASSIMP_VERSION}
    INSTALL_DIR "${EXTERNAL_INSTALL_LOCATION}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DCMAKE_C_FLAGS=-Wno-strict-prototypes
    STEP_TARGETS build
    EXCLUDE_FROM_ALL TRUE
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND find <SOURCE_DIR> -type f -exec dos2unix {} +
    BUILD_ALWAYS 1
)


# Clone GLEW-cmake from GitHub
execute_process(
    COMMAND git clone https://github.com/Perlmint/glew-cmake.git ${GLEW_DIR}
)

# Add GLEW-cmake directory
add_subdirectory(${GLEW_DIR})



add_custom_target(FindLibraries ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Finding external libraries..."
    COMMAND ${CMAKE_COMMAND} -D CMAKE_PREFIX_PATH=${EXTERNAL_INSTALL_LOCATION} -P ${CMAKE_CURRENT_LIST_DIR}/FindLibraries.cmake
    DEPENDS assimp
)

# Include the directories for header files
include_directories(${EXTERNAL_INSTALL_LOCATION}/include)


# Add the target for the engine library
add_library(cetra SHARED 
    src/engine.c
    src/util.c
    src/scene.c
    src/shader.c
    src/program.c
    src/mesh.c
    src/texture.c
    src/camera.c
    src/light.c
    src/import.c
    src/ext/cwalk.c
    src/ext/progressbar.c
)

# Ensure GLEW and external libraries are built before the engine target
add_dependencies(cetra glfw cglm libglew_static assimp FindLibraries)

# Link the engine with external libraries
if(APPLE)
    target_link_libraries(cetra "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static "${EXTERNAL_INSTALL_LOCATION}/lib/libassimp.dylib")

    add_executable(render src/main.c)
    target_link_libraries(render cetra "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static "${EXTERNAL_INSTALL_LOCATION}/lib/libassimp.dylib")
elseif(UNIX)
    target_link_libraries(cetra "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static "${EXTERNAL_INSTALL_LOCATION}/lib/libassimp.so")

    add_executable(render src/main.c)
    target_link_libraries(render cetra "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static "${EXTERNAL_INSTALL_LOCATION}/lib/libassimp.so")
elseif(WIN32)
    target_link_libraries(cetra "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static "${EXTERNAL_INSTALL_LOCATION}/lib/libassimp.dll")

    add_executable(render src/main.c)
    target_link_libraries(render cetra "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static "${EXTERNAL_INSTALL_LOCATION}/lib/libassimp.dll")
endif()

# Platform-specific configurations for linking
if(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(cetra ${COCOA_LIBRARY} ${IOKIT_LIBRARY} ${COREVIDEO_LIBRARY} ${OPENGL_LIBRARY} ${CURSES_LIBRARIES})
    target_link_libraries(render ${COCOA_LIBRARY} ${IOKIT_LIBRARY} ${COREVIDEO_LIBRARY} ${OPENGL_LIBRARY} ${CURSES_LIBRARIES})
elseif(UNIX)
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(cetra ${X11_LIBRARIES} ${OPENGL_LIBRARIES} dl m pthread ${CURSES_LIBRARIES})
    target_link_libraries(render ${X11_LIBRARIES} ${OPENGL_LIBRARIES} dl m pthread ${CURSES_LIBRARIES})
elseif(WIN32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(cetra OpenGL::GL)
    target_link_libraries(render OpenGL::GL)
endif()

add_custom_command(
    TARGET render POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/models ${CMAKE_BINARY_DIR}/models
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/textures ${CMAKE_BINARY_DIR}/textures
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders ${CMAKE_BINARY_DIR}/shaders
    COMMENT "Copying models, textures, and shaders to build directory"
)
