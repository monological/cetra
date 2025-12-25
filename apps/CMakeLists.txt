# Cetra Example Applications

# Helper function to create an app target
function(add_cetra_app APP_NAME)
    file(GLOB APP_SOURCES src/*.c)
    add_executable(${APP_NAME} ${APP_SOURCES})

    target_link_libraries(${APP_NAME} PRIVATE cetra)

    target_include_directories(${APP_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}/include
    )

    target_compile_options(${APP_NAME} PRIVATE -Wall -g)

    if(APPLE)
        target_compile_definitions(${APP_NAME} PRIVATE GL_SILENCE_DEPRECATION)
    endif()
endfunction()

# Add each app subdirectory
add_subdirectory(render)
add_subdirectory(shapes)
add_subdirectory(pcb)
add_subdirectory(gametest)
add_subdirectory(tree)
add_subdirectory(splash)
