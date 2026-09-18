cmake_minimum_required(VERSION 3.18)
project(libsigrok C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ============================================================================
# Dependencies
# ============================================================================
find_package(PkgConfig REQUIRED)

# glib-2.0 is mandatory
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.32.0)

# USB 支持和会话文件读写需要对应库；串口后端可选。
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.16)
pkg_check_modules(LIBSERIALPORT IMPORTED_TARGET libserialport>=0.1.1)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip>=0.10)
find_package(Threads REQUIRED)

# CH375DLL 是 Windows 专用后端；其他平台提供演示、文件分析和协议解码。
option(ENABLE_WCH_CH32H417 "启用 Windows CH375DLL 硬件驱动" ${WIN32})
if(ENABLE_WCH_CH32H417 AND NOT WIN32)
    message(FATAL_ERROR "CH32H417 硬件驱动需要 Windows CH375DLL；请使用 -DENABLE_WCH_CH32H417=OFF")
endif()
message(STATUS "CH32H417 硬件驱动: ${ENABLE_WCH_CH32H417}")
if(NOT WIN32)
    message(STATUS "当前平台支持演示设备、文件分析和协议解码；CH375DLL 硬件采集仅支持 Windows")
endif()

# Windows-specific libs
if(WIN32 OR MINGW)
    set(PLATFORM_LIBS ws2_32 setupapi ole32)
else()
    set(PLATFORM_LIBS m ${CMAKE_DL_LIBS})
endif()

# ============================================================================
# config.h generation
# ============================================================================
# Determine host
if(WIN32 OR MINGW)
    set(CONF_HOST "x86_64-w64-mingw32")
elseif(APPLE)
    set(CONF_HOST "${CMAKE_SYSTEM_PROCESSOR}-apple-darwin")
else()
    set(CONF_HOST "${CMAKE_SYSTEM_PROCESSOR}-pc-linux-gnu")
endif()

# Check for optional features
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckTypeSize)
include(TestBigEndian)
test_big_endian(WORDS_BIGENDIAN)

check_include_file(dlfcn.h HAVE_DLFCN_H)
check_include_file(sys/mman.h HAVE_SYS_MMAN_H)
check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
check_include_file(sys/timerfd.h HAVE_SYS_TIMERFD_H)
check_include_file(fcntl.h HAVE_FCNTL_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(strings.h HAVE_STRINGS_H)

check_symbol_exists(strcasecmp "strings.h" HAVE_STRCASECMP)
check_symbol_exists(strncasecmp "strings.h" HAVE_STRNCASECMP)
set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBZIP)
check_symbol_exists(zip_discard "zip.h" HAVE_ZIP_DISCARD)
set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBUSB)
check_symbol_exists(libusb_set_auto_detach_kernel_driver "libusb.h" HAVE_LIBUSB_SET_AUTO_DETACH_KERNEL_DRIVER)
unset(CMAKE_REQUIRED_LIBRARIES)

# Version defines for optional libraries
if(LIBUSB_FOUND)
    set(HAVE_LIBUSB_1_0 1)
    set(CONF_LIBUSB_1_0_VERSION "${LIBUSB_VERSION}")
endif()

if(LIBZIP_FOUND)
    set(CONF_LIBZIP_VERSION "${LIBZIP_VERSION}")
endif()

if(LIBSERIALPORT_FOUND)
    set(HAVE_LIBSERIALPORT 1)
    set(CONF_LIBSERIALPORT_VERSION "${LIBSERIALPORT_VERSION}")
endif()

# Driver defines
set(HAVE_DRIVERS 1)
set(HAVE_HW_WCH_CH32H417 ${ENABLE_WCH_CH32H417})
set(HAVE_HW_DEMO 1)
set(HAVE_WCH_CH32H417 ${ENABLE_WCH_CH32H417})

# Force HAVE_SERIAL_COMM for SCPI string parsing (no libserialport needed)
set(HAVE_SERIAL_COMM 1)

# 由目标编译器确定数据模型，兼容 Windows LLP64 和 Unix LP64。
check_type_size("unsigned long" SIZEOF_UNSIGNED_LONG)
check_type_size("unsigned int" SIZEOF_UNSIGNED_INT)
if(SIZEOF_UNSIGNED_LONG EQUAL 8)
    set(HAVE_UINT64_T_UL 1)
elseif(SIZEOF_UNSIGNED_INT EQUAL 8)
    set(HAVE_UINT64_T_UI 1)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)

# Generate version.h
set(SR_PACKAGE_VERSION_MAJOR 0)
set(SR_PACKAGE_VERSION_MINOR 6)
set(SR_PACKAGE_VERSION_MICRO 0)
set(SR_PACKAGE_VERSION_STRING "0.6.0-ch32h417")
set(SR_LIB_VERSION_CURRENT 4)
set(SR_LIB_VERSION_REVISION 0)
set(SR_LIB_VERSION_AGE 0)
set(SR_LIB_VERSION_STRING "4:0:0")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/libsigrok/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/libsigrok/version.h)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/libsigrok/version.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libsigrok)

# ============================================================================
# Include directories  
# ============================================================================
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}  # for config.h
    ${CMAKE_CURRENT_BINARY_DIR}/include  # for version.h
    ${GLIB_INCLUDE_DIRS}
)

if(LIBUSB_FOUND)
    include_directories(${LIBUSB_INCLUDE_DIRS})
endif()
if(LIBSERIALPORT_FOUND)
    include_directories(${LIBSERIALPORT_INCLUDE_DIRS})
endif()
if(LIBZIP_FOUND)
    include_directories(${LIBZIP_INCLUDE_DIRS})
endif()

# ============================================================================
# Compiler flags
# ============================================================================
if(MSVC)
    # MSVC doesn't support GCC section attributes
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4996 /wd4267")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-incompatible-pointer-types")
    if(NOT DISABLE_WERROR)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
    endif()
    
    # Driver list section magic needs no-reorder optimization
    # This is handled per-platform via the libsigrok-internal.h macros
endif()

# Platform defines
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601)
endif()

# ============================================================================
# Source files
# ============================================================================
set(LIBSIGROK_SOURCES
    # Core backend
    src/backend.c
    src/binary_helpers.c
    src/conversion.c
    src/crc.c
    src/device.c
    src/session.c
    src/session_file.c
    src/session_driver.c
    src/hwdriver.c
    src/trigger.c
    src/soft-trigger.c
    src/analog.c
    src/fallback.c
    src/resource.c
    src/strutil.c
    src/log.c
    src/version.c
    src/error.c
    src/std.c
    src/sw_limits.c

    # 显式注册驱动，避免依赖 ELF、PE 和 Mach-O 的链接段排列。
    src/drivers.c

    # Hardware drivers
    src/hardware/demo/api.c
    src/hardware/demo/protocol.c

    # Input modules
    src/input/input.c
    src/input/feed_queue.c
    src/input/binary.c
    src/input/chronovu_la8.c
    src/input/csv.c
    src/input/logicport.c
    src/input/raw_analog.c
    src/input/saleae.c
    src/input/trace32_ad.c
    src/input/vcd.c
    src/input/wav.c
    src/input/null.c

    # Output modules
    src/output/output.c
    src/output/analog.c
    src/output/ascii.c
    src/output/bits.c
    src/output/binary.c
    src/output/csv.c
    src/output/chronovu_la8.c
    src/output/wav.c
    src/output/hex.c
    src/output/ols.c
    src/output/srzip.c
    src/output/vcd.c
    src/output/wavedrom.c
    src/output/null.c

    # Transform modules
    src/transform/transform.c
    src/transform/nop.c
    src/transform/scale.c
    src/transform/invert.c

    # USB support
    src/usb.c
    src/ezusb.c

    # Serial support
    src/serial.c
    src/serial_bt.c
    src/serial_hid.c
    src/serial_hid_bu86x.c
    src/serial_hid_ch9325.c
    src/serial_hid_cp2110.c
    src/serial_hid_victor.c
    src/serial_libsp.c

    # SCPI core (utility functions used by device.c, std.c, csv.c)
    src/scpi/scpi.c
    src/scpi/scpi_tcp.c
    src/scpi/scpi_serial.c
    src/scpi/scpi_usbtmc_libusb.c

    # Bluetooth
    src/bt/bt_bluez.c

    # Modbus
    src/modbus/modbus.c
    src/modbus/modbus_serial_rtu.c

    # DMM chip protocols
    src/dmm/asycii.c
    src/dmm/bm25x.c
    src/dmm/bm52x.c
    src/dmm/bm85x.c
    src/dmm/bm86x.c
    src/dmm/dtm0660.c
    src/dmm/eev121gw.c
    src/dmm/es519xx.c
    src/dmm/fs9721.c
    src/dmm/fs9922.c
    src/dmm/m2110.c
    src/dmm/metex14.c
    src/dmm/mm38xr.c
    src/dmm/ms2115b.c
    src/dmm/ms8250d.c
    src/dmm/rs9lcd.c
    src/dmm/ut372.c
    src/dmm/ut71x.c
    src/dmm/vc870.c
    src/dmm/vc96.c

    # LCR
    src/lcr/es51919.c
    src/lcr/vc4080.c

    # Scale
    src/scale/kern.c
)

# ============================================================================
# Library target
# ============================================================================
if(ENABLE_WCH_CH32H417)
    list(APPEND LIBSIGROK_SOURCES
        src/hardware/wch-ch32h417/api.c
        src/hardware/wch-ch32h417/protocol.c
        src/hardware/wch-ch32h417/ch375_wrapper.c)
endif()

add_library(sigrok SHARED ${LIBSIGROK_SOURCES})
target_compile_definitions(sigrok PRIVATE SR_CMAKE_DRIVER_LIST)
target_include_directories(sigrok PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(sigrok PUBLIC PkgConfig::GLIB PRIVATE
    PkgConfig::LIBUSB PkgConfig::LIBZIP Threads::Threads ${PLATFORM_LIBS}
)

if(LIBSERIALPORT_FOUND)
    target_link_libraries(sigrok PRIVATE PkgConfig::LIBSERIALPORT)
endif()

# Set output name to match pkg-config expectation
set_target_properties(sigrok PROPERTIES
    OUTPUT_NAME sigrok
    VERSION 0.6.0
    SOVERSION 4
)

# ============================================================================
# Install
# ============================================================================
install(TARGETS sigrok
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/libsigrok/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libsigrok
    FILES_MATCHING PATTERN "*.h"
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/config.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libsigrok
)
# Also install to include root for C++ bindings compat
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/config.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ============================================================================
# pkg-config file
# ============================================================================
set(prefix ${CMAKE_INSTALL_PREFIX})
set(exec_prefix "\${prefix}")
set(libdir "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libsigrok.pc.in ${CMAKE_CURRENT_BINARY_DIR}/libsigrok.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libsigrok.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
