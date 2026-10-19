cmake_minimum_required(VERSION 3.25)
project(vtoast_reborn LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
include(cmake/VtFFmpeg.cmake)
add_subdirectory(engine)
add_subdirectory(media)
add_subdirectory(cli)
add_subdirectory(tests)
