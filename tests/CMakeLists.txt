# Each suite is one executable without an external test framework (see test_util.hpp).
foreach(name format memory opcodes expression decoder assembler roundtrip disassembler petscii image_io cpu c64 session monitor video reverse jumptable datatypes d64 t64 crt)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE m6510core)
  m6510_set_warnings(test_${name})
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Smoke test of the command line tool.
add_test(NAME cli_version COMMAND m6510tool version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "m6510tool [0-9]+\\.[0-9]+\\.[0-9]+")

# Example program: assemble -> disassemble (--asm) -> assemble again -> identical files; BASIC listing.
add_test(NAME cli_roundtrip
         COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:m6510tool> -DSRC=${PROJECT_SOURCE_DIR}/examples/hello.asm
                 -DCOMMENTS=${PROJECT_SOURCE_DIR}/examples/hello.comments.txt -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_roundtrip.cmake)

# The example agent session: every reply must be {"ok":true,...}.
add_test(NAME cli_serve
         COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:m6510tool> -DSESSION=${PROJECT_SOURCE_DIR}/examples/agent_session.jsonl
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_serve.cmake)

# The example monitor script (docs/monitor.md).
add_test(NAME cli_monitor
         COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:m6510tool> -DSCRIPT=${PROJECT_SOURCE_DIR}/examples/monitor_session.txt
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_monitor.cmake)

# The orchestrator tests need Python 3 (standard library only) and the m6510tool binary (docs/reverse-engineering.md).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME agent
           COMMAND ${CMAKE_COMMAND} -E env "M6510TOOL=$<TARGET_FILE:m6510tool>" ${Python3_EXECUTABLE} -W ignore -m unittest discover
                   -s ${CMAKE_CURRENT_SOURCE_DIR}/agent)
endif()
