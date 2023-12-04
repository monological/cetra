cmake_minimum_required(VERSION 3.16)
project(engine LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
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
)


# Clone GLEW-cmake from GitHub
execute_process(
    COMMAND git clone https://github.com/Perlmint/glew-cmake.git ${GLEW_DIR}
)

# Add GLEW-cmake directory
add_subdirectory(${GLEW_DIR})

# Find the required libraries
find_library(GLFW_LIBRARY NAMES glfw3 PATHS "${EXTERNAL_INSTALL_LOCATION}/lib" NO_DEFAULT_PATH)
message(STATUS "GLFW Library found at: ${GLFW_LIBRARY}")

find_library(CGLM_LIBRARY NAMES cglm libcglm PATHS ${EXTERNAL_INSTALL_LOCATION}/lib NO_DEFAULT_PATH)
message(STATUS "CGLM Library found at: ${CGLM_LIBRARY}")

find_library(ASSIMP_LIBRARY NAMES assimp PATHS "${EXTERNAL_INSTALL_LOCATION}/lib" NO_DEFAULT_PATH)
message(STATUS "ASSIMP Library found at: ${ASSIMP_LIBRARY}")

# Include the directories for header files
include_directories(${EXTERNAL_INSTALL_LOCATION}/include)


# Add the target for the engine library
add_library(engine SHARED 
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
    src/cwalk.c
    src/str.c
)

# Ensure GLEW and external libraries are built before the engine target
add_dependencies(engine glfw cglm libglew_static assimp)

# Link the engine with external libraries
target_link_libraries(engine "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static ${ASSIMP_LIBRARY})

add_executable(cetra src/main.c)
target_link_libraries(cetra engine "${EXTERNAL_INSTALL_LOCATION}/lib/libglfw3.a" ${CGLM_LIBRARY} libglew_static ${ASSIMP_LIBRARY})

# Platform-specific configurations for linking
if(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(engine ${COCOA_LIBRARY} ${IOKIT_LIBRARY} ${COREVIDEO_LIBRARY} ${OPENGL_LIBRARY})
    target_link_libraries(cetra ${COCOA_LIBRARY} ${IOKIT_LIBRARY} ${COREVIDEO_LIBRARY} ${OPENGL_LIBRARY})
elseif(UNIX)
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(engine ${X11_LIBRARIES} ${OPENGL_LIBRARIES} dl m pthread)
    target_link_libraries(cetra ${X11_LIBRARIES} ${OPENGL_LIBRARIES} dl m pthread)
elseif(WIN32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(engine OpenGL::GL)
    target_link_libraries(cetra OpenGL::GL)
endif()

# Copy models directory and glsl files to the build directory
file(COPY models DESTINATION ${CMAKE_BINARY_DIR})
file(COPY textures DESTINATION ${CMAKE_BINARY_DIR})
file(COPY shaders DESTINATION ${CMAKE_BINARY_DIR})

