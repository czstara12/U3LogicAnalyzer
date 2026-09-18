/**
 * @file
 * @brief 协议解码库的 CMake 平台配置，独立于 Autotools 模板。
 */
#pragma once
#cmakedefine WORDS_BIGENDIAN 1
#define CONF_HOST "@CONF_HOST@"
#define PACKAGE_NAME "@PACKAGE_NAME@"
#define PACKAGE_STRING "@PACKAGE_STRING@"
#define PACKAGE_TARNAME "@PACKAGE_TARNAME@"
#define PACKAGE_URL "@PACKAGE_URL@"
#define PACKAGE_VERSION "@PACKAGE_VERSION@"
#define PACKAGE_BUGREPORT "@PACKAGE_BUGREPORT@"
