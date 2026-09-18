cmake_minimum_required(VERSION 3.18)
project(libsigrokdecode C)

include(GNUInstallDirs)
include(TestBigEndian)
test_big_endian(WORDS_BIGENDIAN)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.32.0)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter Development.Embed)
find_package(Threads REQUIRED)

# Platform
if(WIN32 OR MINGW)
    set(PLATFORM_LIBS ws2_32)
    set(CONF_HOST "x86_64-w64-mingw32")
    add_definitions(-DWIN32_LEAN_AND_MEAN)
else()
    set(PLATFORM_LIBS m ${CMAKE_DL_LIBS})
    if(APPLE)
        set(CONF_HOST "${CMAKE_SYSTEM_PROCESSOR}-apple-darwin")
    else()
        set(CONF_HOST "${CMAKE_SYSTEM_PROCESSOR}-pc-linux-gnu")
    endif()
endif()

# CMake 使用独立模板，避免 Autotools 的 #undef 覆盖平台检测结果。
set(PACKAGE_NAME "libsigrokdecode")
set(PACKAGE_STRING "libsigrokdecode 0.6.0")
set(PACKAGE_TARNAME "libsigrokdecode")
set(PACKAGE_URL "http://www.sigrok.org")
set(PACKAGE_VERSION "0.6.0")
set(PACKAGE_BUGREPORT "sigrok-devel@lists.sourceforge.net")
set(SRD_PACKAGE_VERSION_MAJOR 0)
set(SRD_PACKAGE_VERSION_MINOR 6)
set(SRD_PACKAGE_VERSION_MICRO 0)
set(SRD_PACKAGE_VERSION_STRING "0.6.0")
set(SRD_LIB_VERSION_CURRENT 4)
set(SRD_LIB_VERSION_REVISION 0)
set(SRD_LIB_VERSION_AGE 0)
set(SRD_LIB_VERSION_STRING "4:0:0")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)

# version.h generation (using ifndef guards, no conflict)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.h
"#ifndef LIBSIGROKDECODE_VERSION_H\n#define LIBSIGROKDECODE_VERSION_H\n\
#define SRD_PACKAGE_VERSION_MAJOR 0\n\
#define SRD_PACKAGE_VERSION_MINOR 6\n\
#define SRD_PACKAGE_VERSION_MICRO 0\n\
#define SRD_PACKAGE_VERSION_STRING \"0.6.0\"\n\
#define SRD_LIB_VERSION_CURRENT 4\n\
#define SRD_LIB_VERSION_REVISION 0\n\
#define SRD_LIB_VERSION_AGE 0\n\
#define SRD_LIB_VERSION_STRING \"4:0:0\"\n\
#endif\n")

# Compiler flags
if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4996")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")
    if(NOT DISABLE_WERROR)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
    endif()
endif()

include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Source files
set(LIBSIGROKDECODE_SOURCES
    decoder.c
    error.c
    exception.c
    instance.c
    log.c
    module_sigrokdecode.c
    session.c
    srd.c
    type_decoder.c
    util.c
    version.c
)

# Library
add_library(sigrokdecode SHARED ${LIBSIGROKDECODE_SOURCES})

# Export all symbols on Windows (needed for DLL import lib)
if(MINGW)
    set_target_properties(sigrokdecode PROPERTIES LINK_FLAGS "-Wl,--export-all-symbols")
endif()

target_link_libraries(sigrokdecode PUBLIC PkgConfig::GLIB
    PRIVATE Python3::Python Threads::Threads ${PLATFORM_LIBS})
target_compile_definitions(sigrokdecode PRIVATE
    DECODERS_DIR="${CMAKE_INSTALL_FULL_DATADIR}/libsigrokdecode/decoders")

set_target_properties(sigrokdecode PROPERTIES
    OUTPUT_NAME sigrokdecode
    VERSION 0.6.0
    SOVERSION 4
)

install(TARGETS sigrokdecode
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers
install(FILES libsigrokdecode.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libsigrokdecode
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libsigrokdecode
)

# Install decoders (Python protocol decoders)
install(DIRECTORY decoders/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/libsigrokdecode/decoders
    FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE
)

# pkg-config
set(prefix ${CMAKE_INSTALL_PREFIX})
set(exec_prefix "\${prefix}")
set(libdir "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libsigrokdecode.pc
"prefix=${prefix}
exec_prefix=\${prefix}
libdir=${libdir}
includedir=${includedir}

Name: libsigrokdecode
Description: Protocol decoding library for sigrok
URL: http://www.sigrok.org
Version: 0.6.0
Requires: glib-2.0
Libs: -L\${libdir} -lsigrokdecode
Cflags: -I\${includedir}
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libsigrokdecode.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
