# Writes git_version.h with the current `git describe`, at build time, and
# only when it changed (so an unchanged hash doesn't force a rebuild).
#   cmake -DGIT=<git> -DSRC=<source dir> -DOUT=<header> -P git_version.cmake
set(hash "unknown")
if (GIT)
    execute_process(COMMAND ${GIT} describe --always --dirty --abbrev=10
                    WORKING_DIRECTORY ${SRC}
                    OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE rc)
    if (rc EQUAL 0 AND out)
        set(hash "${out}")
    endif()
endif()
set(content "#define FW_GIT_HASH \"${hash}\"\n")
set(old "")
if (EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if (NOT old STREQUAL content)
    file(WRITE ${OUT} "${content}")
endif()
