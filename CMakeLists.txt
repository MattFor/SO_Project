cmake_minimum_required(VERSION 4.0)
project(SO_Project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)

# Headers
include_directories(include)

add_executable(SO_Project
        main.cpp
        src/logger/Logger.cpp
        src/logger/LoggerService.cpp
        include/logger/LoggerService.h
        src/patient/Patient.cpp
        include/patient/Patient.h
        src/patient/PatientGenerator.cpp
        include/patient/PatientGenerator.h
)

# Windows networking support
if (WIN32)
    target_compile_definitions(SO_Project PRIVATE WIN32_LEAN_AND_MEAN)
    target_link_libraries(SO_Project PRIVATE ws2_32)
endif()