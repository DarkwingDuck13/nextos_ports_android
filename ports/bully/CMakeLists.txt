cmake_minimum_required(VERSION 3.10)
project(BullyLoader C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


set(SRCS
    src/main.c
    src/so_util.c
    src/jni_shim.c
    src/imports.c
    src/egl_shim.c
    src/asset_archive.c
    src/zip_fs.c
    src/pthread_bridge.c
    src/util.c
    src/error.c
)

add_executable(bully ${SRCS})

target_include_directories(bully PRIVATE src)

target_compile_definitions(bully PRIVATE _GNU_SOURCE)

target_compile_options(bully PRIVATE
    -O2
    -Wno-unused-parameter
    -Wno-unused-function
)


target_link_libraries(bully PRIVATE
    SDL2
    EGL
    dl
    m
    pthread
)
