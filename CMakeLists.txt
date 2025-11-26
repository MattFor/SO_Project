cmake_minimum_required(VERSION 4.0)
project(SO_Project)

set(CMAKE_CXX_STANDARD 26)

# katalog z plikami nagłówkowymi
include_directories(include)

add_executable(SO_Project
        main.cpp
        src/Logger.cpp
)
