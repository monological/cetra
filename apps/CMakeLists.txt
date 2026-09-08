# Cetra Example Applications

# Helper function to create an app target
function(add_cetra_app APP_NAME)
    file(GLOB APP_SOURCES src/*.c)
    add_executable(${APP_NAME} ${APP_SOURCES})

    # The cetra/ include tree and GL_SILENCE_DEPRECATION both come
    # transitively from cetra. The internal tree does not: it is private to
    # the library, and an in-tree app reaches it only through this line, so
    # #include "cetra/internal/x.h" in an app is a declaration that the app
    # is a debug harness for the engine. A project outside the tree cannot
    # write that line at all.
    target_link_libraries(${APP_NAME} PRIVATE cetra)
    target_link_libraries(${APP_NAME} PRIVATE cetra_warnings)
    get_target_property(_internal_dir cetra CETRA_INTERNAL_INCLUDE_DIR)
    target_include_directories(${APP_NAME} PRIVATE ${_internal_dir})
endfunction()

# Every subdirectory holding a CMakeLists.txt is an app. DISCOVERED rather than
# listed, for two reasons.
#
# A private app can be kept out of the repository with an apps/.gitignore
# entry, and is then absent from a clone -- and add_subdirectory on a missing
# directory is a CONFIGURE error, not a skipped target. That fails for everyone
# who does not have the app and for nobody who does, so whoever adds it cannot
# see the breakage: a fresh clone of this repository would not build at all.
# (An app of its own is better off as its own project with cetra as a
# submodule; README.md, "Using cetra in your project".)
#
# And a name written here is public even when the code behind it is not, which
# defeats the point of ignoring it.
#
# CONFIGURE_DEPENDS so adding an app re-runs cmake instead of being silently
# missed until the next clean configure, matching the shader glob in
# cetra/CMakeLists.txt. Globbing the MANIFEST rather than the directory also
# skips an empty leftover directory, which would otherwise pass a plain
# IS_DIRECTORY test and then fail inside add_subdirectory anyway.
file(GLOB CETRA_APP_MANIFESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt)
foreach(manifest ${CETRA_APP_MANIFESTS})
    get_filename_component(app_dir ${manifest} DIRECTORY)
    get_filename_component(app_name ${app_dir} NAME)
    add_subdirectory(${app_name})
endforeach()
