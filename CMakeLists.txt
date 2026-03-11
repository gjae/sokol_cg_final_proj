cmake_minimum_required(VERSION 3.10)
project(proyecto_final_cg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
  game.cpp
  sokol_impl.cpp
)

# Headers de sokol
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})

# Definiciones para sokol
target_compile_definitions(${PROJECT_NAME} PRIVATE
  SOKOL_GLCORE
)

# Dependencias de sistema para sokol en Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    GL
    X11
    Xi
    Xcursor
    dl
    m
  )
endif()
