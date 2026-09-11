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

# Two include trees, both of forwarding headers, under PROJECT_BINARY_DIR for
# the reason the output directories in the root CMakeLists give:
#
#   include/cetra/<name>.h            the public surface, what an app may NAME
#   include-internal/cetra/internal/  everything else
#
# The first is what a project that adds this directory sees. The second is
# private to the library and reachable only by the in-tree apps (add_cetra_app
# links the cetra_internal target), so an app that includes cetra/internal/ is
# saying it is a debug harness for the engine rather than a user of it.
#
# The allowlist governs what may be NAMED in an include, not what a public
# header's closure reaches: material.h includes roads.h and texture.h includes
# thread.h, and both stay internal. A header listed here is a promise that its
# contents are meant for an app; one missing from it is a statement that they
# are not, and adding one is a one-line change that the list makes visible.
set(CETRA_PUBLIC_INCLUDE_DIR ${PROJECT_BINARY_DIR}/include)
set(CETRA_INTERNAL_INCLUDE_DIR ${PROJECT_BINARY_DIR}/include-internal)

set(CETRA_PUBLIC_HEADERS
    animation.h animator.h app.h camera.h common.h compat.h config_snapshot.h cook.h
    cscene.h decal.h engine.h exposure.h geometry.h gi_volume.h ibl.h ies.h
    import.h input.h light.h material.h mesh.h mesh_builder.h noise.h
    particle_emitter.h particle_module.h particle_renderer.h particle_sim.h
    particle_system.h postfx.h probe.h probe_set.h program.h scene.h shader.h
    shadow.h sky.h springbone.h text.h texture.h transform.h ui.h uniform.h util.h
    water.h wind.h
    ext/log.h ext/cwalk.h)

# Forwarding headers replace a CREATE_LINK symlink (which needs Developer Mode
# on Windows). Each forwarder just #includes the real header by absolute path;
# a quoted include inside that header still resolves against its own directory,
# so nested includes need no extra -I, and editing a real header needs no
# reconfigure since the forwarder never changes. A NEW header does need a
# reconfigure -- same as the source glob below. src/ext is shallow on purpose:
# the JoltC/cimgui subtrees come through their own targets. Everything under
# game/ and procedural/ is public: the game framework and the terrain stack are
# app-facing throughout.
file(GLOB _cetra_all_headers RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/procedural/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/*.h)
set(_cetra_forwarders "")
foreach(_rel IN LISTS _cetra_all_headers)
    if(_rel IN_LIST CETRA_PUBLIC_HEADERS OR _rel MATCHES "^(game|procedural)/")
        set(_dest "${CETRA_PUBLIC_INCLUDE_DIR}/cetra/${_rel}")
    else()
        set(_dest "${CETRA_INTERNAL_INCLUDE_DIR}/cetra/internal/${_rel}")
    endif()
    file(CONFIGURE
        OUTPUT  "${_dest}"
        CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/${_rel}\"\n")
    list(APPEND _cetra_forwarders "${_dest}")
endforeach()
# A header that left the allowlist, or the tree, leaves its old forwarder
# behind in an existing build directory, where an include the boundary now
# refuses keeps resolving until someone wipes the build -- which is how a
# consumer compiled against an internal header for a whole spec. Anything in
# either tree that this configure did not just write goes.
file(GLOB_RECURSE _cetra_stale
    ${CETRA_PUBLIC_INCLUDE_DIR}/cetra/*.h
    ${CETRA_INTERNAL_INCLUDE_DIR}/cetra/*.h)
list(REMOVE_ITEM _cetra_stale ${_cetra_forwarders})
if(_cetra_stale)
    file(REMOVE ${_cetra_stale})
endif()
foreach(_rel IN LISTS CETRA_PUBLIC_HEADERS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_rel}")
        message(FATAL_ERROR "CETRA_PUBLIC_HEADERS names src/${_rel}, which does not exist")
    endif()
endforeach()

# Shader header generation. Generated into the build tree, not the source tree,
# so a read-only checkout builds and two build dirs never fight over one file;
# and into a directory of its own that only this library searches, so no app
# or consumer object can include it. gen_shader_header.py is stdlib-only and
# emits its own MSVC pragmas.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_GEN_DIR ${PROJECT_BINARY_DIR}/generated)
set(SHADER_HEADER ${SHADER_GEN_DIR}/shader_strings.h)
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})

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

# The static library. Listing the generated header as a source is what orders
# its generation before the objects that include it.
add_library(cetra STATIC ${CETRA_SOURCES} ${SHADER_HEADER})

# The internal tree as a target, for an in-tree app to link: what makes
# #include "cetra/internal/foo.h" resolve. Nothing links it but add_cetra_app,
# so a consumer cannot name an internal header.
add_library(cetra_internal INTERFACE)
target_include_directories(cetra_internal INTERFACE ${CETRA_INTERNAL_INCLUDE_DIR})

target_include_directories(cetra
    PUBLIC
        # The public forwarders, for #include "cetra/foo.h".
        ${CETRA_PUBLIC_INCLUDE_DIR}
    PRIVATE
        # The library's own bare includes. Private, so an app cannot name an
        # internal header by its bare spelling and skip the boundary above.
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ext
        ${CMAKE_CURRENT_SOURCE_DIR}/src/game
        # program.c's #include "shader_strings.h".
        ${SHADER_GEN_DIR}
)

# PUBLIC: engine.h and other public headers include <GL/glew.h>,
# <GLFW/glfw3.h> and <cglm/cglm.h>, so consumers need these include paths.
target_link_libraries(cetra PUBLIC cetra_deps)
target_link_libraries(cetra PRIVATE cetra_warnings)
# Linux only. glibc needs it for the POSIX APIs used here; on macOS it pins
# __DARWIN_C_LEVEL down and hides Darwin extensions (which is why util.c has to
# re-enable _DARWIN_C_SOURCE), and on Windows it is meaningless.
#
# _DEFAULT_SOURCE rides with it because naming _POSIX_C_SOURCE explicitly is what
# TURNS OFF glibc's default feature set, and M_PI lives in the part that goes with
# it -- guarded by __USE_MISC || __USE_XOPEN, neither implied by POSIX alone. Four
# files here use it (light.c, emissive_light.c, tree_gen.c, vegetation_tex.c); the
# other ten reach for cglm's CGLM_PI, which is GLM_PIf and therefore a FLOAT, so
# converting the four to match would change double arithmetic to single rather
# than fix a portability bug. macOS never needed either: its math.h exposes M_PI
# unconditionally, so this only ever failed on the platform nobody configures on.
target_compile_definitions(cetra PRIVATE
    $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
    $<$<PLATFORM_ID:Linux>:_DEFAULT_SOURCE>)

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

# No install rule. The engine is built as a subdirectory of the project that
# uses it (README, "Using cetra in your project"). The install rule that was
# here was deleted: it had no export set, so find_package could not find the
# result, and it installed none of the vendored static libraries cetra links,
# so nothing built against its prefix could link. GLEW, cglm, cimgui and JoltC
# still have unconditional install rules of their own that run under a
# consumer's cmake --install; GLFW's and Jolt's are switched off in the root
# CMakeLists. A working install has to export all of those targets with their
# headers and ship a config file, which is separate work.
