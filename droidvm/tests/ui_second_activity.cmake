# Opens the drawing activity with an Intent extra, draws, and goes back.
execute_process(COMMAND ${DROIDVM} ${DATA}/ahoj.apk --headless --run 1200
                        --tap 540,600@100 --tap 360,960@200
                        --swipe 100,400,600,700@400 --tap 540,1220@800
                        --screenshot ${OUT}/after_back.bmp
                ERROR_VARIABLE log RESULT_VARIABLE rc)
foreach(expected "DrawActivity, kliknutí z intentu: 1" "I/Ahoj: zpět")
  string(FIND "${log}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing '${expected}' in the log:\n${log}")
  endif()
endforeach()
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "exit code ${rc}\n${log}")
endif()
