cmake_minimum_required(VERSION 3.20)
project(hermit VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# pybind11: prefer one already provided (pip/Homebrew/system) so a sandboxed
# build with no network - Homebrew's clean room, cibuildwheel's isolated build
# env - still configures. A plain `cmake -B build` finds none and falls back to
# FetchContent, version-pinned and cached under build/_deps after the first
# configure. Bump GIT_TAG to upgrade.
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# Core library
add_subdirectory(core)

# Python bindings
add_subdirectory(bindings)

option(HERMIT_BUILD_TESTS "Build the Catch2 unit tests" ON)

if(HERMIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
