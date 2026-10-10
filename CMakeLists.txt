cmake_minimum_required(VERSION 3.20)
project(hermit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# pybind11 via FetchContent - version-pinned and reproducible, no submodule to
# manage. Sources are cached under build/_deps after the first configure, so the
# network is only hit once. Bump GIT_TAG to upgrade.
include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pybind11)

# Core library
add_subdirectory(core)

# Python bindings
add_subdirectory(bindings)

option(HERMIT_BUILD_TOOLS "Build the standalone command-line tools (hbc_dump, hbc_disasm, ...)" ON)
option(HERMIT_BUILD_TESTS "Build the Catch2 unit tests" ON)

if(HERMIT_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(HERMIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
