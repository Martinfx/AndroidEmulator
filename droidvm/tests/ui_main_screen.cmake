# Taps "+1" twice, types a name, taps "Pozdravit" and checks the app's log.
execute_process(COMMAND ${DROIDVM} ${DATA}/ahoj.apk --headless --run 600
                        --tap 540,600@100 --tap 540,600@150
                        --tap 180,690@200 --type Martin@250 --tap 360,865@300
                        --screenshot ${OUT}/main_screen.bmp
                ERROR_VARIABLE log RESULT_VARIABLE rc)
foreach(expected "D/Ahoj: onCreate" "I/Ahoj: kliknutí: 2" "I/Ahoj: pozdrav: Ahoj, Martin!")
  string(FIND "${log}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing '${expected}' in the log:\n${log}")
  endif()
endforeach()
if(NOT rc EQUAL 0 OR NOT EXISTS ${OUT}/main_screen.bmp)
  message(FATAL_ERROR "exit code ${rc} or no screenshot\n${log}")
endif()
