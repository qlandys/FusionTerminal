if (NOT DEFINED OUTPUT_RC OR OUTPUT_RC STREQUAL "")
    message(FATAL_ERROR "WriteGhostRc.cmake: OUTPUT_RC not set")
endif ()

if (NOT DEFINED ICON_PATH OR ICON_PATH STREQUAL "")
    message(FATAL_ERROR "WriteGhostRc.cmake: ICON_PATH not set")
endif ()

# Ensure a clean, minimal resource file.
file(WRITE "${OUTPUT_RC}" "IDI_APP_ICON ICON \"${ICON_PATH}\"\n")

