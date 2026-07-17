cmake_minimum_required(VERSION 3.10)
project(showtasks)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(include)

set(SOURCES
    src/main.cpp
)

add_executable(showtasks ${SOURCES})
