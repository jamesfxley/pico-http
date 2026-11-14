# SPDX-License-Identifier: MIT
# Fails when a function in the -fstack-usage report has a frame over LIMIT
# bytes (AR-13). GCC writes the report beside the object file. Run as:
# cmake -DOBJECT=<file.o> -DLIMIT=<bytes> -P <this>.
string(REGEX REPLACE "\\.o$" ".su" REPORT "${OBJECT}")
if(NOT EXISTS "${REPORT}")
    message(FATAL_ERROR "stack-usage report not found: ${REPORT}")
endif()

file(STRINGS "${REPORT}" LINES)
list(LENGTH LINES COUNT)
if(COUNT EQUAL 0)
    message(FATAL_ERROR "stack-usage report is empty: ${REPORT}")
endif()

set(OVER "")
foreach(LINE IN LISTS LINES)
    # <file>:<line>:<col>:<function>\t<bytes>\t<qualifier>
    string(REPLACE "\t" ";" FIELDS "${LINE}")
    list(LENGTH FIELDS FIELD_COUNT)
    if(NOT FIELD_COUNT EQUAL 3)
        message(FATAL_ERROR "unexpected report line: ${LINE}")
    endif()
    list(GET FIELDS 0 FUNCTION)
    list(GET FIELDS 1 BYTES)
    if(BYTES GREATER LIMIT)
        string(APPEND OVER "  ${BYTES} B  ${FUNCTION}\n")
    endif()
endforeach()

if(OVER)
    message(FATAL_ERROR
        "frames over ${LIMIT} B in ${REPORT}; an HTTPResponse is several KiB "
        "and must not be built on the callback stack:\n${OVER}")
endif()
message(STATUS "${COUNT} functions, none over ${LIMIT} B")
