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

# Vendored cJSON (MIT): built without our -Werror policy.
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)
target_compile_options(cjson PRIVATE -w)

# The firmware's own protocol code: framing, CRC, op table, register names.
set(FW_SHARED ${FW}/apiframe.c ${FW}/crc32.c ${FW}/api_proto.c ${FW}/json.c)

set(LIB_SRC
    src/device.c src/binops.c src/recover.c src/jsonout.c src/transport_serial.c)
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

add_executable(ataboy cli/ataboy.c cli/human.c cli/bootcache.c)
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
