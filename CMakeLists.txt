cmake_minimum_required(VERSION 3.10)
project(stream1090)

# ------------------------------------------------------------
# Build type
# ------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
    message(STATUS "[stream1090] Build type not specified: defaulting to Release")
endif()

# ------------------------------------------------------------
# Options
# ------------------------------------------------------------
option(ENABLE_STATS          "Compile with STATS_ENABLED" ON)
option(END_STATS             "Compile with STATS_END_ONLY" OFF)
option(ENABLE_CUSTOM_INPUT   "Compile with STREAM1090_CUSTOM_INPUT" OFF)
option(ENABLE_RSSI           "Compile with STREAM1090_ENABLE_RSSI" ON)
option(ENABLE_RTLSDR_BLOG    "Enable vendored RTL-SDR Blog fork" OFF)
option(ENABLE_TOO_MUCH_CPU   "Unlocks the 40 and 48 Msps speeds" OFF)
option(BUILD_TESTING         "Build tests" OFF)
option(ENABLE_NATIVE_ARCH    "Tune the binary for the CPU it is built on" ON)
option(ENABLE_LTO            "Link time optimization" ON)
option(ENABLE_UNROLL         "Ask the compiler to unroll the DSP loops" ON)
option(ENABLE_SIMD           "Use the hand written SIMD kernels (NEON/SSE)" ON)

# Explicit CPU to tune for. Only used when ENABLE_NATIVE_ARCH cannot be applied
# (cross compiling, or a compiler that does not understand -mcpu=native).
# Useful values on ARM: cortex-a76 (Pi 5), cortex-a72 (Pi 4), cortex-a53 (Pi 3 / Zero 2 W)
set(STREAM1090_CPU "" CACHE STRING "Explicit CPU for -mcpu=/-mtune= (e.g. cortex-a76)")

# Profile guided optimization: off | generate | use
set(ENABLE_PGO "off" CACHE STRING "Profile guided optimization: off, generate or use")
set_property(CACHE ENABLE_PGO PROPERTY STRINGS off generate use)

set(SIMD_DEF         STREAM1090_SIMD=$<BOOL:${ENABLE_SIMD}>)
set(STATS_DEF        STATS_ENABLED=$<BOOL:${ENABLE_STATS}>)
set(STATS_END_DEF    STATS_END_ONLY=$<BOOL:${END_STATS}>)
set(CUSTOM_INPUT_DEF STREAM1090_CUSTOM_INPUT=$<BOOL:${ENABLE_CUSTOM_INPUT}>)
set(RSSI_DEF         STREAM1090_RSSI=$<BOOL:${ENABLE_RSSI}>)
set(TOO_MUCH_CPU_DEF STREAM1090_TOO_MUCH_CPU=$<BOOL:${ENABLE_TOO_MUCH_CPU}>)

# ------------------------------------------------------------
# Compiler settings
# ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXCompilerFlag)

set(DEFAULT_COMPILE_OPTIONS
    -Wall -Wextra -Wunused-function -O3 -ffast-math
)
set(DEFAULT_LINK_OPTIONS "")

# Adds a flag to DEFAULT_COMPILE_OPTIONS if the compiler accepts it.
# Sets <resultVar> so the caller can react to a flag that did not make it.
function(stream1090_try_flag flag resultVar)
    string(MAKE_C_IDENTIFIER "HAVE${flag}" cacheVar)
    check_cxx_compiler_flag("${flag}" ${cacheVar})
    if(${cacheVar})
        set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS} ${flag} PARENT_SCOPE)
    endif()
    set(${resultVar} ${${cacheVar}} PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------
# CPU tuning
#
# The default aarch64 baseline (armv8-a, generic scheduling) leaves a good
# chunk of performance on the table on the boards this usually runs on.
# Tuning for the actual core mostly changes instruction selection and
# scheduling, which is exactly what the tight DSP loops care about.
# ------------------------------------------------------------
set(CPU_FLAG_APPLIED FALSE)

if(ENABLE_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm)")
        stream1090_try_flag(-mcpu=native CPU_FLAG_APPLIED)
    else()
        stream1090_try_flag(-march=native CPU_FLAG_APPLIED)
        if(CPU_FLAG_APPLIED)
            stream1090_try_flag(-mtune=native _unused)
        endif()
    endif()

    if(CPU_FLAG_APPLIED)
        message(STATUS "[stream1090] Tuning for the build host CPU")
    else()
        message(STATUS "[stream1090] Compiler rejects native tuning, falling back")
    endif()
endif()

# Either the user pinned a CPU, or native tuning was unavailable
if(NOT CPU_FLAG_APPLIED AND STREAM1090_CPU)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm)")
        stream1090_try_flag(-mcpu=${STREAM1090_CPU} CPU_FLAG_APPLIED)
    else()
        stream1090_try_flag(-mtune=${STREAM1090_CPU} CPU_FLAG_APPLIED)
    endif()
    if(CPU_FLAG_APPLIED)
        message(STATUS "[stream1090] Tuning for ${STREAM1090_CPU}")
    else()
        message(WARNING "[stream1090] Compiler does not accept CPU '${STREAM1090_CPU}'")
    endif()
endif()

if(NOT CPU_FLAG_APPLIED)
    message(STATUS "[stream1090] Generic CPU tuning. Consider -DSTREAM1090_CPU=cortex-a76 or similar")
endif()

# ------------------------------------------------------------
# Loop unrolling
#
# GCC does not enable -funroll-loops at -O3. The FIR and the resampler are
# short trip count loops over compile time constant tap tables, which is
# precisely the case where unrolling pays for itself.
# ------------------------------------------------------------
if(ENABLE_UNROLL)
    stream1090_try_flag(-funroll-loops _unused)
endif()

# ------------------------------------------------------------
# Link time optimization
# ------------------------------------------------------------
if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
    if(LTO_SUPPORTED)
        message(STATUS "[stream1090] Link time optimization enabled")
    else()
        message(STATUS "[stream1090] Link time optimization unavailable: ${LTO_ERROR}")
    endif()
else()
    set(LTO_SUPPORTED FALSE)
endif()

# ------------------------------------------------------------
# Profile guided optimization
#
# The demodulator is a wall of hard to predict branches, which is the case
# PGO helps with most. Workflow:
#   cmake ../ --fresh -DENABLE_PGO=generate && make
#   ./stream1090 -s 2.4 -u 8 -q < recording.iq > /dev/null
#   cmake ../ -DENABLE_PGO=use && make
# ------------------------------------------------------------
set(PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Where the PGO profile data lives")

if(ENABLE_PGO STREQUAL "generate")
    file(MAKE_DIRECTORY "${PGO_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND DEFAULT_COMPILE_OPTIONS -fprofile-generate=${PGO_DIR})
        list(APPEND DEFAULT_LINK_OPTIONS    -fprofile-generate=${PGO_DIR})
    else()
        list(APPEND DEFAULT_COMPILE_OPTIONS -fprofile-generate -fprofile-dir=${PGO_DIR})
        list(APPEND DEFAULT_LINK_OPTIONS    -fprofile-generate -fprofile-dir=${PGO_DIR})
    endif()
    message(STATUS "[stream1090] PGO instrumentation build, profile goes to ${PGO_DIR}")

elseif(ENABLE_PGO STREQUAL "use")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # The instrumented run drops raw counter files. Clang cannot read those
        # directly, they have to be indexed into a .profdata first.
        set(PGO_PROFILE "${PGO_DIR}/default.profdata")
        file(GLOB PGO_RAW_FILES "${PGO_DIR}/*.profraw")

        if(PGO_RAW_FILES)
            find_program(LLVM_PROFDATA
                NAMES llvm-profdata
                      llvm-profdata-21 llvm-profdata-20 llvm-profdata-19
                      llvm-profdata-18 llvm-profdata-17 llvm-profdata-16
                      llvm-profdata-15 llvm-profdata-14)

            if(NOT LLVM_PROFDATA)
                message(FATAL_ERROR
                    "[stream1090] Found raw profiles in ${PGO_DIR} but no llvm-profdata to index them.\n"
                    "             Install it (apt install llvm) or build the PGO step with GCC.")
            endif()

            execute_process(
                COMMAND ${LLVM_PROFDATA} merge -output=${PGO_PROFILE} ${PGO_RAW_FILES}
                RESULT_VARIABLE PGO_MERGE_RESULT
                ERROR_VARIABLE  PGO_MERGE_ERROR)

            if(NOT PGO_MERGE_RESULT EQUAL 0)
                message(FATAL_ERROR "[stream1090] llvm-profdata merge failed: ${PGO_MERGE_ERROR}")
            endif()

            message(STATUS "[stream1090] Indexed the recorded profile into ${PGO_PROFILE}")
        endif()

        if(NOT EXISTS "${PGO_PROFILE}")
            message(FATAL_ERROR
                "[stream1090] No profile found in ${PGO_DIR}.\n"
                "             Build with -DENABLE_PGO=generate and run stream1090 on a recording first.")
        endif()

        list(APPEND DEFAULT_COMPILE_OPTIONS
            -fprofile-use=${PGO_PROFILE}
            # a profile recorded before the last edit is still worth using
            -Wno-profile-instr-out-of-date
            -Wno-profile-instr-unprofiled)

        message(STATUS "[stream1090] PGO optimized build, reading profile from ${PGO_PROFILE}")
    else()
        if(NOT EXISTS "${PGO_DIR}")
            message(FATAL_ERROR
                "[stream1090] No profile found in ${PGO_DIR}.\n"
                "             Build with -DENABLE_PGO=generate and run stream1090 on a recording first.")
        endif()

        list(APPEND DEFAULT_COMPILE_OPTIONS
            -fprofile-use -fprofile-dir=${PGO_DIR} -fprofile-correction -Wno-missing-profile)

        message(STATUS "[stream1090] PGO optimized build, reading profile from ${PGO_DIR}")
    endif()

elseif(NOT ENABLE_PGO STREQUAL "off")
    message(FATAL_ERROR "[stream1090] ENABLE_PGO must be off, generate or use")
endif()

# ------------------------------------------------------------
# Device backend detection
# ------------------------------------------------------------
find_package(PkgConfig REQUIRED)

set(DEVICE_SOURCES "")
set(DEVICE_LIBS "")
set(DEVICE_INCLUDE_DIRS "")
set(DEVICE_DEFINITIONS "")

# --- Airspy --------------------------------------------------
pkg_check_modules(AIRSPY QUIET libairspy)
if(AIRSPY_FOUND)
    message(STATUS "[stream1090] Airspy support enabled")
    list(APPEND DEVICE_SOURCES       src/devices/AirspyDevice.cpp)
    list(APPEND DEVICE_LIBS          ${AIRSPY_LDFLAGS})
    list(APPEND DEVICE_INCLUDE_DIRS  ${AIRSPY_INCLUDE_DIRS})
    list(APPEND DEVICE_DEFINITIONS   STREAM1090_HAVE_AIRSPY)
else()
    message(STATUS "[stream1090] Airspy support disabled (libairspy not found)")
endif()

# ------------------------------------------------------------
# RTL-SDR backend
# ------------------------------------------------------------
if(ENABLE_RTLSDR_BLOG)
    message(STATUS "[stream1090] Using vendored RTL-SDR Blog fork")

    set(DETACH_KERNEL_DRIVER ON CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/rtl-sdr-blog)

    list(APPEND DEVICE_SOURCES       src/devices/RtlSdrDevice.cpp)
    list(APPEND DEVICE_LIBS          rtlsdr)
    list(APPEND DEVICE_INCLUDE_DIRS  thirdparty/rtl-sdr-blog/include)
    list(APPEND DEVICE_DEFINITIONS   STREAM1090_HAVE_RTLSDR)
    list(APPEND DEVICE_DEFINITIONS   STREAM1090_HAVE_RTLSDR_BLOG)

else()
    message(STATUS "[stream1090] Trying system librtlsdr...")

    find_package(PkgConfig)
    pkg_check_modules(RTLSDR QUIET librtlsdr)

    if(RTLSDR_FOUND)
        message(STATUS "[stream1090] System librtlsdr found")

        list(APPEND DEVICE_SOURCES       src/devices/RtlSdrDevice.cpp)
        list(APPEND DEVICE_LIBS          ${RTLSDR_LDFLAGS})
        list(APPEND DEVICE_INCLUDE_DIRS  ${RTLSDR_INCLUDE_DIRS})
        list(APPEND DEVICE_DEFINITIONS   STREAM1090_HAVE_RTLSDR)

    else()
        message(STATUS "[stream1090] RTL-SDR support disabled (no Blog fork, no system librtlsdr)")
    endif()
endif()


# ------------------------------------------------------------
# Feature flags
# ------------------------------------------------------------
if (ENABLE_RSSI)
    message(STATUS "[stream1090] RSSI output enabled")
else()
    message(STATUS "[stream1090] RSSI output disabled")
endif()

if (ENABLE_CUSTOM_INPUT)
    message(STATUS "[stream1090] Custom input mode enabled")
endif()

# ------------------------------------------------------------
# Core sources
# ------------------------------------------------------------
file(GLOB STREAM1090_CORE src/*.cpp)

# ------------------------------------------------------------
# Main executable
# ------------------------------------------------------------
add_executable(stream1090
    main.cpp
    ${STREAM1090_CORE}
    ${DEVICE_SOURCES}
)

target_include_directories(stream1090 PRIVATE
    include
    ${DEVICE_INCLUDE_DIRS}
)

target_compile_options(stream1090 PRIVATE ${DEFAULT_COMPILE_OPTIONS})

if(DEFAULT_LINK_OPTIONS)
    target_link_libraries(stream1090 PRIVATE ${DEFAULT_LINK_OPTIONS})
endif()

if(LTO_SUPPORTED)
    set_property(TARGET stream1090 PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_compile_definitions(stream1090 PRIVATE
    ${SIMD_DEF}
    ${STATS_DEF}
    ${STATS_END_DEF}
    ${CUSTOM_INPUT_DEF}
    ${RSSI_DEF}
    ${TOO_MUCH_CPU_DEF}
    ${DEVICE_DEFINITIONS}
)

target_link_libraries(stream1090 PRIVATE ${DEVICE_LIBS})

include(CTest)
if(BUILD_TESTING)
    add_executable(crc_error_table_test tests/CRCErrorTableTest.cpp)
    target_include_directories(crc_error_table_test PRIVATE include)
    target_compile_options(crc_error_table_test PRIVATE ${DEFAULT_COMPILE_OPTIONS})
    add_test(NAME crc_error_table_test COMMAND crc_error_table_test)

    add_executable(icao_cache_test tests/ICAOCacheTest.cpp)
    target_include_directories(icao_cache_test PRIVATE include)
    target_compile_options(icao_cache_test PRIVATE ${DEFAULT_COMPILE_OPTIONS})
    add_test(NAME icao_cache_test COMMAND icao_cache_test)

    add_executable(df11_interrogator_test tests/DF11InterrogatorTest.cpp)
    target_include_directories(df11_interrogator_test PRIVATE include)
    target_compile_options(df11_interrogator_test PRIVATE ${DEFAULT_COMPILE_OPTIONS})
    add_test(NAME df11_interrogator_test COMMAND df11_interrogator_test)

    add_executable(ring_buffer_test tests/RingBufferTest.cpp)
    target_include_directories(ring_buffer_test PRIVATE include)
    target_compile_options(ring_buffer_test PRIVATE ${DEFAULT_COMPILE_OPTIONS})
    add_test(NAME ring_buffer_test COMMAND ring_buffer_test)

    add_executable(erasure_repair_test tests/ErasureRepairTest.cpp)
    target_include_directories(erasure_repair_test PRIVATE include)
    target_compile_options(erasure_repair_test PRIVATE ${DEFAULT_COMPILE_OPTIONS})
    add_test(NAME erasure_repair_test COMMAND erasure_repair_test)
endif()

# ------------------------------------------------------------
# table_gen (excluded from all)
# ------------------------------------------------------------
add_executable(table_gen table_gen.cpp)
target_include_directories(table_gen PRIVATE include)
target_compile_options(table_gen PRIVATE ${DEFAULT_COMPILE_OPTIONS})
if(DEFAULT_LINK_OPTIONS)
    target_link_libraries(table_gen PRIVATE ${DEFAULT_LINK_OPTIONS})
endif()
set_target_properties(table_gen PROPERTIES EXCLUDE_FROM_ALL TRUE)
