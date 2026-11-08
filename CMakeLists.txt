cmake_minimum_required(VERSION 3.16)

project(M6510Tools
        VERSION 0.1.0
        DESCRIPTION "MOS 6510 (Commodore 64) assembler, disassembler, emulator and machine code monitor"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(M6510_BUILD_TESTS "Build the unit tests" ON)
option(M6510_BUILD_TOOLS "Build developer tools (cross-check helpers)" OFF)
option(M6510_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(M6510_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

function(m6510_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(M6510_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(M6510_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(M6510_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# ---- core library ---------------------------------------------------------------------
# New components (opcode table, decoder, assembler, cpu, c64, ...) are added to this list as
# they are written; see docs/architecture.md for the planned layout.
add_library(m6510core STATIC
  src/assembler.cpp
  src/c64.cpp
  src/cpu.cpp
  src/decoder.cpp
  src/disassembler.cpp
  src/encoder.cpp
  src/expression.cpp
  src/format.cpp
  src/image_io.cpp
  src/json.cpp
  src/kernal.cpp
  src/opcodes.cpp
  src/petscii.cpp
  src/session.cpp
  src/snapshot.cpp)
target_include_directories(m6510core PUBLIC include)
target_compile_features(m6510core PUBLIC cxx_std_20)
m6510_set_warnings(m6510core)

# ---- command line tool ----------------------------------------------------------------
add_executable(m6510tool app/main.cpp)
target_link_libraries(m6510tool PRIVATE m6510core)
m6510_set_warnings(m6510tool)

install(TARGETS m6510tool RUNTIME DESTINATION bin)

# ---- developer tools ------------------------------------------------------------------
# Cross-check helpers (see docs/testing.md); later also the Harte/Dormann runners and the reference diff.
if(M6510_BUILD_TOOLS)
  add_executable(m6510_dump_forms tools/dump_forms.cpp)
  target_link_libraries(m6510_dump_forms PRIVATE m6510core)
  m6510_set_warnings(m6510_dump_forms)

  add_executable(m6510_harte tools/harte_runner.cpp)
  target_link_libraries(m6510_harte PRIVATE m6510core)
  m6510_set_warnings(m6510_harte)

  add_executable(m6510_dormann tools/dormann_runner.cpp)
  target_link_libraries(m6510_dormann PRIVATE m6510core)
  m6510_set_warnings(m6510_dormann)
endif()

# ---- tests ----------------------------------------------------------------------------
if(M6510_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
