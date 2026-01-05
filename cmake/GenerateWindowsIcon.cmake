if (NOT DEFINED OUTPUT_ICO OR OUTPUT_ICO STREQUAL "")
    message(FATAL_ERROR "GenerateWindowsIcon.cmake: OUTPUT_ICO not set")
endif ()

if (NOT DEFINED ICON_INPUT OR ICON_INPUT STREQUAL "")
    message(FATAL_ERROR "GenerateWindowsIcon.cmake: ICON_INPUT not set")
endif ()

set(_python "${PYTHON_EXE}")
if (NOT DEFINED _python OR _python STREQUAL "")
    set(_python python)
endif ()

get_filename_component(_out_dir "${OUTPUT_ICO}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")

set(_script "${CMAKE_CURRENT_LIST_DIR}/../tools/scripts/make_windows_ico.py")
if (NOT EXISTS "${_script}")
    message(FATAL_ERROR "GenerateWindowsIcon.cmake: missing script: ${_script}")
endif ()

execute_process(
    COMMAND "${_python}" "${_script}" "${ICON_INPUT}" "${OUTPUT_ICO}"
    RESULT_VARIABLE _res
    OUTPUT_QUIET
    ERROR_QUIET
)

if (_res EQUAL 0 AND EXISTS "${OUTPUT_ICO}")
    return()
endif ()

if (DEFINED FALLBACK_ICO AND NOT FALLBACK_ICO STREQUAL "" AND EXISTS "${FALLBACK_ICO}")
    message(WARNING "PlasmaTerminal: failed to generate Windows icon from ${ICON_INPUT}; using fallback ${FALLBACK_ICO}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FALLBACK_ICO}" "${OUTPUT_ICO}"
        RESULT_VARIABLE _copy_res
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if (_copy_res EQUAL 0 AND EXISTS "${OUTPUT_ICO}")
        return()
    endif ()
endif ()

message(WARNING "PlasmaTerminal: failed to generate Windows icon and no fallback could be copied; keeping existing icon if any.")
