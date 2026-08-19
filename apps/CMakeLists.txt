# Cetra Example Applications

# Helper function to create an app target
function(add_cetra_app APP_NAME)
    file(GLOB APP_SOURCES src/*.c)
    add_executable(${APP_NAME} ${APP_SOURCES})

    target_link_libraries(${APP_NAME} PRIVATE cetra)
    target_link_libraries(${APP_NAME} PRIVATE cetra_warnings)
    # The cetra/ include tree (forwarders + shader_strings.h) and
    # GL_SILENCE_DEPRECATION both arrive transitively from cetra. An app source
    # includes "cetra/shader_strings.h" directly, so depend on its generation.
    add_dependencies(${APP_NAME} cetra_shaders)
endfunction()

# Every subdirectory holding a CMakeLists.txt is an app. DISCOVERED rather than
# listed, for two reasons.
#
# A private app is absent from a clone -- apps/.gitignore keeps one out of the
# repository deliberately -- and add_subdirectory on a missing directory is a
# CONFIGURE error, not a skipped target. That fails for everyone who does not
# have the app and for nobody who does, so whoever adds it cannot see the
# breakage: a fresh clone of this repository would not build at all.
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
