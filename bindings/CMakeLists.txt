pybind11_add_module(_libhbc bindings.cpp)
target_link_libraries(_libhbc PRIVATE hbc)
