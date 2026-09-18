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

#include "iapdialog.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QFile>
#include <QDebug>
#include <QDateTime>

#include "../../../libsigrok/src/hardware/wch-ch32h417/ch375_wrapper.h"

#include <hidapi.h>

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace pv {
namespace dialogs {

/* IAPDialog implementation */

IAPDialog::IAPDialog(QWidget *parent, int device_index)
    : QDialog(parent)
    , device_index_(device_index)
    , iap_vid_(0x1A86)
    , iap_pid_(0xFE17)
    , worker_(nullptr)
    , worker_thread_(nullptr)
    , is_upgrading_(false)
{
    setup_ui();
}

IAPDialog::~IAPDialog()
{
    if (worker_thread_ && worker_thread_->isRunning()) {
        if (worker_) {
            worker_->cancel();
        }
        worker_thread_->quit();
        if (!worker_thread_->wait(2000)) {
            worker_thread_->terminate();
            worker_thread_->wait();
        }
    }
    worker_ = nullptr;
    worker_thread_ = nullptr;
}

void IAPDialog::set_iap_vid_pid(uint16_t vid, uint16_t pid)
{
    iap_vid_ = vid;
    iap_pid_ = pid;
}

void IAPDialog::set_firmware_path(const QString &path)
{
    firmware_path_ = path;
    if (file_edit_) {
        file_edit_->setText(path);
    }
}

void IAPDialog::setup_ui()
{
    setWindowTitle(tr("固件升级"));
    setMinimumWidth(500);
    setMinimumHeight(400);

    QVBoxLayout *main_layout = new QVBoxLayout(this);

    QGridLayout *file_layout = new QGridLayout();
    file_layout->addWidget(new QLabel(tr("固件文件:")), 0, 0);
    file_edit_ = new QLineEdit();
    file_edit_->setPlaceholderText(tr("选择BIN或HEX格式固件文件"));
    file_layout->addWidget(file_edit_, 0, 1);
    browse_btn_ = new QPushButton(tr("浏览..."));
    file_layout->addWidget(browse_btn_, 0, 2);
    main_layout->addLayout(file_layout);

    QHBoxLayout *progress_layout = new QHBoxLayout();
    progress_bar_ = new QProgressBar();
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_layout->addWidget(progress_bar_);
    status_label_ = new QLabel(tr("就绪"));
    status_label_->setMinimumWidth(100);
    progress_layout->addWidget(status_label_);
    main_layout->addLayout(progress_layout);

    QLabel *log_label = new QLabel(tr("升级日志:"));
    main_layout->addWidget(log_label);
    log_text_ = new QTextEdit();
    log_text_->setReadOnly(true);
    log_text_->setMaximumHeight(200);
    main_layout->addWidget(log_text_);

    QHBoxLayout *btn_layout = new QHBoxLayout();
    btn_layout->addStretch();
    start_btn_ = new QPushButton(tr("开始升级"));
    start_btn_->setDefault(true);
    cancel_btn_ = new QPushButton(tr("取消"));
    btn_layout->addWidget(start_btn_);
    btn_layout->addWidget(cancel_btn_);
    main_layout->addLayout(btn_layout);

    connect(browse_btn_, &QPushButton::clicked, this, &IAPDialog::on_browse_clicked);
    connect(start_btn_, &QPushButton::clicked, this, &IAPDialog::on_start_clicked);
    connect(cancel_btn_, &QPushButton::clicked, this, &IAPDialog::on_cancel_clicked);
}

void IAPDialog::log(const QString &message)
{
    printf("[IAP] %s\n", message.toLocal8Bit().constData());
    log_text_->append(QDateTime::currentDateTime().toString("[hh:mm:ss] ") + message);
}

void IAPDialog::update_progress(int percent, const QString &status)
{
    progress_bar_->setValue(percent);
    status_label_->setText(status);
}

void IAPDialog::on_browse_clicked()
{
    QString file_name = QFileDialog::getOpenFileName(
        this,
        tr("选择固件文件"),
        QString(),
        tr("固件文件 (*.bin *.hex);;所有文件 (*.*)")
    );

    if (!file_name.isEmpty()) {
        file_edit_->setText(file_name);
        firmware_path_ = file_name;
    }
}

void IAPDialog::on_start_clicked()
{
    firmware_path_ = file_edit_->text();

    if (firmware_path_.isEmpty()) {
        QMessageBox::warning(this, tr("错误"), tr("请选择固件文件"));
        return;
    }

    if (!QFile::exists(firmware_path_)) {
        QMessageBox::warning(this, tr("错误"), tr("固件文件不存在"));
        return;
    }

    if (is_upgrading_) {
        return;
    }

    is_upgrading_ = true;
    start_btn_->setEnabled(false);
    browse_btn_->setEnabled(false);
    file_edit_->setEnabled(false);
    cancel_btn_->setText(tr("取消"));

    log(tr("开始固件升级..."));
    log(tr("设备索引: %1").arg(device_index_));
    log(tr("IAP VID: 0x%1, PID: 0x%2").arg(iap_vid_, 4, 16, QChar('0')).arg(iap_pid_, 4, 16, QChar('0')));

    /* 创建工作线程 */
    worker_ = new IAPWorker(device_index_, firmware_path_, iap_vid_, iap_pid_);
    worker_thread_ = new QThread();
    worker_->moveToThread(worker_thread_);

    connect(worker_thread_, &QThread::started, worker_, &IAPWorker::do_work);
    connect(worker_, &IAPWorker::progress_updated, this, &IAPDialog::on_progress_updated);
    connect(worker_, &IAPWorker::log_message, this, &IAPDialog::on_log_message);
    connect(worker_, &IAPWorker::finished, this, &IAPDialog::on_upgrade_finished);
    connect(worker_, &IAPWorker::finished, worker_, &IAPWorker::deleteLater);
    connect(worker_, &IAPWorker::finished, worker_thread_, &QThread::quit);
    connect(worker_thread_, &QThread::finished, worker_thread_, &QThread::deleteLater);

    worker_thread_->start();
}

void IAPDialog::on_cancel_clicked()
{
    if (is_upgrading_ && worker_) {
        worker_->cancel();
        log(tr("正在取消升级..."));
    } else {
        reject();
    }
}

void IAPDialog::on_progress_updated(int percent, const QString &status)
{
    update_progress(percent, status);
}

void IAPDialog::on_log_message(const QString &message)
{
    log(message);
}

void IAPDialog::on_upgrade_finished(bool success)
{
    is_upgrading_ = false;
    start_btn_->setEnabled(true);
    browse_btn_->setEnabled(true);
    file_edit_->setEnabled(true);
    cancel_btn_->setText(tr("关闭"));

    worker_ = nullptr;
    worker_thread_ = nullptr;

    if (success) {
        update_progress(100, tr("升级成功"));
        log(tr("固件升级成功！"));
        QMessageBox::information(this, tr("成功"), tr("固件升级成功！\n设备将重新启动。"));
    } else {
        update_progress(0, tr("升级失败"));
        log(tr("固件升级失败！"));
        QMessageBox::critical(this, tr("失败"), tr("固件升级失败！\n请检查设备连接和固件文件。"));
    }
}

/* IAPWorker implementation */

IAPWorker::IAPWorker(int device_index, const QString &firmware_path,
                     uint16_t iap_vid, uint16_t iap_pid, bool already_in_hid_mode)
    : device_index_(device_index)
    , firmware_path_(firmware_path)
    , iap_vid_(iap_vid)
    , iap_pid_(iap_pid)
    , hid_handle_(nullptr)
    , already_in_hid_mode_(already_in_hid_mode)
    , cancelled_(false)
{
}

IAPWorker::~IAPWorker()
{
    close_hid();
}

void IAPWorker::start()
{
    do_work();
}

void IAPWorker::cancel()
{
    QMutexLocker locker(&mutex_);
    cancelled_ = true;
}

void IAPWorker::log(const QString &message)
{
    printf("[IAPWorker] %s\n", message.toLocal8Bit().constData());
    Q_EMIT log_message(message);
}

void IAPWorker::update_progress(int percent, const QString &status)
{
    Q_EMIT progress_updated(percent, status);
}

bool IAPWorker::init_hid()
{
    static bool hid_initialized = false;
    if (!hid_initialized) {
        if (::hid_init() != 0) {
            log(tr("hidapi初始化失败"));
            return false;
        }
        hid_initialized = true;
    }

    hid_handle_ = ::hid_open(iap_vid_, iap_pid_, NULL);
    if (!hid_handle_) {
        log(tr("未找到IAP设备 (VID=0x%1, PID=0x%2)")
            .arg(iap_vid_, 4, 16, QChar('0'))
            .arg(iap_pid_, 4, 16, QChar('0')));
        return false;
    }

    log(tr("找到IAP设备"));
    return true;
}

void IAPWorker::close_hid()
{
    if (hid_handle_) {
        ::hid_set_nonblocking((hid_device*)hid_handle_, 1);
        ::hid_close((hid_device*)hid_handle_);
        hid_handle_ = nullptr;
    }
}

bool IAPWorker::hid_write(const uint8_t *data, size_t len)
{
    if (!hid_handle_) return false;

    ::hid_set_nonblocking((hid_device*)hid_handle_, 1);

    uint8_t send_buf[HID_PACKET_LEN] = {0};
    send_buf[0] = HID_REPORT_ID;

    if (len > 0 && len <= (HID_PACKET_LEN - 1)) {
        memcpy(send_buf + 1, data, len);
    }

    int elapsed = 0;
    int check_interval = 100;
    int timeout_ms = HID_TIMEOUT_MS;

    while (elapsed < timeout_ms) {
        {
            QMutexLocker locker(&mutex_);
            if (cancelled_) {
                log(tr("写入被取消"));
                ::hid_set_nonblocking((hid_device*)hid_handle_, 0);
                return false;
            }
        }

        int result = ::hid_write((hid_device*)hid_handle_, send_buf, HID_PACKET_LEN);
        if (result > 0) {
            ::hid_set_nonblocking((hid_device*)hid_handle_, 0);
            return true;
        }
        if (result == 0) {
            QThread::msleep(check_interval);
            elapsed += check_interval;
            continue;
        }
        log(tr("HID写入失败"));
        ::hid_set_nonblocking((hid_device*)hid_handle_, 0);
        return false;
    }

    ::hid_set_nonblocking((hid_device*)hid_handle_, 0);
    log(tr("HID写入超时 (%1ms)").arg(timeout_ms));
    return false;
}

bool IAPWorker::hid_read(uint8_t *data, size_t len, int timeout_ms)
{
    if (!hid_handle_) return false;

    uint8_t recv_buf[HID_PACKET_LEN] = {0};

    int elapsed = 0;
    int check_interval = 100;

    while (elapsed < timeout_ms) {
        {
            QMutexLocker locker(&mutex_);
            if (cancelled_) {
                log(tr("读取被取消"));
                return false;
            }
        }

        int result = ::hid_read_timeout((hid_device*)hid_handle_, recv_buf, HID_PACKET_LEN, check_interval);
        if (result > 0) {
            if (len <= (HID_PACKET_LEN - 1)) {
                memcpy(data, recv_buf + 1, len);
            }
            return true;
        }
        if (result == 0) {
            elapsed += check_interval;
            continue;
        }
        log(tr("HID读取错误"));
        return false;
    }

    log(tr("HID读取超时 (%1ms)").arg(timeout_ms));
    return false;
}

bool IAPWorker::enter_iap_mode()
{
#ifndef _WIN32
    log(tr("当前平台不支持通过 CH375 驱动切换 IAP 模式，请先将设备切换到 HID 升级模式。"));
    return false;
#else
    {
        QMutexLocker locker(&mutex_);
        if (cancelled_) {
            log(tr("用户取消升级"));
            return false;
        }
    }

    log(tr("发送IAP模式进入命令..."));

    uint8_t cmd[64] = {0};
    cmd[0] = 0xAE;  /* CMD_ENTER_IAP */
    cmd[1] = 0;

    unsigned long io_length = 64;
    if (!ch375_write_endpoint(device_index_, 1, cmd, &io_length)) {
        log(tr("发送IAP命令失败"));
        return false;
    }

    log(tr("IAP命令已发送，关闭CH375设备..."));

    ch375_close_device(device_index_);

    log(tr("CH375设备已关闭，等待设备切换模式..."));

    for (int i = 0; i < 5; i++) {
        QThread::msleep(100);
        {
            QMutexLocker locker(&mutex_);
            if (cancelled_) {
                log(tr("用户取消升级"));
                return false;
            }
        }
    }

    return true;
#endif
}

bool IAPWorker::erase_flash(uint32_t start_addr)
{
    {
        QMutexLocker locker(&mutex_);
        if (cancelled_) {
            log(tr("用户取消升级"));
            return false;
        }
    }

    log(tr("擦除FLASH...地址: 0x%1").arg(start_addr, 8, 16, QChar('0')));
    update_progress(20, tr("擦除FLASH..."));

    uint8_t cmd[64] = {0};
    cmd[0] = CMD_IAP_ERASE;
    cmd[1] = 0;
    cmd[2] = (start_addr >> 0) & 0xFF;
    cmd[3] = (start_addr >> 8) & 0xFF;
    cmd[4] = (start_addr >> 16) & 0xFF;
    cmd[5] = (start_addr >> 24) & 0xFF;

    if (!hid_write(cmd, 6)) {
        log(tr("发送擦除命令失败"));
        return false;
    }

    uint8_t resp[64] = {0};
    if (!hid_read(resp, 2, 5000)) {
        log(tr("擦除响应超时"));
        return false;
    }

    if (resp[0] != 0x00 || resp[1] != 0x00) {
        log(tr("擦除失败，错误码: %1").arg(resp[1]));
        return false;
    }

    log(tr("擦除成功"));
    return true;
}

bool IAPWorker::program_flash(const uint8_t *data, size_t len)
{
    log(tr("编程FLASH...数据长度: %1字节").arg(len));
    update_progress(30, tr("编程FLASH..."));

    size_t offset = 0;
    size_t total = len;
    size_t max_data_len = 62;
    uint16_t retry_count = 0;

    while (offset < len) {
        {
            QMutexLocker locker(&mutex_);
            if (cancelled_) {
                log(tr("用户取消升级"));
                return false;
            }
        }

        size_t chunk_size = (len - offset) > max_data_len ? max_data_len : (len - offset);

        uint8_t cmd[64] = {0};
        cmd[0] = CMD_IAP_PROM;
        cmd[1] = (uint8_t)chunk_size;
        memcpy(cmd + 2, data + offset, chunk_size);

        size_t cmd_len = 2 + chunk_size;
        if (!hid_write(cmd, cmd_len)) {
            log(tr("写入数据失败，偏移: %1").arg(offset));
            retry_count++;
            if (retry_count > 3) {
                return false;
            }
            QThread::msleep(100);
            continue;
        }

        uint8_t resp[64] = {0};
        if (!hid_read(resp, 2, HID_TIMEOUT_MS)) {
            log(tr("编程响应超时，偏移: %1").arg(offset));
            retry_count++;
            if (retry_count > 3) {
                return false;
            }
            QThread::msleep(100);
            continue;
        }

        if (resp[0] != 0x00 || resp[1] != 0x00) {
            log(tr("编程失败，错误码: %1，偏移: %2").arg(resp[1]).arg(offset));
            retry_count++;
            if (retry_count > 3) {
                return false;
            }
            QThread::msleep(100);
            continue;
        }

        retry_count = 0;
        offset += chunk_size;

        /* 更新进度 (30% - 70%) */
        int progress = 30 + (int)(40 * offset / total);
        update_progress(progress, tr("编程FLASH... %1%").arg(offset * 100 / total));
    }

    uint8_t cmd[64] = {0};
    cmd[0] = CMD_IAP_PROM;
    cmd[1] = 0;
    hid_write(cmd, 2);

    uint8_t resp[64] = {0};
    hid_read(resp, 2, HID_TIMEOUT_MS);

    log(tr("编程完成"));
    return true;
}

bool IAPWorker::verify_flash(uint32_t start_addr, const uint8_t *data, size_t len)
{
    log(tr("校验FLASH..."));
    update_progress(70, tr("校验FLASH..."));

    size_t offset = 0;
    size_t total = len;
    size_t max_data_len = 56;
    uint32_t verify_addr = start_addr;
    uint16_t retry_count = 0;

    while (offset < len) {
        {
            QMutexLocker locker(&mutex_);
            if (cancelled_) {
                log(tr("用户取消升级"));
                return false;
            }
        }

        size_t chunk_size = (len - offset) > max_data_len ? max_data_len : (len - offset);

        uint8_t cmd[64] = {0};
        cmd[0] = CMD_IAP_VERIFY;
        cmd[1] = (uint8_t)chunk_size;
        cmd[2] = (verify_addr >> 0) & 0xFF;
        cmd[3] = (verify_addr >> 8) & 0xFF;
        cmd[4] = (verify_addr >> 16) & 0xFF;
        cmd[5] = (verify_addr >> 24) & 0xFF;
        memcpy(cmd + 6, data + offset, chunk_size);

        size_t cmd_len = 6 + chunk_size;  /* 命令头(2)+地址(4)+数据长度 */
        if (!hid_write(cmd, cmd_len)) {
            log(tr("发送校验命令失败，偏移: %1").arg(offset));
            retry_count++;
            if (retry_count > 3) {
                return false;
            }
            QThread::msleep(100);
            continue;
        }

        uint8_t resp[64] = {0};
        if (!hid_read(resp, 2, HID_TIMEOUT_MS)) {
            log(tr("校验响应超时，偏移: %1").arg(offset));
            retry_count++;
            if (retry_count > 3) {
                return false;
            }
            QThread::msleep(100);
            continue;
        }

        if (resp[0] != 0x00 || resp[1] != 0x00) {
            log(tr("校验失败，错误码: %1，地址: 0x%2")
                .arg(resp[1])
                .arg(verify_addr, 8, 16, QChar('0')));
            retry_count++;
            if (retry_count > 3) {
                return false;
            }
            QThread::msleep(100);
            continue;
        }

        retry_count = 0;
        offset += chunk_size;
        verify_addr += chunk_size;

        /* 更新进度 (70% - 95%) */
        int progress = 70 + (int)(25 * offset / total);
        update_progress(progress, tr("校验FLASH... %1%").arg(offset * 100 / total));
    }

    log(tr("校验成功"));
    return true;
}

bool IAPWorker::end_upgrade()
{
    log(tr("发送升级结束命令..."));
    update_progress(95, tr("完成升级..."));

    uint8_t cmd[64] = {0};
    cmd[0] = CMD_IAP_END;
    cmd[1] = 0;

    if (!hid_write(cmd, 2)) {
        log(tr("发送结束命令失败"));
        return false;
    }

    log(tr("升级结束命令已发送"));

    uint8_t resp[64] = {0};
    if (hid_read(resp, 2, 8000)) {
        if (resp[0] == CMD_IAP_SUCC) {
            log(tr("设备确认升级完成"));
        }
    } else {
        log(tr("设备已重启(无响应为正常情况)"));
    }

    return true;
}

bool IAPWorker::read_firmware_file(QByteArray &out_data, uint32_t &start_addr)
{
    QFile file(firmware_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        log(tr("无法打开固件文件: %1").arg(firmware_path_));
        return false;
    }

    QByteArray file_data = file.readAll();
    file.close();

    if (firmware_path_.toLower().endsWith(".hex")) {
        log(tr("检测到HEX格式文件，正在转换..."));
        if (!hex_to_bin(file_data, out_data, start_addr)) {
            log(tr("HEX转换失败"));
            return false;
        }
        log(tr("HEX转换成功，起始地址: 0x%1, 数据大小: %2字节")
            .arg(start_addr, 8, 16, QChar('0'))
            .arg(out_data.size()));
    } else {
        out_data = file_data;
        start_addr = 0;
        log(tr("读取BIN文件，数据大小: %1字节").arg(out_data.size()));
    }

    return true;
}

bool IAPWorker::hex_to_bin(const QByteArray &hex_data, QByteArray &bin_data, uint32_t &start_addr)
{
    bin_data.clear();
    bin_data.reserve(hex_data.size() / 2);

    QList<QByteArray> lines = hex_data.split('\n');
    uint32_t base_addr = 0;
    uint32_t first_data_addr = 0xFFFFFFFF;
    bool ok;

    for (const QByteArray &line : lines) {
        QString line_str = QString::fromLatin1(line).trimmed();
        if (line_str.isEmpty() || !line_str.startsWith(':')) {
            continue;
        }

        line_str = line_str.mid(1);

        uint8_t byte_count = line_str.mid(0, 2).toUInt(&ok, 16);
        if (!ok) continue;

        uint16_t addr_offset = line_str.mid(2, 4).toUShort(&ok, 16);
        if (!ok) continue;

        uint8_t record_type = line_str.mid(6, 2).toUInt(&ok, 16);
        if (!ok) continue;

        if (record_type == 0x00) {
            uint32_t full_addr = base_addr + addr_offset;

            if (first_data_addr == 0xFFFFFFFF) {
                first_data_addr = full_addr;
            }

            uint32_t write_addr = full_addr - first_data_addr;

            for (int i = 0; i < byte_count; i++) {
                uint8_t byte_val = line_str.mid(8 + i * 2, 2).toUInt(&ok, 16);
                if (ok) {
                    while (bin_data.size() <= (int)(write_addr + i)) {
                        bin_data.append((char)0xFF);
                    }
                    bin_data[write_addr + i] = (char)byte_val;
                }
            }
        } else if (record_type == 0x01) {
            break;
        } else if (record_type == 0x04) {
            base_addr = line_str.mid(8, 4).toUInt(&ok, 16) << 16;
        }
    }

    if (bin_data.size() > 0 && first_data_addr != 0xFFFFFFFF) {
        start_addr = first_data_addr;
        return true;
    }
    return false;
}

void IAPWorker::do_work()
{
    bool success = false;
    uint32_t start_addr = 0;

    update_progress(5, tr("读取固件文件..."));
    QByteArray firmware_data;
    if (!read_firmware_file(firmware_data, start_addr)) {
        Q_EMIT finished(false);
        return;
    }

    if (firmware_data.isEmpty()) {
        log(tr("固件数据为空"));
        Q_EMIT finished(false);
        return;
    }

    if (already_in_hid_mode_) {
        log(tr("设备已处于HID/IAP模式，跳过模式切换"));
        update_progress(10, tr("设备已处于IAP模式..."));
    } else {
        update_progress(10, tr("进入IAP模式..."));
        if (!enter_iap_mode()) {
            Q_EMIT finished(false);
            return;
        }
    }

    update_progress(15, tr("查找HID设备..."));

    for (int i = 0; i < 10; i++) {
        QThread::msleep(100);
        {
            QMutexLocker locker(&mutex_);
            if (cancelled_) {
                log(tr("用户取消升级"));
                close_hid();
                Q_EMIT finished(false);
                return;
            }
        }
    }

    int retry_count = 0;
    while (retry_count < 10) {
        {
            QMutexLocker locker(&mutex_);
            if (cancelled_) {
                log(tr("用户取消升级"));
                close_hid();
                Q_EMIT finished(false);
                return;
            }
        }

        if (init_hid()) {
            break;
        }
        retry_count++;
        log(tr("等待HID设备... (%1/10)").arg(retry_count));
        QThread::msleep(500);
    }

    if (!hid_handle_) {
        log(tr("无法找到IAP HID设备"));
        Q_EMIT finished(false);
        return;
    }

    /* 4. 擦除FLASH */
    if (!erase_flash(start_addr)) {
        close_hid();
        Q_EMIT finished(false);
        return;
    }

    /* 5. 编程FLASH */
    if (!program_flash((const uint8_t*)firmware_data.constData(), firmware_data.size())) {
        close_hid();
        Q_EMIT finished(false);
        return;
    }

    /* 6. 校验FLASH */
    if (!verify_flash(start_addr, (const uint8_t*)firmware_data.constData(), firmware_data.size())) {
        close_hid();
        Q_EMIT finished(false);
        return;
    }

    /* 7. 结束升级 */
    if (!end_upgrade()) {
        close_hid();
        Q_EMIT finished(false);
        return;
    }

    /* 8. 关闭HID */
    close_hid();

    update_progress(100, tr("升级完成"));
    success = true;

    Q_EMIT finished(success);
}

} // namespace dialogs
} // namespace pv
