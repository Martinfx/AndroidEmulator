# Runs VmTest.main() on droidvm and compares the output with a real JVM's.
execute_process(COMMAND ${DROIDVM} ${DATA}/vmtest.dex --main VmTest
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
file(READ ${DATA}/VmTest.expected expected)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "droidvm exited with ${rc}\n${err}")
endif()
if(NOT out STREQUAL expected)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/VmTest.actual "${out}")
  message(FATAL_ERROR "output differs from the JVM, see VmTest.actual\n${err}")
endif()
