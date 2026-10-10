add_executable(hbc_dump hbc-dump.cpp)
target_link_libraries(hbc_dump PRIVATE hbc)

add_executable(hbc_roundtrip hbc-roundtrip.cpp)
target_link_libraries(hbc_roundtrip PRIVATE hbc)

add_executable(hbc_section_map hbc-section-map.cpp)
target_link_libraries(hbc_section_map PRIVATE hbc)

add_executable(hbc_patch hbc-patch.cpp)
target_link_libraries(hbc_patch PRIVATE hbc)

add_executable(hbc_strings hbc-strings.cpp)
target_link_libraries(hbc_strings PRIVATE hbc)

add_executable(hbc_functions hbc-functions.cpp)
target_link_libraries(hbc_functions PRIVATE hbc)

add_executable(hbc_disasm hbc-disasm.cpp)
target_link_libraries(hbc_disasm PRIVATE hbc)