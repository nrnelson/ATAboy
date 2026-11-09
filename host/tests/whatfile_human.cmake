# ctest helper: run "ataboy whatfile IMG <every LBA>" in the corpus directory
# and compare stdout with the Python tool's output (tests/whatfile/IMG.txt).
#   cmake -DEXE=ataboy -DIMG=x.img -DDIR=host/tests -P whatfile_human.cmake
set(work ${DIR}/corpus/fuzz_fat)
file(SIZE ${work}/${IMG} size)
math(EXPR last "${size} / 512 + 1")
set(lbas "")
foreach(i RANGE 0 ${last})
    list(APPEND lbas ${i})
endforeach()
list(APPEND lbas 100000 8589934592 -1)          # same extras as gen_seeds.py --golden
execute_process(COMMAND ${EXE} whatfile ${IMG} ${lbas} WORKING_DIRECTORY ${work}
                OUTPUT_VARIABLE got RESULT_VARIABLE rc)
file(READ ${DIR}/whatfile/${IMG}.txt want)
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "ataboy whatfile exited with ${rc}")
endif()
if (NOT got STREQUAL want)
    file(WRITE ${IMG}.got.txt "${got}")
    message(FATAL_ERROR "human output differs from tests/whatfile/${IMG}.txt (see ${IMG}.got.txt)")
endif()
