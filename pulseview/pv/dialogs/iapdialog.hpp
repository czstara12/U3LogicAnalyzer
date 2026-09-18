/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2026 Q2H2 
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PULSEVIEW_PV_DIALOGS_IAPDIALOG_HPP
#define PULSEVIEW_PV_DIALOGS_IAPDIALOG_HPP

#include <QDialog>
#include <QThread>
#include <QMutex>
#include <QString>
#include <QProgressBar>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <memory>
#include <cstdint>

/* IAP命令定义 */
#define CMD_IAP_PROM    0x80    /* 编程命令 */
#define CMD_IAP_ERASE   0x81    /* 擦除命令 */
#define CMD_IAP_VERIFY  0x82    /* 校验命令 */
#define CMD_IAP_END     0x83    /* 结束标志 */
#define CMD_IAP_SUCC    0x84    /* 成功标志 */

/* IAP错误码 */
#define IAP_ERR_UNKNOWN        0   /* 未知错误 */
#define IAP_ERR_OVERTIME       1   /* 超时 */
#define IAP_ERR_CHECK          2   /* 校验和不通过 */
#define IAP_ERR_ADDR           3   /* 地址错误 */
#define IAP_ERR_ERASE_FAIL     4   /* 擦除失败 */
#define IAP_ERR_PROG_NO_ERASE  5   /* 未擦除直接写 */
#define IAP_ERR_WRITE_FAIL     6   /* 写入失败 */
#define IAP_ERR_VERIFY         7   /* 校验失败 */

#define IAP_DATA_LEN           64  /* IAP数据包长度 */
#define HID_PACKET_LEN         65  /* HID数据包长度(含ReportID) */
#define HID_REPORT_ID          0   /* HID Report ID */
#define HID_TIMEOUT_MS         500 /* HID超时时间(ms) */

namespace pv {

class Session;

namespace dialogs {

class IAPWorker;

/**
 * IAP固件升级
 *
 */
class IAPDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * 构造函数
     * @param parent 父窗口
     * @param device_index CH32H417设备索引（用于ch375通信）
     */
    IAPDialog(QWidget *parent, int device_index);
    ~IAPDialog();

    /**
     * 设置IAP模式的VID和PID
     * @param vid
     * @param pid
     */
    void set_iap_vid_pid(uint16_t vid, uint16_t pid);

    /**
     * 设置固件文件路径
     * @param path 固件文件路径
     */
    void set_firmware_path(const QString &path);

Q_SIGNALS:
    /**
     * 升级完成信号
     * @param success 是否成功
     */
    void upgrade_finished(bool success);

private Q_SLOTS:
    void on_browse_clicked();
    void on_start_clicked();
    void on_cancel_clicked();

    void on_progress_updated(int percent, const QString &status);
    void on_log_message(const QString &message);
    void on_upgrade_finished(bool success);

private:
    void setup_ui();
    void log(const QString &message);
    void update_progress(int percent, const QString &status);

private:
    int device_index_;              /* CH32H417设备索引 */
    uint16_t iap_vid_;              /* IAP模式VID */
    uint16_t iap_pid_;              /* IAP模式PID */
    QString firmware_path_;         /* 固件文件路径 */

    QLineEdit *file_edit_;          /* 文件路径输入框 */
    QPushButton *browse_btn_;       /* 浏览按钮 */
    QPushButton *start_btn_;        /* 开始按钮 */
    QPushButton *cancel_btn_;       /* 取消按钮 */
    QProgressBar *progress_bar_;    /* 进度条 */
    QLabel *status_label_;          /* 状态标签 */
    QTextEdit *log_text_;           /* 日志文本框 */

    IAPWorker *worker_;             /* 升级工作对象 */
    QThread *worker_thread_;        /* 升级工作线程 */
    bool is_upgrading_;             /* 是否正在升级 */
};

/**
 * IAP升级工作线程
 *
 * 在后台线程中执行IAP升级操作，避免阻塞UI
 */
class IAPWorker : public QObject
{
    Q_OBJECT

public:
    /**
     * 构造函数
     * @param device_index CH32H417设备索引
     * @param firmware_path 固件文件路径
     * @param iap_vid IAP模式VID
     * @param iap_pid IAP模式PID
     */
    IAPWorker(int device_index, const QString &firmware_path,
              uint16_t iap_vid, uint16_t iap_pid, bool already_in_hid_mode = false);
    ~IAPWorker();

    void start();

Q_SIGNALS:
    void progress_updated(int percent, const QString &status);
    void log_message(const QString &message);
    void finished(bool success);

public Q_SLOTS:
    void do_work();
    void cancel();

private:
    /* HID设备操作 */
    bool init_hid();
    void close_hid();
    bool find_hid_device();
    bool hid_write(const uint8_t *data, size_t len);
    bool hid_read(uint8_t *data, size_t len, int timeout_ms);

    /* IAP操作 */
    bool enter_iap_mode();
    bool erase_flash(uint32_t start_addr);
    bool program_flash(const uint8_t *data, size_t len);
    bool verify_flash(uint32_t start_addr, const uint8_t *data, size_t len);
    bool end_upgrade();

    /* 文件处理 */
    bool read_firmware_file(QByteArray &out_data, uint32_t &start_addr);
    bool hex_to_bin(const QByteArray &hex_data, QByteArray &bin_data, uint32_t &start_addr);

    /* 日志辅助 */
    void log(const QString &message);
    void update_progress(int percent, const QString &status);

private:
    int device_index_;
    QString firmware_path_;
    uint16_t iap_vid_;
    uint16_t iap_pid_;

    void *hid_handle_;              /* hidapi设备句柄 */
    bool already_in_hid_mode_;      /* 设备是否已处于HID模式 */
    bool cancelled_;                /* 取消标志 */
    QMutex mutex_;                  /* 互斥锁 */
};

} // namespace dialogs
} // namespace pv

#endif // PULSEVIEW_PV_DIALOGS_IAPDIALOG_HPP
