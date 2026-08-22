cmake_minimum_required(VERSION 3.10)
project(stream1090 VERSION 1.0.0)

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
option(ENABLE_PREAMBLE_GATE  "Reject address-parity frames that are weak and preamble-less" ON)
set(PREAMBLE_GATE_THRESHOLD 1.2 CACHE STRING "Preamble score a real transmission must reach")
set(MIN_SNR               2.5 CACHE STRING "Reject address-parity frames below this signal-to-noise ratio (0 disables; a ratio, so it transfers between receivers)")
option(ENABLE_ORBGRAND "Enable reliability-ordered last-resort DF17 repair" OFF)
set(ORBGRAND_GUESSES "1024" CACHE STRING "Hard cap on ORBGRAND error-pattern guesses per frame")
option(ENABLE_RTLSDR_BLOG    "Enable vendored RTL-SDR Blog fork" OFF)
option(ENABLE_TOO_MUCH_CPU   "Unlocks the 40 and 48 Msps speeds" OFF)
option(BUILD_TESTING         "Build tests" OFF)
set(INTERPOLATION_KERNEL "6" CACHE STRING "RTL-SDR interpolation kernel (1 linear, 6 sharpening cubic)")
option(ENABLE_NATIVE_ARCH "Tune the binary for the CPU it is built on" ON)
option(ENABLE_LTO         "Link time optimization" ON)
option(ENABLE_UNROLL      "Ask the compiler to unroll the DSP loops" ON)
set(ENABLE_PGO "off" CACHE STRING "Profile guided optimization: off, generate or use")
set_property(CACHE ENABLE_PGO PROPERTY STRINGS off generate use)

# Explicit CPU to tune for. Only used when ENABLE_NATIVE_ARCH cannot be applied
# (cross compiling, or a compiler that does not understand -mcpu=native).
# Useful values on ARM: cortex-a76 (Pi 5), cortex-a72 (Pi 4), cortex-a53 (Pi 3 / Zero 2 W)
set(STREAM1090_CPU "" CACHE STRING "Explicit CPU for -mcpu=/-mtune= (e.g. cortex-a76)")

set(STATS_DEF        STATS_ENABLED=$<BOOL:${ENABLE_STATS}>)
set(STATS_END_DEF    STATS_END_ONLY=$<BOOL:${END_STATS}>)
set(CUSTOM_INPUT_DEF STREAM1090_CUSTOM_INPUT=$<BOOL:${ENABLE_CUSTOM_INPUT}>)
set(RSSI_DEF         STREAM1090_RSSI=$<BOOL:${ENABLE_RSSI}>)
set(PREAMBLE_GATE_DEF STREAM1090_PREAMBLE_GATE=$<BOOL:${ENABLE_PREAMBLE_GATE}>)
set(PREAMBLE_THRESHOLD_DEF STREAM1090_PREAMBLE_THRESHOLD=${PREAMBLE_GATE_THRESHOLD})
set(MIN_SNR_DEF         STREAM1090_MIN_SNR=${MIN_SNR})
set(ORBGRAND_DEF STREAM1090_ORBGRAND=$<BOOL:${ENABLE_ORBGRAND}>)
set(ORBGRAND_GUESSES_DEF STREAM1090_ORBGRAND_GUESSES=${ORBGRAND_GUESSES})
set(TOO_MUCH_CPU_DEF STREAM1090_TOO_MUCH_CPU=$<BOOL:${ENABLE_TOO_MUCH_CPU}>)
set(INTERPOLATION_DEF STREAM1090_INTERP=${INTERPOLATION_KERNEL})

# ------------------------------------------------------------
# Compiler settings
# ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DEFAULT_COMPILE_OPTIONS
    -Wall -Wextra -Wunused-function -O3 -ffast-math
)

# ------------------------------------------------------------
# CPU tuning
#
# The default aarch64 baseline (armv8-a, generic scheduling) leaves a good
# chunk of performance on the table on the boards this usually runs on.
# Tuning for the actual core mostly changes instruction selection and
# scheduling, which is exactly what the tight DSP loops care about.
# ------------------------------------------------------------
include(CheckCXXCompilerFlag)

# Adds a flag to DEFAULT_COMPILE_OPTIONS if the compiler accepts it.
function(stream1090_try_flag flag resultVar)
    string(MAKE_C_IDENTIFIER "HAVE${flag}" cacheVar)
    check_cxx_compiler_flag("${flag}" ${cacheVar})
    if(${cacheVar})
        set(DEFAULT_COMPILE_OPTIONS ${DEFAULT_COMPILE_OPTIONS} ${flag} PARENT_SCOPE)
    endif()
    set(${resultVar} ${${cacheVar}} PARENT_SCOPE)
endfunction()

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
set(DEFAULT_LINK_OPTIONS "")

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
    ${STATS_DEF}
    ${STATS_END_DEF}
    ${CUSTOM_INPUT_DEF}
    ${RSSI_DEF}
    ${TOO_MUCH_CPU_DEF}
    ${PREAMBLE_GATE_DEF}
    ${PREAMBLE_THRESHOLD_DEF}
    ${MIN_SNR_DEF}
    ${ORBGRAND_DEF}
    ${ORBGRAND_GUESSES_DEF}
    ${INTERPOLATION_DEF}
    ${DEVICE_DEFINITIONS}
)

target_link_libraries(stream1090 PRIVATE ${DEVICE_LIBS})

include(CTest)

function(stream1090_add_test target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE include)
    target_compile_options(${target} PRIVATE ${DEFAULT_COMPILE_OPTIONS})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

if(BUILD_TESTING)
    stream1090_add_test(crc_error_table_test tests/CRCErrorTableTest.cpp)
    stream1090_add_test(icao_cache_test tests/ICAOCacheTest.cpp)
    stream1090_add_test(df11_interrogator_test tests/DF11InterrogatorTest.cpp)
    stream1090_add_test(ring_buffer_test tests/RingBufferTest.cpp)
    stream1090_add_test(erasure_repair_test tests/ErasureRepairTest.cpp)
    stream1090_add_test(low_pass_filter_test tests/LowPassFilterTest.cpp)
    stream1090_add_test(sampler_test tests/SamplerTest.cpp)
    stream1090_add_test(mode_s_altitude_test tests/ModeSAltitudeTest.cpp)
    stream1090_add_test(mode_s_position_test tests/ModeSPositionTest.cpp)
    stream1090_add_test(position_repair_test tests/PositionRepairTest.cpp)
    stream1090_add_test(df17_first_frame_test tests/DF17FirstFrameTest.cpp)
    stream1090_add_test(sampler_interp_sharpening_test tests/SamplerInterpolationTest.cpp)
    target_compile_definitions(sampler_interp_sharpening_test PRIVATE STREAM1090_INTERP=6)
endif()

# ------------------------------------------------------------
# Install rules
# ------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS stream1090
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES configs/rtlsdr.ini configs/airspy.ini
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/stream1090
)

install(DIRECTORY custom_filters/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/stream1090/custom_filters
)

install(DIRECTORY filter_utils/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/stream1090/filter_utils
)

# ------------------------------------------------------------
# CPack / DEB packaging
# ------------------------------------------------------------
set(CPACK_PACKAGE_NAME "stream1090")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Joseph Prochazka <joe@swiftbyte.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Mode-S demodulator for ADS-B reception via SDR")
set(CPACK_PACKAGE_DESCRIPTION
    "stream1090 reads IQ samples from an Airspy or RTL-SDR dongle and outputs\n"
    "raw Mode-S message frames to stdout using CRC-based message framing.\n"
    "It is designed to feed a decoder such as readsb or dump1090-fa via socat.")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "socat")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/jprochazka/stream1090")

include(CPack)

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
