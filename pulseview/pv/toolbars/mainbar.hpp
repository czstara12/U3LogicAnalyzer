/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2026 Q2H2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
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

#ifndef PULSEVIEW_PV_TOOLBARS_MAINBAR_HPP
#define PULSEVIEW_PV_TOOLBARS_MAINBAR_HPP

#include <cstdint>
#include <list>
#include <memory>
#include <QString>
#include <QDialog>
#include <glibmm/variant.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QToolBar>
#include <QToolButton>
#include <pv/popups/channels.hpp>
#include <pv/session.hpp>
#include <pv/views/trace/standardbar.hpp>
#include <pv/widgets/devicetoolbutton.hpp>
#include <pv/widgets/popuptoolbutton.hpp>
#include <pv/widgets/sweeptimingwidget.hpp>
#include <thread>
#include <mutex>
#include <QFile>
#include <libusb.h>
#include <hidapi.h>

#include "../../../libsigrok/src/hardware/wch-ch32h417/ch375_wrapper.h"

Q_DECLARE_METATYPE(unsigned long)

using std::shared_ptr;

namespace sigrok {
class Device;
class InputFormat;
class OutputFormat;
}

Q_DECLARE_METATYPE(shared_ptr<sigrok::Device>)

class QAction;

namespace pv {

class MainWindow;
class Session;

namespace views {
namespace trace {
class View;
}
}

namespace toolbars {

class MainBar : public pv::views::trace::StandardBar
{
	Q_OBJECT

private:
	static const uint64_t MinSampleCount;
	static const uint64_t MaxSampleCount;
	static const uint64_t DefaultSampleCount;
	static const uint64_t MinSampleRate;
	static uint64_t MaxSampleRate;
	/**
	 * Name of the setting used to remember the directory
	 * containing the last file that was opened.
	 */
	static const char *SettingOpenDirectory;

	/**
	 * Name of the setting used to remember the directory
	 * containing the last file that was saved.
	 */
	static const char *SettingSaveDirectory;
	std::thread t_hot_plug;
	QString m_vendorName;
	std::mutex vendorName_mutex;
	
public:
	MainBar(Session &session, QWidget *parent,
		pv::views::trace::View *view);
	~MainBar();
	void update_device_list();
	void update_runstop_status(int state);

	void set_capture_state(pv::Session::capture_state state);

	void reset_device_selector();

	QAction* action_new_view() const;
	QAction* action_open() const;
	QAction* action_save() const;
	QAction* action_save_as() const;
	QAction* action_save_selection_as() const;
	QAction* action_restore_setup() const;
	QAction* action_save_setup() const;
	QAction* action_connect() const;
	
	uint64_t get_sample_count();
	uint64_t get_sample_rate();

	uint16_t get_channel_number() const;
	uint64_t get_trigger_offset();
	uint16_t get_sample_count_index();
	void set_device_selector_name(QString device_selector);
	void hot_plug_start();
	void hot_plug_stop();
	void set_vendorName(QString vendorName);
	QString get_vendorName();
	
private:
	void run_stop();

	void select_init_device();

	void save_selection_to_file();

	void update_sample_rate_selector();
	void update_sample_rate_selector_value();
	void update_sample_count_selector();
	void update_device_config_widgets();
	void commit_sample_rate();
	void commit_sample_count();
	void commit_channel_number(uint64_t channelNum);
	void commit_threshold_value(int index, float thresholdLevel);
	void hot_plug_listen();
	void renew_samplecount_samplerate(uint64_t samplerate,uint64_t samplerate_max);
	void get_decoder_list_default(QString decoder_name);

	static void ch32h417_notify_callback(unsigned long eventStatus);
	static MainBar* s_mainbar_instance;

private Q_SLOTS:
	void show_session_error(const QString text, const QString info_text);

	void export_file(shared_ptr<sigrok::OutputFormat> format,
		bool selection_only = false, QString file_name = "");
	void import_file(shared_ptr<sigrok::InputFormat> format);

	void on_device_selected();
	void on_device_changed();
	void on_capture_state_changed(int state);
	void on_sample_count_changed();
	void on_sample_rate_changed();
	void channels_button_ex_clicked();
	void on_config_changed();

	void on_actionSingle_triggered();
	void on_actionRepeat_triggered();
	
	void on_action_version_info_triggered();
    void on_action_feedback_triggered();
    void on_action_update_triggered();
	void on_action_use_guide_triggered();
    void on_action_about_triggered();

	void on_actionNewView_triggered(QAction* action = nullptr);

	void on_actionOpen_triggered();
	void on_actionSave_triggered();
	void on_actionSaveAs_triggered();
	void on_actionSaveSelectionAs_triggered();

	void on_actionSaveSetup_triggered();
	void on_actionRestoreSetup_triggered();

	void on_actionConnect_triggered();

	
	void on_add_math_signal_clicked();

	void on_update_max_sample_rate(uint64_t maxSampleRate);
	void on_update_channel_count(uint16_t channelCount);  // CH32H417通道数变化处理
	void on_update_threshold_value_1(float thresholdValue);
	void on_update_threshold_value_2(float thresholdValue);
	void on_update_threshold_value_ch32h417(float thresholdValue);  // CH32H417专用阈值处理
	void on_mode_changed(int mode);
	void on_adc_channel_changed(int channel);  // ADC通道切换处理
	void on_adc_precision_changed(int precision);  // ADC精度切换处理
	void on_threshold_value_raw_changed(uint16_t value);  // DAC原始值设置
	void on_ch32h417_device_event(unsigned long eventStatus);
	void on_channel_selection_changed();  // 通道选择变化处理，用于更新通道高度
	void update_device_status_icon();  // 更新设备状态图标
public Q_SLOTS:
	void on_add_decoder_clicked();
	
protected:
	void add_toolbar_widgets();

	bool eventFilter(QObject *watched, QEvent *event);

Q_SIGNALS:
	void new_view(Session *session, int type);
	void show_decoder_selector(Session *session);
	void divice_detached();
	void device_attached();
	void run_stop_button_clicked();
	void ch32h417_device_event_signal(unsigned long eventStatus);

private:
	QAction *const action_new_view_;
	// QAction *const action_run_stop_;
	QAction *action_new_binary_;
	QAction *action_new_tabular_;
	QAction *const action_open_;
	QAction *const action_save_;
	QAction *const action_save_as_;
	QAction *const action_save_selection_as_;
	QAction *const action_restore_setup_;
	QAction *const action_save_setup_;
	QAction *const action_connect_;

	QToolButton *run_stop_button_;
	QToolButton *model_change_button_;
	QToolButton *new_view_button_, *open_button_, *save_button_;
	QToolButton *help_button_;
	QToolButton *device_status_icon_;

	pv::widgets::DeviceToolButton device_selector_;

	// pv::widgets::PopupToolButton configure_button_;
	// QAction *configure_button_action_;

	pv::widgets::PopupToolButton channels_button_;
	QToolButton * channels_button_ex_;
	QAction *channels_button_action_;

	pv::widgets::SweepTimingWidget sample_count_;
	pv::widgets::SweepTimingWidget sample_rate_;
	bool updating_sample_rate_;
	bool updating_sample_count_;

	bool sample_count_supported_;

	atomic<bool> hot_plug_listen_;
	bool iap_upgrading_;

	bool is_count_setting_default_;
	bool is_rate_setting_default_;
	uint64_t sample_count_value_ = 12500;
	uint64_t sample_rate_value_ = SR_KHZ(500);
	uint16_t channel_number_;
	float thresholdValue_1_ = 1.0f;
	float thresholdValue_2_ = 1.0f;
	float thresholdValue_ch32h417_ = 2.3f;
	uint64_t hardware_version_ = 0;

public:
	pv::popups::Channels*  channels_;
	int is_repeat_acq_default_ = 0;
	float threshold_bank_1_default_ = 1.0;
	float threshold_bank_2_default_ = 1.0;
	float threshold_ch32h417_default_ = 2.3f;
	uint8_t channel_number_default_ = 16;
	uint64_t sample_count_default_index_ = 0;
	uint64_t sample_rate_default_index_ = 0;
	QStringList decoder_list_default_;
	int capture_mode_ = Session::Single;
	void get_setting_default();
	void set_setting_default();
	void renew_setting_default();
	void save_setup(QSettings &settings) const;
	void restore_setup(QSettings &settings);
	void renew_default_bank_threshold();
	void show_hardware_version();
	QDialog channel_dlg;
#ifdef ENABLE_DECODE
	QToolButton *add_decoder_button_;
#endif

	QToolButton *add_math_signal_button_;
};

} // namespace toolbars
} // namespace pv

#endif // PULSEVIEW_PV_TOOLBARS_MAINBAR_HPP
