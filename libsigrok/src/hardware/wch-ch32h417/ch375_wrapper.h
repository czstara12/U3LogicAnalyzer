/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on libsigrok.
 *
 * Copyright (C) 2026 Q2H2
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * CH375DLL动态加载封装
 * 使用LoadLibrary动态加载DLL函数，无需.lib文件
 * 使用C标准类型，适配mingw-w64交叉编译
 */

#ifndef LIBSIGROK_HARDWARE_WCH_CH32H417_CH375_WRAPPER_H
#define LIBSIGROK_HARDWARE_WCH_CH32H417_CH375_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 常量定义
 * ============================================================================ */

/* CH375数据包长度 */
#define CH375_PACKET_LENGTH     64

/* CH375端点地址 */
#define CH375_ENDP_AUX_UP       0x81    /* 中断上传端点 */
#define CH375_ENDP_AUX_DOWN     0x01    /* 辅助下传端点 */
#define CH375_ENDP_DATA_UP      0x82    /* 数据块上传端点 */
#define CH375_ENDP_DATA_DOWN    0x02    /* 数据块下传端点 */

/* 最大设备数量 */
#define CH375_MAX_NUMBER        16

/* 无效句柄值 */
#define CH375_INVALID_HANDLE    ((void *)-1)

/* 返回值定义 */
#define CH375_TRUE              1
#define CH375_FALSE             0

/* 设备事件定义 (用于CH375SetDeviceNotify回调) */
#define CH375_DEVICE_ARRIVAL        3   /* 设备插入事件 */
#define CH375_DEVICE_REMOVE_PEND    1   /* 设备将要拔出 */
#define CH375_DEVICE_REMOVE         0   /* 设备拔出事件 */

/* 默认缓冲上传大小 (1MB) */
#define CH375_DEFAULT_TRANSFER_SIZE    (1024 * 1024)

/* ============================================================================
 * 函数指针类型定义
 * ============================================================================ */

/* 设备管理 */
typedef void* (*CH375OpenDevice_func)(unsigned long iIndex);
typedef void (*CH375CloseDevice_func)(unsigned long iIndex);
typedef unsigned long (*CH375GetVersion_func)(void);
typedef unsigned long (*CH375GetDrvVersion_func)(void);
typedef unsigned long (*CH375GetUsbID_func)(unsigned long iIndex);
typedef void* (*CH375GetDeviceName_func)(unsigned long iIndex);

/* 设备描述符 */
typedef int (*CH375GetDeviceDescr_func)(unsigned long iIndex, void *oBuffer, unsigned long *ioLength);
typedef int (*CH375GetConfigDescr_func)(unsigned long iIndex, void *oBuffer, unsigned long *ioLength);

/* 设备控制 */
typedef int (*CH375ResetDevice_func)(unsigned long iIndex);
typedef int (*CH375SetExclusive_func)(unsigned long iIndex, unsigned long iExclusive);

/* 超时设置 */
typedef int (*CH375SetTimeout_func)(unsigned long iIndex, unsigned long iWriteTimeout, unsigned long iReadTimeout);
typedef int (*CH375SetTimeoutEx_func)(unsigned long iIndex, unsigned long iWriteTimeout, unsigned long iReadTimeout,
                                      unsigned long iAuxTimeout, unsigned long iInterTimeout);

/* 端点1直接上下传 */
typedef int (*CH375WriteRead_func)(unsigned long iIndex, void *iBuffer, void *oBuffer, unsigned long *ioLength);

/* 端点读写 */
typedef int (*CH375WriteEndP_func)(unsigned long iIndex, unsigned long iEndP, void *iBuffer, unsigned long *ioLength);
typedef int (*CH375ReadEndP_func)(unsigned long iIndex, unsigned long iEndP, void *oBuffer, unsigned long *ioLength);

/* 端点控制 */
typedef int (*CH375AbortEndPRead_func)(unsigned long iIndex, unsigned long iEndP);
typedef int (*CH375AbortEndPWrite_func)(unsigned long iIndex, unsigned long iEndP);
typedef int (*CH375ResetInEndP_func)(unsigned long iIndex, unsigned long iEndP);
typedef int (*CH375ResetOutEndP_func)(unsigned long iIndex, unsigned long iEndP);

/* 数据端点读写 */
typedef int (*CH375ReadData_func)(unsigned long iIndex, void *oBuffer, unsigned long *ioLength);
typedef int (*CH375WriteData_func)(unsigned long iIndex, void *iBuffer, unsigned long *ioLength);
typedef int (*CH375AbortRead_func)(unsigned long iIndex);
typedef int (*CH375AbortWrite_func)(unsigned long iIndex);

/* 缓冲上传模式 */
typedef int (*CH375SetBufUpload_func)(unsigned long iIndex, unsigned long iEnableOrClear);
typedef long (*CH375QueryBufUpload_func)(unsigned long iIndex);
typedef int (*CH375SetBufUploadEx_func)(unsigned long iIndex, unsigned long iEnableOrClear,
                                        unsigned long iEndP, unsigned long TransferSize);
typedef int (*CH375QueryBufUploadEx_func)(unsigned long iIndex, unsigned long iEndP,
                                          unsigned long *oTransferCount, unsigned long *oTotalDataLen);
typedef int (*CH375ClearBufUpload_func)(unsigned long iIndex, unsigned long iEndP);

/* 缓冲下传模式 */
typedef int (*CH375SetBufDownload_func)(unsigned long iIndex, unsigned long iEnableOrClear);
typedef long (*CH375QueryBufDownload_func)(unsigned long iIndex);
typedef int (*CH375SetBufDownloadEx_func)(unsigned long iIndex, unsigned long iEnableOrClear,
                                          unsigned long iEndP, unsigned long TransferSize);

/* IO模式设置 */
typedef int (*CH375SetIOMode_func)(unsigned long iIndex, unsigned long iData);

/* 设备事件通知回调类型 */
typedef void (*CH375NotifyCallback)(unsigned long iEventStatus);

/* 设备事件通知 */
typedef int (*CH375SetDeviceNotify_func)(unsigned long iIndex, char *iDeviceID, CH375NotifyCallback iNotifyRoutine);

/* CH375驱动命令 */
typedef int (*CH375DriverCommand_func)(unsigned long iIndex, void *ioCommand);

/* CH375驱动命令相关定义 */
#define mFuncSetIOMode          0x00000012    /* 设置IO模式 */

/* WIN32命令结构 */
#pragma pack(push, 1)
typedef struct _mWIN32_COMMAND {
    unsigned long mFunction;        /* 功能代码 */
    unsigned long mLength;          /* 数据长度 */
    unsigned char mBuffer[64];      /* 数据缓冲区 */
} mWIN32_COMMAND;
#pragma pack(pop)

/* ============================================================================
 * 封装接口函数声明
 * ============================================================================ */

/* DLL加载/卸载 */
int ch375_init(void);
void ch375_cleanup(void);
int ch375_is_loaded(void);

/* 设备管理 */
void* ch375_open_device(unsigned long index);
void ch375_close_device(unsigned long index);
unsigned long ch375_get_usb_id(unsigned long index);
const char* ch375_get_device_name(unsigned long index);
int ch375_get_device_descr(unsigned long index, void *buffer, unsigned long *length);
int ch375_reset_device(unsigned long index);

/* 超时设置 */
int ch375_set_timeout(unsigned long index, unsigned long writeTimeout, unsigned long readTimeout);
int ch375_set_timeout_ex(unsigned long index, unsigned long writeTimeout, unsigned long readTimeout,
                         unsigned long auxTimeout, unsigned long interTimeout);

/* 端点1直接上下传 (命令下发和响应回读) */
int ch375_write_read(unsigned long index, void *iBuffer, void *oBuffer, unsigned long *ioLength);

/* 端点读写 (分开写读) - 参数pipe_num为管道号(1,2,3) */
int ch375_write_endpoint(unsigned long index, unsigned long pipe_num, void *buffer, unsigned long *length);
int ch375_read_endpoint(unsigned long index, unsigned long pipe_num, void *buffer, unsigned long *length);

/* 端点控制 - 参数pipe_num为管道号(1,2,3) */
int ch375_abort_endpoint_read(unsigned long index, unsigned long pipe_num);
int ch375_abort_endpoint_write(unsigned long index, unsigned long pipe_num);
int ch375_reset_in_endpoint(unsigned long index, unsigned long pipe_num);
int ch375_reset_out_endpoint(unsigned long index, unsigned long pipe_num);

/* 缓冲上传模式 (端点2/3数据传输) - 参数pipe_num为管道号(1,2,3) */
int ch375_set_buf_upload_ex(unsigned long index, unsigned long enable, unsigned long pipe_num, unsigned long transferSize);
int ch375_query_buf_upload_ex(unsigned long index, unsigned long pipe_num, unsigned long *transferCount, unsigned long *totalDataLen);
int ch375_clear_buf_upload(unsigned long index, unsigned long pipe_num);

/* 缓冲下传模式 - 参数pipe_num为管道号(1,2,3) */
int ch375_set_buf_download_ex(unsigned long index, unsigned long enable, unsigned long pipe_num, unsigned long transferSize);

/* IO模式设置 - 设置缓冲上传模式时读写是否同步 */
int ch375_set_io_mode(unsigned long index, unsigned long sync);

/* 设备事件通知 - 注册/取消设备插入拔出回调 */
int ch375_set_device_notify(unsigned long index, char *deviceID, CH375NotifyCallback callback);

/* 数据端点读写 */
int ch375_read_data(unsigned long index, void *buffer, unsigned long *length);
int ch375_write_data(unsigned long index, void *buffer, unsigned long *length);
int ch375_abort_read(unsigned long index);
int ch375_abort_write(unsigned long index);

#ifdef __cplusplus
}
#endif

#endif /* LIBSIGROK_HARDWARE_WCH_CH32H417_CH375_WRAPPER_H */
