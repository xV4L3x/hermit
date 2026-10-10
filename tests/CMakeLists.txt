find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found via find_package - fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

add_executable(hermit_tests
    unit/header/test_header.cpp
    unit/roundtrip/test_roundtrip.cpp
    unit/string-table/test_string_table.cpp
    unit/function-table/test_function_table.cpp
    unit/disassembler/test_disassembler.cpp
    unit/cfg/test_cfg.cpp
    unit/xrefs/test_xrefs.cpp
    unit/decompiler/test_decompiler.cpp
    unit/patcher/test_encoder.cpp
    unit/patcher/test_module.cpp
    unit/patcher/test_string_edit.cpp
    unit/patcher/test_literals.cpp
)

target_link_libraries(hermit_tests PRIVATE hbc Catch2::Catch2WithMain)
target_include_directories(hermit_tests PRIVATE unit)

# Inject corpus path so tests can find hello.hbc regardless of CWD
target_compile_definitions(hermit_tests PRIVATE
    HBC_CORPUS_DIR="${CMAKE_SOURCE_DIR}/seeder/corpus"
)

add_test(NAME hermit_tests COMMAND hermit_tests)
