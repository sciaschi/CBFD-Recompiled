# Applies a patch to the source folder it's run in (FetchContent's PATCH_COMMAND), unless it's applied
# already (a configure that runs the patch step again).
#   cmake -DPATCH=<file> -P apply.cmake
execute_process(COMMAND git apply --reverse --check "${PATCH}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if (applied EQUAL 0)
    return()
endif()
execute_process(COMMAND git apply "${PATCH}" RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "couldn't apply ${PATCH}")
endif()
