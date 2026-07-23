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

# Add each app subdirectory
add_subdirectory(render)
add_subdirectory(shapes)
add_subdirectory(pcb)
add_subdirectory(gametest)
add_subdirectory(tree)
add_subdirectory(splash)
add_subdirectory(spores)
