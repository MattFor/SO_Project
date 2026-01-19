cmake_minimum_required(VERSION 4.0)
project(SO_Project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT UNIX OR APPLE)
    message(FATAL_ERROR "This project must be built on Linux (POSIX IPC required).")
endif()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DISABLE_WARNINGS "Disable all compiler warnings" OFF)

# Default to FAST_BUILD=ON (aggressive optimizations). User may override with -DFAST_BUILD=OFF
if (NOT DEFINED FAST_BUILD)
    set(FAST_BUILD ON CACHE BOOL "Enable aggressive runtime optimizations (Ofast, LTO, march=native)" FORCE)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if (FAST_BUILD)
        message(STATUS "FAST_BUILD=ON -> enabling aggressive optimization flags (Ofast, LTO, march=native)")
        # Core optimization flags for speed
        set(SPEED_FLAGS "-Ofast -march=native -funroll-loops -ffast-math -fno-math-errno -fomit-frame-pointer -fstrict-aliasing")
        # Enable LTO if available
        set(LTO_FLAGS "-flto")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SPEED_FLAGS} ${LTO_FLAGS} -DNDEBUG -pipe")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LTO_FLAGS}")
    else()
        message(STATUS "FAST_BUILD=OFF -> using safer -O2 (faster compile)")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g0 -pipe -DNDEBUG")
    endif()
else()
    message(STATUS "Unknown compiler: using default flags")
endif()

# POSIX libraries
set(POSIX_LIBS pthread rt)

# Project includes
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(master master.cpp)
add_executable(registration src/registration.cpp)
add_executable(triage src/triage.cpp)
add_executable(doctor src/doctor.cpp)
add_executable(patient src/patient.cpp)
add_executable(dispatcher src/dispatcher.cpp)

# Link POSIX libs
target_link_libraries(master PRIVATE ${POSIX_LIBS})
target_link_libraries(registration PRIVATE ${POSIX_LIBS})
target_link_libraries(triage PRIVATE ${POSIX_LIBS})
target_link_libraries(doctor PRIVATE ${POSIX_LIBS})
target_link_libraries(patient PRIVATE ${POSIX_LIBS})
target_link_libraries(dispatcher PRIVATE ${POSIX_LIBS})

# Set runtime output dir
set_target_properties(
        master registration triage doctor patient dispatcher
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Enable target-level IPO/LTO for supported toolchains when FAST_BUILD=ON
if (FAST_BUILD)
    foreach(tgt IN ITEMS master registration triage doctor patient dispatcher)
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(STATUS "Enabling target LTO/IPO for ${tgt}")
            set_target_properties(${tgt} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endforeach()
endif()

# Optionally disable warnings globally (keeps per-target control possible)
if (NOT DISABLE_WARNINGS)
    add_compile_options(
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wunused-variable
    )
endif()

# Helpful custom targets
add_custom_target(run
        COMMAND ${CMAKE_BINARY_DIR}/bin/master 30 3 50
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        DEPENDS master registration triage doctor patient dispatcher
        COMMENT "Running Project ER simulation"
)

add_custom_target(clean-ipc
        COMMAND rm -f /dev/mqueue/er_* || true
        COMMAND rm -f /dev/shm/er_* || true
        COMMENT "Cleaning POSIX message queues and shared memory"
)
