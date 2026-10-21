# The test data is prebuilt so the tests need no JDK:
#   data/vmtest.dex      tests/java/VmTest.java compiled with javac + dx
#   data/VmTest.expected output of the same program on a real JVM
#   data/ahoj.apk        samples/ahoj built with samples/build-apk.sh
# Regenerate them with tests/regenerate-data.sh after changing the sources.
set(DATA ${CMAKE_CURRENT_SOURCE_DIR}/data)

add_test(NAME vm_matches_jvm
  COMMAND ${CMAKE_COMMAND} -DDROIDVM=$<TARGET_FILE:droidvm> -DDATA=${DATA}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/vm_matches_jvm.cmake)

add_test(NAME ui_main_screen
  COMMAND ${CMAKE_COMMAND} -DDROIDVM=$<TARGET_FILE:droidvm> -DDATA=${DATA}
          -DOUT=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/ui_main_screen.cmake)

add_test(NAME ui_second_activity
  COMMAND ${CMAKE_COMMAND} -DDROIDVM=$<TARGET_FILE:droidvm> -DDATA=${DATA}
          -DOUT=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/ui_second_activity.cmake)
