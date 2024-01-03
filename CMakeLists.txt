cmake_minimum_required(VERSION 3.20)

project(oprle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(lodepng_srcs
    "lodepng/lodepng.cpp"
)

set(srcs 
    "src/op6tlogo.cpp"
)

add_executable(${PROJECT_NAME} ${srcs} ${lodepng_srcs})
message("${PROJECT_SOURCE_DIR}/include")
target_include_directories(${PROJECT_NAME} PRIVATE 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/lodepng"
)
target_compile_options(${PROJECT_NAME} PRIVATE 
    "-Os"
    "-Wall" 
    "-Wextra" 
    "-fno-rtti" 
    "-fno-exceptions"
)
target_link_options(${PROJECT_NAME} PRIVATE
    "-s"
    "-static"
)