##
## This file is part of the PulseView project.
##
## Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
## Copyright (C) 2012-2013 Alexandru Gagniuc <mr.nuke.me@gmail.com>
## Copyright (C) 2020 Soeren Apel <soeren@apelpie.net>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.
##

cmake_minimum_required(VERSION 3.16)

project(pulseview C CXX)

include(GNUInstallDirs)

# Let AUTOMOC and AUTOUIC process GENERATED files.
if(POLICY CMP0071)
	cmake_policy(SET CMP0071 NEW)
endif()

# Only interpret if() arguments as variables or keywords when unquoted.
if(POLICY CMP0054)
	cmake_policy(SET CMP0054 NEW)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake")

#===============================================================================
#= User Options
#-------------------------------------------------------------------------------

option(DISABLE_WERROR "Build without -Werror" TRUE)
option(ENABLE_SIGNALS "Build with UNIX signals" TRUE)
option(ENABLE_STACKTRACE "Enable stack trace when crashing" FALSE)
option(ENABLE_DECODE "Build with libsigrokdecode" TRUE)
option(ENABLE_FLOW "Build with libsigrokflow" FALSE)
option(ENABLE_TESTS "Enable unit tests" FALSE)
option(STATIC_PKGDEPS_LIBS "Statically link to (pkg-config) libraries" FALSE)

if(WIN32)
	# On Windows/MinGW we need to statically link to libraries.
	# This option is user configurable, but enable it by default on win32.
	# DISABLED: Qt5-static injects -static which conflicts with dynamic GLib linking
	# set(STATIC_PKGDEPS_LIBS TRUE)

	# Windows does not support UNIX signals.
	set(ENABLE_SIGNALS FALSE)
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
	"Choose the type of build (None, Debug, Release, RelWithDebInfo, MinSizeRel)."
	FORCE)
endif()

#===============================================================================
#= Documentation
#-------------------------------------------------------------------------------

# add_subdirectory(manual)  # Disabled: not needed for CH32H417 build

#===============================================================================
#= Dependencies
#-------------------------------------------------------------------------------

list(APPEND PKGDEPS glib-2.0>=2.28.0)
list(APPEND PKGDEPS glibmm-2.4>=2.28.0)
list(APPEND PKGDEPS libusb-1.0>=1.0.16)

if(ENABLE_FLOW)
	list(APPEND PKGDEPS gstreamermm-1.0>=1.8.0)
	list(APPEND PKGDEPS libsigrokflow>=0.1.0)
endif()

set(LIBSR_CXX_BINDING "libsigrokcxx>=0.5.2")
list(APPEND PKGDEPS "${LIBSR_CXX_BINDING}")

if(ENABLE_DECODE)
	list(APPEND PKGDEPS libsigrokdecode>=0.5.2)
endif()

if(ANDROID)
	list(APPEND PKGDEPS libsigrokandroidutils>=0.1.0)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSRCXX ${LIBSR_CXX_BINDING})
if(NOT LIBSRCXX_FOUND OR NOT LIBSRCXX_VERSION)
	message(FATAL_ERROR "libsigrok C++ bindings missing, check libsigrok's 'configure' output (missing dependencies?)")
endif()
pkg_check_modules(PKGDEPS REQUIRED ${PKGDEPS})

set(CMAKE_AUTOMOC TRUE)

find_package(Qt5 5.12 COMPONENTS Core Gui LinguistTools Widgets Svg REQUIRED)

message(STATUS "Qt version: ${Qt5_VERSION}")

get_target_property(PULSEVIEW_QT_LIBRARY_TYPE Qt5::Core TYPE)
if(WIN32 AND PULSEVIEW_QT_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
	# MXE workaround: Use pkg-config to find Qt5 libs.
	# https://github.com/mxe/mxe/issues/1642
	# Not required (and doesn't work) on MSYS2.
	if(NOT DEFINED ENV{MSYSTEM})
		pkg_check_modules(QT5ALL REQUIRED Qt5Widgets>=5.3 Qt5Gui>=5.3 Qt5Svg>=5.3)
	endif()
endif()

set(QT_LIBRARIES Qt5::Gui Qt5::Widgets Qt5::Svg)

set(BOOSTCOMPS filesystem serialization)
if(ENABLE_TESTS)
	list(APPEND BOOSTCOMPS unit_test_framework)
endif()

if(ENABLE_STACKTRACE)
	include(FindBacktrace)
	if (Backtrace_FOUND)
		set(_Boost_STACKTRACE_BACKTRACE_HEADERS "boost/stacktrace.hpp")
		list(APPEND BOOSTCOMPS stacktrace_backtrace)
	else()
		set(_Boost_STACKTRACE_BASIC_HEADERS     "boost/stacktrace.hpp")
		list(APPEND BOOSTCOMPS stacktrace_basic)
	endif()
	find_package(Boost 1.65.1 CONFIG COMPONENTS ${BOOSTCOMPS} REQUIRED)
else()
	find_package(Boost 1.65.1 CONFIG COMPONENTS ${BOOSTCOMPS} REQUIRED)
endif()

# Find the platform's thread library (needed for C++11 threads).
# This will set ${CMAKE_THREAD_LIBS_INIT} to the correct, OS-specific value.
find_package(Threads REQUIRED)

# Check for explicit link against libatomic
#
# Depending on the toolchain, linking a program using atomic functions may need
# "-latomic" explicitly passed to the linker
#
# This check first tests if atomics are available in the C-library, if not and
# libatomic exists, then it runs the same test with -latomic added to the
# linker flags.

# Helper for checking for atomics
function(check_working_cxx_atomics varname additional_lib)
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state()
  set(CMAKE_REQUIRED_FLAGS "-std=c++11")
  set(CMAKE_REQUIRED_LIBRARIES "${additional_lib}")
  set(CMAKE_REQUIRED_QUIET 1)
  CHECK_CXX_SOURCE_COMPILES("
#include <atomic>
std::atomic<int> x;
int main() {
  return std::atomic_fetch_add_explicit(&x, 1, std::memory_order_seq_cst);
}
" ${varname})
  cmake_pop_check_state()
endfunction(check_working_cxx_atomics)

# First check if atomics work without the library.
# If not, check if the library exists, and atomics work with it.
check_working_cxx_atomics(HAVE_CXX_ATOMICS_WITHOUT_LIB "")
if(HAVE_CXX_ATOMICS_WITHOUT_LIB)
  message(STATUS "Atomics provided by the C-library - yes")
else()
  message(STATUS "Atomics provided by the C-library - no")
  find_library(LIBATOMIC_LIBRARY NAMES atomic PATH_SUFFIXES lib)
  if(LIBATOMIC_LIBRARY)
    check_working_cxx_atomics(HAVE_CXX_ATOMICS_WITH_LIB "${LIBATOMIC_LIBRARY}")
    if (HAVE_CXX_ATOMICS_WITH_LIB)
      message(STATUS "Atomics provided by libatomic - yes")
    else()
      message(STATUS "Atomics provided by libatomic - no")
      message(FATAL_ERROR "Compiler must support std::atomic!")
    endif()
  else()
    message(FATAL_ERROR "Compiler appears to require libatomic, but cannot find it.")
  endif()
endif()

# Check availability of features which depend on library versions.
# TODO Ideally use check_symbol_exists() instead, reduce boilerplate.
if(ENABLE_DECODE)
	include(CheckCSourceCompiles)
	include(CMakePushCheckState)
	cmake_push_check_state()
	set(CMAKE_REQUIRED_INCLUDES "${PKGDEPS_INCLUDE_DIRS}")
	set(CMAKE_REQUIRED_LIBRARIES "sigrokdecode")
	foreach (LPATH ${PKGDEPS_LIBRARY_DIRS})
		list(APPEND CMAKE_REQUIRED_LINK_OPTIONS "-L${LPATH}")
	endforeach ()
	check_c_source_compiles("
	#include <libsigrokdecode/libsigrokdecode.h>
	int main(int argc, char *argv[])
	{
		(void)argc;
		(void)argv;
		return srd_session_send_eof(NULL);
	}
	" HAVE_SRD_SESSION_SEND_EOF)
	cmake_pop_check_state()
endif()

#===============================================================================
#= System Introspection
#-------------------------------------------------------------------------------

include(memaccess)
memaccess_check_unaligned_le(HAVE_UNALIGNED_LITTLE_ENDIAN_ACCESS)

#===============================================================================
#= Config Header
#-------------------------------------------------------------------------------

set(PV_TITLE PulseView)
set(PV_VERSION_STRING "0.5.0")

set(PV_GLIBMM_VERSION ${PKGDEPS_glibmm-2.4_VERSION})

include(GetGitRevisionDescription)

# Append the revision hash unless we are exactly on a tagged release.
git_describe(PV_TAG_VERSION_STRING --match "pulseview-${PV_VERSION_STRING}" --exact-match)
if(NOT PV_TAG_VERSION_STRING)
	get_git_head_revision(PV_REVSPEC PV_HASH)
	if(PV_HASH)
		string(SUBSTRING "${PV_HASH}" 0 7 PV_SHORTHASH)
		set(PV_VERSION_STRING "${PV_VERSION_STRING}-git-${PV_SHORTHASH}")
	endif()

	# Non-tagged releases use the unstable manual
	set(PV_MANUAL_VERSION "unstable")
else()
	# Tagged releases use a fixed manual version
	set(PV_MANUAL_VERSION ${PV_VERSION_STRING})
endif()

if(PV_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[-0-9a-z]*)?$")
	set(PV_VERSION_MAJOR ${CMAKE_MATCH_1})
	set(PV_VERSION_MINOR ${CMAKE_MATCH_2})
	set(PV_VERSION_MICRO ${CMAKE_MATCH_3})
	set(PV_VERSION_SUFFIX ${CMAKE_MATCH_4})
endif()

message(STATUS "${PV_TITLE} version: ${PV_VERSION_STRING}")

configure_file (
	${PROJECT_SOURCE_DIR}/config.h.in
	${PROJECT_BINARY_DIR}/config.h
)

#===============================================================================
#= Sources
#-------------------------------------------------------------------------------

set(pulseview_SOURCES
	main.cpp
	pv/application.cpp
	pv/devicemanager.cpp
	pv/globalsettings.cpp
	pv/logging.cpp
	pv/mainwindow.cpp
	pv/metadata_obj.cpp
	pv/session.cpp
	pv/storesession.cpp
	pv/util.cpp
	pv/binding/binding.cpp
	pv/binding/inputoutput.cpp
	pv/binding/device.cpp
	pv/data/analog.cpp
	pv/data/analogsegment.cpp
	pv/data/logic.cpp
	pv/data/logicsegment.cpp
	pv/data/mathsignal.cpp
	pv/data/signalbase.cpp
	pv/data/signaldata.cpp
	pv/data/segment.cpp
	pv/devices/device.cpp
	pv/devices/file.cpp
	pv/devices/hardwaredevice.cpp
	pv/devices/inputfile.cpp
	pv/devices/sessionfile.cpp
	pv/dialogs/connect.cpp
	pv/dialogs/decoder_channel_dialog.cpp
	pv/dialogs/iapdialog.cpp
	pv/dialogs/inputoutputoptions.cpp
	pv/dialogs/settings.cpp
	pv/dialogs/storeprogress.cpp
	pv/popups/deviceoptions.cpp
	pv/popups/channels.cpp
	pv/prop/bool.cpp
	pv/prop/double.cpp
	pv/prop/enum.cpp
	pv/prop/int.cpp
	pv/prop/property.cpp
	pv/prop/string.cpp
	pv/subwindows/subwindowbase.cpp
	pv/toolbars/mainbar.cpp
	pv/views/trace/analogsignal.cpp
	pv/views/trace/cursor.cpp
	pv/views/trace/cursorpair.cpp
	pv/views/trace/flag.cpp
	pv/views/trace/header.cpp
	pv/views/trace/mathsignal.cpp
	pv/views/trace/marginwidget.cpp
	pv/views/trace/logicsignal.cpp
	pv/views/trace/ruler.cpp
	pv/views/trace/signal.cpp
	pv/views/trace/timeitem.cpp
	pv/views/trace/timemarker.cpp
	pv/views/trace/trace.cpp
	pv/views/trace/tracegroup.cpp
	pv/views/trace/tracepalette.cpp
	pv/views/trace/tracetreeitem.cpp
	pv/views/trace/tracetreeitemowner.cpp
	pv/views/trace/triggermarker.cpp
	pv/views/trace/view.cpp
	pv/views/trace/viewitem.cpp
	pv/views/trace/viewitemowner.cpp
	pv/views/trace/viewitempaintparams.cpp
	pv/views/trace/viewport.cpp
	pv/views/trace/viewwidget.cpp
	pv/views/viewbase.cpp
	pv/views/trace/standardbar.cpp
	pv/widgets/colorbutton.cpp
	pv/widgets/colorpopup.cpp
	pv/widgets/devicetoolbutton.cpp
	pv/widgets/exportmenu.cpp
	pv/widgets/flowlayout.cpp
	pv/widgets/importmenu.cpp
	pv/widgets/popup.cpp
	pv/widgets/popuptoolbutton.cpp
	pv/widgets/sweeptimingwidget.cpp
	pv/widgets/timestampspinbox.cpp
	pv/widgets/wellarray.cpp
)

# This list includes only QObject derived class headers.
set(pulseview_HEADERS
	pv/exprtk.hpp
	pv/logging.hpp
	pv/globalsettings.hpp
	pv/mainwindow.hpp
	pv/metadata_obj.hpp
	pv/session.hpp
	pv/storesession.hpp
	pv/binding/device.hpp
	pv/data/analog.hpp
	pv/data/analogsegment.hpp
	pv/data/logic.hpp
	pv/data/logicsegment.hpp
	pv/data/mathsignal.hpp
	pv/data/signalbase.hpp
	pv/dialogs/connect.hpp
	pv/dialogs/decoder_channel_dialog.hpp
	pv/dialogs/iapdialog.hpp
	pv/dialogs/inputoutputoptions.hpp
	pv/dialogs/settings.hpp
	pv/dialogs/storeprogress.hpp
	pv/popups/channels.hpp
	pv/popups/deviceoptions.hpp
	pv/prop/bool.hpp
	pv/prop/double.hpp
	pv/prop/enum.hpp
	pv/prop/int.hpp
	pv/prop/property.hpp
	pv/prop/string.hpp
	pv/subwindows/subwindowbase.hpp
	pv/toolbars/mainbar.hpp
	pv/views/trace/analogsignal.hpp
	pv/views/trace/cursor.hpp
	pv/views/trace/flag.hpp
	pv/views/trace/header.hpp
	pv/views/trace/logicsignal.hpp
	pv/views/trace/mathsignal.hpp
	pv/views/trace/marginwidget.hpp
	pv/views/trace/ruler.hpp
	pv/views/trace/signal.hpp
	pv/views/trace/timeitem.hpp
	pv/views/trace/timemarker.hpp
	pv/views/trace/trace.hpp
	pv/views/trace/tracegroup.hpp
	pv/views/trace/tracetreeitem.hpp
	pv/views/trace/triggermarker.hpp
	pv/views/trace/view.hpp
	pv/views/trace/viewitem.hpp
	pv/views/trace/viewport.hpp
	pv/views/trace/viewwidget.hpp
	pv/views/viewbase.hpp
	pv/views/trace/standardbar.hpp
	pv/widgets/colorbutton.hpp
	pv/widgets/colorpopup.hpp
	pv/widgets/devicetoolbutton.hpp
	pv/widgets/exportmenu.hpp
	pv/widgets/flowlayout.hpp
	pv/widgets/importmenu.hpp
	pv/widgets/popup.hpp
	pv/widgets/popuptoolbutton.hpp
	pv/widgets/sweeptimingwidget.hpp
	pv/widgets/timestampspinbox.hpp
	pv/widgets/wellarray.hpp
)

set(pulseview_RESOURCES
	pulseview.qrc
)

if(ENABLE_SIGNALS)
	list(APPEND pulseview_SOURCES signalhandler.cpp)
	list(APPEND pulseview_HEADERS signalhandler.hpp)
endif()

if(ENABLE_DECODE)
	list(APPEND pulseview_SOURCES
		pv/binding/decoder.cpp
		pv/data/decodesignal.cpp
		pv/data/decode/annotation.cpp
		pv/data/decode/decoder.cpp
		pv/data/decode/row.cpp
		pv/data/decode/rowdata.cpp
		pv/subwindows/decoder_selector/item.cpp
		pv/subwindows/decoder_selector/model.cpp
		pv/subwindows/decoder_selector/subwindow.cpp
		pv/views/decoder_binary/view.cpp
		pv/views/decoder_binary/QHexView.cpp
		pv/views/tabular_decoder/model.cpp
		pv/views/tabular_decoder/view.cpp
		pv/views/trace/decodetrace.cpp
		pv/widgets/decodergroupbox.cpp
		pv/widgets/decodermenu.cpp
	)

	list(APPEND pulseview_HEADERS
		pv/data/decodesignal.hpp
		pv/subwindows/decoder_selector/subwindow.hpp
		pv/views/decoder_binary/view.hpp
		pv/views/decoder_binary/QHexView.hpp
		pv/views/tabular_decoder/view.hpp
		pv/views/trace/decodetrace.hpp
		pv/widgets/decodergroupbox.hpp
		pv/widgets/decodermenu.hpp
	)
endif()

if(WIN32)
	# Use the sigrok icon for the pulseview.exe executable.
	set(CMAKE_RC_COMPILE_OBJECT "${CMAKE_RC_COMPILER} -O coff -I${CMAKE_CURRENT_SOURCE_DIR} <SOURCE> <OBJECT>")
	enable_language(RC)
	list(APPEND pulseview_SOURCES pulseviewico.rc)
endif()

if(ANDROID)
	list(APPEND pulseview_SOURCES
		android/assetreader.cpp
		android/loghandler.cpp
	)
endif()

qt5_add_resources(pulseview_RESOURCES_RCC ${pulseview_RESOURCES})

#===============================================================================
#= Translations
#-------------------------------------------------------------------------------

file(GLOB TS_FILES ${CMAKE_SOURCE_DIR}/l10n/*.ts)
set_property(SOURCE ${TS_FILES} PROPERTY OUTPUT_LOCATION ${CMAKE_BINARY_DIR}/l10n)
if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
	configure_file("translations.qrc" "translations.qrc" COPYONLY)
endif ()

qt5_add_translation(QM_FILES ${TS_FILES})
add_custom_target(pulseview_translations DEPENDS ${QM_FILES})

qt5_add_resources(pulseview_RESOURCES_RCC ${CMAKE_BINARY_DIR}/translations.qrc)

#===============================================================================
#= Global Definitions
#-------------------------------------------------------------------------------

add_definitions(-DQT_NO_KEYWORDS)
add_definitions(-D__STDC_LIMIT_MACROS)
add_definitions(-Wall -Wextra)
add_definitions(-std=c++14)
add_definitions(-DBOOST_MATH_DISABLE_FLOAT128=1)
if(STATIC_PKGDEPS_LIBS)
	add_definitions(-DGLIBMM_STATIC_COMPILATION -DGLIB_STATIC_COMPILATION)
endif()
add_definitions(-D_HAS_STD_BYTE=0)
if(WIN32)
	add_definitions(-Wa,-mbig-obj -O3)
endif()

if(ENABLE_FLOW)
	add_definitions(-DENABLE_FLOW)
endif()

if(ENABLE_DECODE)
	add_definitions(-DENABLE_DECODE)
endif()

if(NOT DISABLE_WERROR)
	add_definitions(-Werror)
endif()

if(ENABLE_SIGNALS)
	add_definitions(-DENABLE_SIGNALS)
endif()

if(ENABLE_STACKTRACE)
	add_definitions(-DENABLE_STACKTRACE)
	if(UNIX AND NOT APPLE)
		add_definitions(-fno-pie)
	endif()
	if (Backtrace_FOUND)
		add_definitions(-DBOOST_STACKTRACE_USE_BACKTRACE)
	endif()
endif()

#===============================================================================
#= Global Include Directories
#-------------------------------------------------------------------------------

include_directories(
	${CMAKE_CURRENT_BINARY_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
	${Boost_INCLUDE_DIRS}
)

# 各平台的 HIDAPI 包名不同，优先使用其导出的头文件与链接依赖。
pkg_search_module(HIDAPI QUIET IMPORTED_TARGET hidapi hidapi-hidraw hidapi-libusb)
if(HIDAPI_FOUND)
	set(PULSEVIEW_HIDAPI_LIBRARY PkgConfig::HIDAPI)
else()
	get_filename_component(PULSEVIEW_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
	get_filename_component(PULSEVIEW_TOOLCHAIN_PREFIX "${PULSEVIEW_COMPILER_BIN_DIR}" DIRECTORY)
	find_path(HIDAPI_INCLUDE_DIR
		NAMES hidapi.h
		HINTS "${PULSEVIEW_TOOLCHAIN_PREFIX}/include"
		PATH_SUFFIXES hidapi
	)
	if(NOT HIDAPI_INCLUDE_DIR)
		message(FATAL_ERROR "找不到 hidapi.h，请安装 HIDAPI 开发包或设置 HIDAPI_INCLUDE_DIR。")
	endif()
	include_directories(${HIDAPI_INCLUDE_DIR})
	find_library(PULSEVIEW_HIDAPI_LIBRARY
		NAMES hidapi hidapi-hidraw hidapi-libusb
		HINTS "${PULSEVIEW_TOOLCHAIN_PREFIX}/lib"
	)
	if(NOT PULSEVIEW_HIDAPI_LIBRARY)
		message(FATAL_ERROR "找不到 HIDAPI 库，请安装 HIDAPI 开发包。")
	endif()
endif()

if(STATIC_PKGDEPS_LIBS)
	include_directories(${PKGDEPS_STATIC_INCLUDE_DIRS})
else()
	include_directories(${PKGDEPS_INCLUDE_DIRS})
endif()

# Workaround for MSYS2: PKGDEPS_INCLUDE_DIRS may be empty even when pkg-config finds packages
# Manually add glib-2.0 and glibmm-2.4 include paths
if(WIN32 AND NOT PKGDEPS_INCLUDE_DIRS)
	# Get include directories from pkg-config for each package separately to avoid failures
	set(DEPS_INCLUDE_DIRS "")

	# glib-2.0
	execute_process(
		COMMAND pkg-config --cflags-only-I glib-2.0
		OUTPUT_VARIABLE GLIB_FLAGS
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE GLIB_RESULT
		ERROR_QUIET
	)
	if(GLIB_RESULT EQUAL 0 AND GLIB_FLAGS)
		string(REPLACE "-I" "" GLIB_DIRS "${GLIB_FLAGS}")
		separate_arguments(GLIB_DIRS)
		list(APPEND DEPS_INCLUDE_DIRS ${GLIB_DIRS})
	endif()

	# glibmm-2.4
	execute_process(
		COMMAND pkg-config --cflags-only-I glibmm-2.4
		OUTPUT_VARIABLE GLIBMM_FLAGS
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE GLIBMM_RESULT
		ERROR_QUIET
	)
	if(GLIBMM_RESULT EQUAL 0 AND GLIBMM_FLAGS)
		string(REPLACE "-I" "" GLIBMM_DIRS "${GLIBMM_FLAGS}")
		separate_arguments(GLIBMM_DIRS)
		list(APPEND DEPS_INCLUDE_DIRS ${GLIBMM_DIRS})
	endif()

	# libsigrok
	execute_process(
		COMMAND pkg-config --cflags-only-I libsigrok
		OUTPUT_VARIABLE LIBSIGROK_FLAGS
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE LIBSIGROK_RESULT
		ERROR_QUIET
	)
	if(LIBSIGROK_RESULT EQUAL 0 AND LIBSIGROK_FLAGS)
		string(REPLACE "-I" "" LIBSIGROK_DIRS "${LIBSIGROK_FLAGS}")
		separate_arguments(LIBSIGROK_DIRS)
		list(APPEND DEPS_INCLUDE_DIRS ${LIBSIGROK_DIRS})
	endif()

	# libusb-1.0
	execute_process(
		COMMAND pkg-config --cflags-only-I libusb-1.0
		OUTPUT_VARIABLE LIBUSB_FLAGS
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE LIBUSB_RESULT
		ERROR_QUIET
	)
	if(LIBUSB_RESULT EQUAL 0 AND LIBUSB_FLAGS)
		string(REPLACE "-I" "" LIBUSB_DIRS "${LIBUSB_FLAGS}")
		separate_arguments(LIBUSB_DIRS)
		list(APPEND DEPS_INCLUDE_DIRS ${LIBUSB_DIRS})
	endif()

	# hidapi
	execute_process(
		COMMAND pkg-config --cflags-only-I hidapi-hidraw hidapi-libusb
		OUTPUT_VARIABLE HIDAPI_FLAGS
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE HIDAPI_RESULT
		ERROR_QUIET
	)
	if(HIDAPI_RESULT EQUAL 0 AND HIDAPI_FLAGS)
		string(REPLACE "-I" "" HIDAPI_DIRS "${HIDAPI_FLAGS}")
		separate_arguments(HIDAPI_DIRS)
		list(APPEND DEPS_INCLUDE_DIRS ${HIDAPI_DIRS})
	endif()

	include_directories(${DEPS_INCLUDE_DIRS})
	message(STATUS "Manually added include dirs: ${DEPS_INCLUDE_DIRS}")
endif()

#===============================================================================
#= Linker Configuration
#-------------------------------------------------------------------------------

link_directories(${Boost_LIBRARY_DIRS})

set(PULSEVIEW_LINK_LIBS
	${Boost_LIBRARIES}
	${QT_LIBRARIES}
	${CMAKE_THREAD_LIBS_INIT}
	${LIBATOMIC_LIBRARY}
	${PULSEVIEW_HIDAPI_LIBRARY}
)

if(STATIC_PKGDEPS_LIBS)
	link_directories(${PKGDEPS_STATIC_LIBRARY_DIRS})
	list(APPEND PULSEVIEW_LINK_LIBS ${PKGDEPS_STATIC_LDFLAGS})
else()
	link_directories(${PKGDEPS_LIBRARY_DIRS})
	if(WIN32 AND PULSEVIEW_QT_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
		# 静态 Qt 可能传入 -static；其余依赖仍按动态库解析。
		list(APPEND PULSEVIEW_LINK_LIBS "-Wl,-Bdynamic")
	endif()
	list(APPEND PULSEVIEW_LINK_LIBS ${PKGDEPS_LIBRARIES})
endif()

if(WIN32 AND PULSEVIEW_QT_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
	# On Windows we need to statically link the libqsvg imageformat
	# plugin (and the QtSvg component) for SVG graphics/icons to work.
	# We also need QWindowsIntegrationPlugin, Qt5PlatformSupport (only for
	# Qt < 5.8.0), and all Qt libs and their dependencies.
	add_definitions(-DQT_STATICPLUGIN)
	list(APPEND PULSEVIEW_LINK_LIBS Qt5::QSvgPlugin)
	list(APPEND PULSEVIEW_LINK_LIBS Qt5::QWindowsIntegrationPlugin)
	if(Qt5Gui_VERSION VERSION_LESS 5.8.0)
		list(APPEND PULSEVIEW_LINK_LIBS -lQt5PlatformSupport)
	endif()
	list(APPEND PULSEVIEW_LINK_LIBS ${QT5ALL_LDFLAGS})
endif()

if(WIN32)
	list(APPEND PULSEVIEW_LINK_LIBS dwmapi)
endif()

if(ENABLE_STACKTRACE)
	list(APPEND PULSEVIEW_LINK_LIBS ${CMAKE_DL_LIBS} ${Backtrace_LIBRARIES})
	if(UNIX AND NOT APPLE)
		link_libraries("-no-pie")
	endif()
endif()

if(ANDROID)
	list(APPEND PULSEVIEW_LINK_LIBS "-llog")
endif()

set(INPUT_FILES_LIST ${pulseview_SOURCES} ${pulseview_RESOURCES_RCC})
if(ANDROID)
	add_library(${PROJECT_NAME} SHARED ${INPUT_FILES_LIST})
else()
	add_executable(${PROJECT_NAME} ${INPUT_FILES_LIST})
endif()

target_link_libraries(${PROJECT_NAME} ${PULSEVIEW_LINK_LIBS})
add_dependencies(${PROJECT_NAME} pulseview_translations)

# Rename output to LogicAnalyzer.exe
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME LogicAnalyzer)

if(APPLE)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_BUNDLE_NAME "U3LogicAnalyzer"
		MACOSX_BUNDLE_GUI_IDENTIFIER "org.sigrok.U3LogicAnalyzer"
		MACOSX_BUNDLE_SHORT_VERSION_STRING "${PV_VERSION_MAJOR}.${PV_VERSION_MINOR}.${PV_VERSION_MICRO}"
		MACOSX_BUNDLE_BUNDLE_VERSION "${PV_VERSION_MAJOR}.${PV_VERSION_MINOR}.${PV_VERSION_MICRO}"
	)
endif()

if(WIN32 AND NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug")
	# Pass -mwindows so that no "DOS box" opens when PulseView is started.
	set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "-mwindows")
endif()

#===============================================================================
#= Installation
#-------------------------------------------------------------------------------

# Install the executable.
install(TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	BUNDLE DESTINATION .
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# 精简源码包可能不包含手册，缺少文档不影响应用安装与打包。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/doc/pulseview.1")
	install(FILES doc/pulseview.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 COMPONENT doc)
endif()

# Install the desktop file.
install(FILES contrib/org.sigrok.PulseView.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# Install the AppData/AppStream file.
install(FILES contrib/org.sigrok.PulseView.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)

# Install the PulseView icons.
install(FILES icons/pulseview.png DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/48x48/apps)
install(FILES icons/pulseview.svg DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)

# Generate Windows installer script.
configure_file(contrib/pulseview_cross.nsi.in ${CMAKE_CURRENT_BINARY_DIR}/contrib/pulseview_cross.nsi @ONLY)

#===============================================================================
#= Packaging (handled by CPack)
#-------------------------------------------------------------------------------

set(CPACK_PACKAGE_VERSION_MAJOR ${PV_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PV_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PV_VERSION_MICRO})
set(CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/README)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/COPYING)
set(CPACK_SOURCE_IGNORE_FILES ${CMAKE_CURRENT_BINARY_DIR} ".gitignore" ".git")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${PV_VERSION_STRING}")
set(CPACK_SOURCE_GENERATOR "TGZ")

include(CPack)

#===============================================================================
#= Tests
#-------------------------------------------------------------------------------

if(ENABLE_TESTS)
	add_subdirectory(test)
	enable_testing()
	add_test(test ${CMAKE_CURRENT_BINARY_DIR}/test/pulseview-test)
endif()
