cmake_minimum_required(VERSION 3.10)
project(crazytaxi C)

set(CMAKE_C_STANDARD 11)

option(USE_GLES2 "Use GLESv2" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)


file(GLOB CORE_SRC ${CMAKE_SOURCE_DIR}/src/so_util.c
                   ${CMAKE_SOURCE_DIR}/src/egl_shim.c
                   ${CMAKE_SOURCE_DIR}/src/opensles_shim.c
                   ${CMAKE_SOURCE_DIR}/src/util.c
                   ${CMAKE_SOURCE_DIR}/src/error.c)


add_executable(crazytaxi
    src/main.c
    src/imports.c
    src/jni_shim.c
    ${CORE_SRC}
)

target_include_directories(crazytaxi PRIVATE ${SDL2_INCLUDE_DIRS} src)

if(USE_GLES2)
  set(GLES_LIB GLESv2)
else()
  set(GLES_LIB GLESv1_CM)
endif()

target_link_libraries(crazytaxi PRIVATE
    ${SDL2_LIBRARIES}
    ${GLES_LIB}
    EGL
    m
    dl
    pthread
    stdc++
)

target_compile_options(crazytaxi PRIVATE
    ${SDL2_CFLAGS_OTHER} -Wall -Wextra -Wno-unused-parameter)
