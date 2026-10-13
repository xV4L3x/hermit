add_library(hbc STATIC
    src/parser/parser.cpp
    src/parser/string_table.cpp
    src/parser/function_table.cpp
    src/parser/string_kind.cpp
    src/parser/exception_table.cpp
    src/parser/function_source_table.cpp
    src/parser/cjs_module_table.cpp
    src/format/opcodes.cpp
    src/format/opcodes_v94.cpp
    src/format/opcodes_v95.cpp
    src/format/opcodes_v96.cpp
    src/disasm/disasm.cpp
    src/cfg/cfg.cpp
    src/analysis/analysis.cpp
    src/decompiler/decompiler.cpp
    src/patcher/patcher.cpp
    src/patcher/module.cpp
    src/util/sha1.cpp
)

target_include_directories(hbc
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# libhbc is linked into the Python module (a shared object), so it must be
# position-independent. Not needed on macOS, required on Linux.
set_target_properties(hbc PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_options(hbc PRIVATE -Wall -Wextra -Wpedantic)
