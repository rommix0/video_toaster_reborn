cmake_minimum_required(VERSION 3.25)
project(vtoast_reborn LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
add_subdirectory(engine)
add_subdirectory(tests)
