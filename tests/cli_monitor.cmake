# Runs examples/monitor_session.txt through m6510tool monitor and checks the important lines of the output.
execute_process(COMMAND ${TOOL} monitor INPUT_FILE ${SCRIPT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "monitor failed (${rc}): ${err}")
endif()
if(out MATCHES "\n\\? ")
  message(FATAL_ERROR "a command failed:\n${out}")
endif()
foreach(expected
    "  C008  D0 FA     BNE loop"
    "stopped: breakpoint at C004  \\[2 instructions, 4 cycles\\]"
    "stopped: reached C00D"
    "0400  0F "
    "stopped: program returned to BASIC"
    "HELLO, C64"
    "CHROUT  A=48"
    "D020 <- [0-9A-F][0-9A-F]  VIC EXTCOL")
  if(NOT out MATCHES "${expected}")
    message(FATAL_ERROR "missing in the output: ${expected}\n${out}")
  endif()
endforeach()
