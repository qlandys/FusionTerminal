if(NOT DEFINED ICON_DIR OR "${ICON_DIR}" STREQUAL "")
  message(FATAL_ERROR "ICON_DIR is required")
endif()
string(REGEX REPLACE "^\"|\"$" "" ICON_DIR "${ICON_DIR}")

if(NOT DEFINED STAMP_FILE OR "${STAMP_FILE}" STREQUAL "")
  message(FATAL_ERROR "STAMP_FILE is required")
endif()
string(REGEX REPLACE "^\"|\"$" "" STAMP_FILE "${STAMP_FILE}")

if(NOT IS_DIRECTORY "${ICON_DIR}")
  message(WARNING "Icon directory missing: ${ICON_DIR}")
  return()
endif()

file(GLOB ICON_FILES
  "${ICON_DIR}/*.png"
  "${ICON_DIR}/*.ico"
  "${ICON_DIR}/*.webmanifest"
)

set(old "")
if(EXISTS "${STAMP_FILE}")
  file(READ "${STAMP_FILE}" old)
endif()

set(new "")
set(any_changed FALSE)

foreach(f IN LISTS ICON_FILES)
  if(NOT EXISTS "${f}")
    continue()
  endif()
  file(SHA256 "${f}" hash)
  string(APPEND new "${f}\t${hash}\n")
  if(old MATCHES "(^|[\r\n])${f}\t${hash}([\r\n]|$)")
    # unchanged
  else()
    set(any_changed TRUE)
  endif()
endforeach()

if(any_changed)
  foreach(f IN LISTS ICON_FILES)
    if(EXISTS "${f}")
      file(TOUCH "${f}")
    endif()
  endforeach()
  get_filename_component(stamp_dir "${STAMP_FILE}" DIRECTORY)
  if(NOT "${stamp_dir}" STREQUAL "" AND NOT EXISTS "${stamp_dir}")
    file(MAKE_DIRECTORY "${stamp_dir}")
  endif()
  file(WRITE "${STAMP_FILE}" "${new}")
  message(STATUS "Icon assets changed; refreshed timestamps for Windows resources.")
endif()
