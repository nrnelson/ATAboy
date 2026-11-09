# libataboy + the ataboy CLI (host side of the ATAboy automation API).
#
#   cmake -S host -B host/build -G Ninja && ninja -C host/build
#   ctest --test-dir host/build
#
# Options: -DATABOY_SANITIZE=ON  (AddressSanitizer + UndefinedBehaviorSanitizer)
#          -DATABOY_FUZZ=ON      (libFuzzer targets; needs clang)
#          -DATABOY_STATIC=ON    (fully static executables)
# Windows: cross-compile with -DCMAKE_TOOLCHAIN_FILE=host/cmake/mingw-w64.cmake
cmake_minimum_required(VERSION 3.16)
project(ataboy_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ATABOY_SANITIZE "Build with ASan + UBSan" OFF)
option(ATABOY_FUZZ "Build libFuzzer targets (clang)" OFF)
option(ATABOY_STATIC "Link executables statically" OFF)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../firmware)

if (MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-unused-parameter)
    # snprintf truncation into fixed buffers is deliberate (bounded output).
    # Probe the positive form: compilers ignore or warn about unknown -Wno-*.
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-Wformat-truncation ATABOY_HAVE_WFORMAT_TRUNCATION)
    if (ATABOY_HAVE_WFORMAT_TRUNCATION)
        add_compile_options(-Wno-format-truncation)
    endif()
endif()
if (NOT WIN32)
    add_compile_definitions(_DEFAULT_SOURCE _DARWIN_C_SOURCE _FILE_OFFSET_BITS=64)
elseif (MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)    # C99 printf (%zu, %lld)
endif()
if (ATABOY_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()
if (ATABOY_FUZZ)
    if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ATABOY_FUZZ needs clang (libFuzzer): -DCMAKE_C_COMPILER=clang")
    endif()
    # Coverage instrumentation everywhere; only the fuzz targets link libFuzzer.
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Vendored cJSON (MIT): built without our -Werror policy.
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)
target_compile_options(cjson PRIVATE -w)

# The firmware's own protocol code: framing, CRC, op table, register names.
set(FW_SHARED ${FW}/apiframe.c ${FW}/crc32.c ${FW}/api_proto.c ${FW}/json.c)

set(LIB_SRC
    src/device.c src/binops.c src/recover.c src/jsonout.c src/transport_serial.c
    src/mapfile.c src/imaging.c src/hashes.c src/sha256.c)
if (WIN32)
    list(APPEND LIB_SRC src/plat_win.c src/discover_win.c)
elseif (APPLE)
    list(APPEND LIB_SRC src/plat_posix.c src/discover_mac.c)
else()
    list(APPEND LIB_SRC src/plat_posix.c src/discover_linux.c src/transport_sgio.c)
endif()

add_library(ataboy_lib STATIC ${LIB_SRC} ${FW_SHARED})
set_target_properties(ataboy_lib PROPERTIES OUTPUT_NAME ataboy)
target_include_directories(ataboy_lib PUBLIC include PRIVATE src ${FW})
target_link_libraries(ataboy_lib PUBLIC cjson)
if (WIN32)
    target_link_libraries(ataboy_lib PUBLIC setupapi cfgmgr32)
    target_compile_definitions(ataboy_lib PRIVATE _WIN32_WINNT=0x0A00)
else()
    target_link_libraries(ataboy_lib PUBLIC m)
endif()
if (APPLE)
    target_link_libraries(ataboy_lib PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

add_executable(ataboy cli/ataboy.c cli/human.c cli/bootcache.c cli/human_recover.c)
target_include_directories(ataboy PRIVATE ${FW} src)
target_link_libraries(ataboy PRIVATE ataboy_lib)
if (ATABOY_STATIC)
    target_link_options(ataboy PRIVATE -static)
endif()

enable_testing()
add_executable(test_lib tests/test_lib.c)
target_include_directories(test_lib PRIVATE src ${FW})
target_link_libraries(test_lib PRIVATE ataboy_lib)
add_test(NAME test_lib COMMAND test_lib)

# Mapfiles, SHA-256, HASHES pages, image and rescue against a fake device.
foreach(t test_mapfile test_sha256 test_recover)
    add_executable(${t} tests/${t}.c tests/fake_dev.c)
    target_include_directories(${t} PRIVATE src ${FW})
    target_link_libraries(${t} PRIVATE ataboy_lib)
    add_test(NAME ${t} COMMAND ${t})
endforeach()


# Request engine against a model of the firmware's api_dispatch().
add_executable(test_engine tests/test_engine.c tests/fakefw.c ${FW}/rid_cache.c)
target_include_directories(test_engine PRIVATE src ${FW})
target_link_libraries(test_engine PRIVATE ataboy_lib)
add_test(NAME test_engine COMMAND test_engine)

# Serial transport: device.c + transport_serial.c linked without the platform
# layer, which the test replaces with a fake clock and serial port.
add_executable(test_serial tests/test_serial.c tests/fakefw.c src/device.c src/transport_serial.c
               ${FW_SHARED} ${FW}/rid_cache.c)
target_include_directories(test_serial PRIVATE include src ${FW})
target_link_libraries(test_serial PRIVATE cjson)
if (NOT WIN32)
    target_link_libraries(test_serial PRIVATE m)
endif()
add_test(NAME test_serial COMMAND test_serial)

# Fuzz targets: every tests/fuzz_<name>.c becomes one, automatically.
#
# Convention for fuzz_<name>.c: define only
#     int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
# (no main(), no LLVMFuzzerInitialize); it is linked against ataboy_lib with
# include/, src/ and firmware/ on the include path.  Seed inputs, one file
# each, go in tests/corpus/fuzz_<name>/.
#  - ATABOY_FUZZ=ON (clang): fuzz_<name> is a libFuzzer binary, e.g.
#        build/fuzz_<name> <copy of tests/corpus/fuzz_<name>> -max_total_time=60
#    and ctest runs it once over the seeds (-runs=0).
#  - Otherwise fuzz_<name> is linked with tests/fuzz_replay_main.c, a main()
#    that feeds it each file named on the command line (every file of a
#    directory), and ctest replays the seeds through it, so every compiler
#    and sanitizer build exercises them.
file(GLOB FUZZ_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tests/fuzz_*.c)
list(REMOVE_ITEM FUZZ_SOURCES ${CMAKE_CURRENT_LIST_DIR}/tests/fuzz_replay_main.c)
foreach (src IN LISTS FUZZ_SOURCES)
    get_filename_component(name ${src} NAME_WE)
    set(corpus ${CMAKE_CURRENT_LIST_DIR}/tests/corpus/${name})
    file(GLOB seed_files CONFIGURE_DEPENDS ${corpus}/*)   # reconfigure when seeds come or go
    set(seeds "")
    if (seed_files)
        set(seeds ${corpus})
    endif()
    if (ATABOY_FUZZ)
        add_executable(${name} ${src})
        target_link_options(${name} PRIVATE -fsanitize=fuzzer)
        add_test(NAME ${name}_corpus COMMAND ${name} -runs=0 ${seeds})
    else()
        add_executable(${name} ${src} tests/fuzz_replay_main.c)
        add_test(NAME ${name}_corpus COMMAND ${name} ${seeds})
    endif()
    target_include_directories(${name} PRIVATE src ${FW})
    target_link_libraries(${name} PRIVATE ataboy_lib)
endforeach()
