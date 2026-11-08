# Each suite is one executable without an external test framework (see test_util.hpp).
foreach(name format memory)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE m6510core)
  m6510_set_warnings(test_${name})
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Smoke test of the command line tool.
add_test(NAME cli_version COMMAND m6510tool version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "m6510tool [0-9]+\\.[0-9]+\\.[0-9]+")
