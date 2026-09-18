cmake_minimum_required(VERSION 3.18)
project(libsigrokcxx CXX)

include(GNUInstallDirs)
include(TestBigEndian)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIBMM REQUIRED IMPORTED_TARGET glibmm-2.4>=2.32.0)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.32.0)

if(TARGET sigrok)
    set(SIGROK_LIBS sigrok)
else()
    pkg_check_modules(LIBSIGROK REQUIRED IMPORTED_TARGET libsigrok>=0.5.0)
    set(SIGROK_LIBS PkgConfig::LIBSIGROK)
endif()

# 独立生成绑定所需配置，避免错误包含其他组件安装的 config.h。
test_big_endian(WORDS_BIGENDIAN)
configure_file(config.h.cmake config.h)

add_library(sigrokcxx SHARED classes.cpp)
target_compile_definitions(sigrokcxx PRIVATE HAVE_STOI_STOD)
target_include_directories(sigrokcxx
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(sigrokcxx PUBLIC
    ${SIGROK_LIBS} PkgConfig::GLIBMM PkgConfig::GLIB)

set_target_properties(sigrokcxx PROPERTIES
    OUTPUT_NAME sigrokcxx
    VERSION 0.6.0
    SOVERSION 4
)

install(TARGETS sigrokcxx
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/libsigrokcxx/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libsigrokcxx
    FILES_MATCHING PATTERN "*.hpp"
)

set(prefix ${CMAKE_INSTALL_PREFIX})
set(exec_prefix "\${prefix}")
set(libdir "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
set(SR_PACKAGE_VERSION "0.6.0-ch32h417")

configure_file(libsigrokcxx.pc.in libsigrokcxx.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libsigrokcxx.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
