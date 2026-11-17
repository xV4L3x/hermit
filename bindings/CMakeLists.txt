pybind11_add_module(_libhbc bindings.cpp)
target_link_libraries(_libhbc PRIVATE hbc)

# Ship the type stub next to the built module so editors resolve it when
# build/bindings is on PYTHONPATH. Regenerate it with bindings/genstub.py after
# changing the bindings.
add_custom_command(TARGET _libhbc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/_libhbc.pyi
            $<TARGET_FILE_DIR:_libhbc>/_libhbc.pyi
    VERBATIM)

# Install rule for the `libhbc` wheel (scikit-build-core sets SKBUILD). A plain
# `cmake -B build` leaves this off and keeps running the module straight from
# the build tree via PYTHONPATH. Everything lands at the wheel root so the
# extension imports as the top-level `_libhbc`.
if(DEFINED SKBUILD)
    install(TARGETS _libhbc LIBRARY DESTINATION .)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/_libhbc.pyi DESTINATION .)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/py.typed DESTINATION .)
endif()
