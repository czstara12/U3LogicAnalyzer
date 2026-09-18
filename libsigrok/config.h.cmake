/**
 * @file
 * @brief libsigrok 的 CMake 配置模板，按目标平台检测功能。
 */

/* The canonical host libsigrok will run on. */
#define CONF_HOST "@CONF_HOST@"

/* Build-time version of libusb. */
#cmakedefine CONF_LIBUSB_1_0_VERSION "@CONF_LIBUSB_1_0_VERSION@"

/* Build-time version of libzip. */
#cmakedefine CONF_LIBZIP_VERSION "@CONF_LIBZIP_VERSION@"

/* Build-time version of libserialport. */
#cmakedefine CONF_LIBSERIALPORT_VERSION "@CONF_LIBSERIALPORT_VERSION@"

/* Define to 1 if you have the <dlfcn.h> header file. */
#cmakedefine HAVE_DLFCN_H 1

/* Whether at least one driver is enabled. */
#cmakedefine HAVE_DRIVERS 1

/* Whether to support WCH CH32H417 device. */
#cmakedefine HAVE_HW_WCH_CH32H417 1
#cmakedefine HAVE_HW_DEMO 1

/* Whether serial communication is supported. */
#cmakedefine HAVE_SERIAL_COMM 1

/* Specifies whether we have libftdi. */
/* #undef HAVE_LIBFTDI */

/* Specifies whether libhidapi is available. */
/* #undef HAVE_LIBHIDAPI */

/* Specifies whether Bluetooth communication is supported. */
/* #undef HAVE_BLUETOOTH */

/* Specifies whether librevisa is available. */
/* #undef HAVE_LIBREVISA */

/* Specifies whether libgpib is available. */
/* #undef HAVE_LIBGPIB */

/* Specifies whether libieee1284 is available. */
/* #undef HAVE_LIBIEEE1284 */

/* Specifies whether libnettle is available. */
/* #undef HAVE_LIBNETTLE */

/* Specifies whether libgio is available. */
/* #undef HAVE_LIBGIO_2_0 */

/* Define to 1 if you have the <libusb-1.0/libusb.h> header file. */
#cmakedefine HAVE_LIBUSB_1_0 1

/* Define to 1 if you have the <libserialport.h> header file. */
#cmakedefine HAVE_LIBSERIALPORT 1

/* Define to 1 if you have the <sys/mman.h> header file. */
#cmakedefine HAVE_SYS_MMAN_H 1

/* Define to 1 if you have the <sys/ioctl.h> header file. */
#cmakedefine HAVE_SYS_IOCTL_H 1

/* Define to 1 if you have the <sys/timerfd.h> header file. */
#cmakedefine HAVE_SYS_TIMERFD_H 1

/* Define to 1 if you have the <fcntl.h> header file. */
#cmakedefine HAVE_FCNTL_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#cmakedefine HAVE_UNISTD_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <strings.h> header file. */
#cmakedefine HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <stdio.h> header file. */
#define HAVE_STDIO_H 1

/* Define to 1 if you have the `strcasecmp' function. */
#cmakedefine HAVE_STRCASECMP 1

/* Define to 1 if you have the `strncasecmp' function. */
#cmakedefine HAVE_STRNCASECMP 1

/* Define to 1 if you have the `gettimeofday' function. */
#define HAVE_GETTIMEOFDAY 1

/* Whether freebsd is the target. */
/* #undef __FreeBSD__ */

/* Whether apple is the target. */
/* #undef __APPLE__ */

/* Define to 1 if you have unaligned little endian access */
/* 使用可移植的逐字节访问，避免对目标架构对齐规则作假设。 */
/* #undef HAVE_UNALIGNED_LITTLE_ENDIAN_ACCESS */

/** 目标处理器采用大端字节序时定义。 */
#cmakedefine WORDS_BIGENDIAN 1

/* Define to 1 if you have unaligned big endian access */
/* #undef HAVE_UNALIGNED_BIG_ENDIAN_ACCESS */

/* libzip has the zip_discard function. */
#cmakedefine HAVE_ZIP_DISCARD 1

/* libusb has the libusb_os_handle function. */
/* #undef HAVE_LIBUSB_OS_HANDLE */

/* libusb has the libusb_set_auto_detach_kernel_driver function. */
#cmakedefine HAVE_LIBUSB_SET_AUTO_DETACH_KERNEL_DRIVER 1

/* The size of `unsigned long`, as computed by sizeof. */
#define SIZEOF_UNSIGNED_LONG @SIZEOF_UNSIGNED_LONG@

/* The size of `unsigned int`, as computed by sizeof. */
#define SIZEOF_UNSIGNED_INT @SIZEOF_UNSIGNED_INT@

/* Use unsigned long for uint64_t. */
#cmakedefine HAVE_UINT64_T_UL 1

/* Use unsigned int for uint64_t. */
#cmakedefine HAVE_UINT64_T_UI 1

/* Version number of package */
#define VERSION "0.6.0-ch32h417"

/* Package name */
#define PACKAGE_NAME "libsigrok"
#define PACKAGE_STRING "libsigrok 0.6.0-ch32h417"

/* Package version macros */
#define SR_PACKAGE_VERSION_MAJOR 0
#define SR_PACKAGE_VERSION_MINOR 6
#define SR_PACKAGE_VERSION_MICRO 0
#define SR_PACKAGE_VERSION_STRING "0.6.0-ch32h417"

/* Libtool version macros */
#define SR_LIB_VERSION_CURRENT 4
#define SR_LIB_VERSION_REVISION 0
#define SR_LIB_VERSION_AGE 0
#define SR_LIB_VERSION_STRING "4:0:0"

/* Define to 1 if on Windows/MinGW */
#if defined(_WIN32) || defined(__MINGW32__)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
/* Building on MinGW */
#ifndef WINVER
#define WINVER 0x0601
#endif
#endif
