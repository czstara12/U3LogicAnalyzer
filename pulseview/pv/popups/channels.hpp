/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnalyzer is based on PulseView.
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

#ifndef PULSEVIEW_PV_POPUPS_CHANNELS_HPP
#define PULSEVIEW_PV_POPUPS_CHANNELS_HPP

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalMapper>
#include <QRadioButton>
#include <QString>
#include <QVBoxLayout>
#include <pv/widgets/popup.hpp>
#include <QComboBox>
#include <QGroupBox>
#include <QDoubleSpinBox>
#include <cstdint>

using std::function;
using std::map;
using std::shared_ptr;
using std::vector;

namespace sigrok {
class ChannelGroup;
}

namespace pv {

class Session;

namespace binding {
class Device;
}

namespace data {
class SignalBase;
}

namespace popups {

class Channels : public pv::widgets::Popup
{
	Q_OBJECT

public:
	enum class DeviceMode {
		CH569_USB3,
		CH569_USB2,
		CH32H417_USB3_LOGIC,
		CH32H417_USB2_LOGIC,
		CH32H417_ADC,
		OTHER
	};

	Channels(Session &session, QString device_name, QWidget *parent = nullptr);

	void set_default_channel();
	void set_default_bank_threshold(float threshold_1, float threshold_2);
	void set_usb2_mode(bool is_usb2);
	bool is_usb2() const { return is_usb2_; }

	QSize sizeHint() const override;

	QRadioButton* radioChannels[4];
	QString device_name_;
	QDialog* parent_dialog_;

Q_SIGNALS:
	void max_sample_rate_change(uint64_t maxSampleRate);
	void channel_count_change(uint16_t channelCount);
	void threshold_value_change_1(float threshold);
	void threshold_value_change_2(float threshold);
	void threshold_value_change_ch32h417(float threshold);
	void mode_changed(int mode);
	void adc_channel_changed(int channel);
	void adc_precision_changed(int precision);
	void threshold_value_raw_changed(uint16_t value);
	void channel_selection_changed();

public Q_SLOTS:
	void channelSelect(uint16_t index);

private Q_SLOTS:
	void on_channel_checked(QWidget *widget);
	void on_mode_changed(int index);
	void on_adc_channel_changed(int index);
	void on_adc_precision_changed(int index);
	void on_set_threshold_value_1(int index);
	void on_set_threshold_value_2(int index);
	void on_voltage_level_changed(double value);
	void checkbox_toggled(bool state);

private:
	void init_ui();
	void init_wch_ui();
	void init_other_ui();
	void init_channel_radios(QVBoxLayout* layout);
	void init_threshold_selectors(QFormLayout* layout);
	void init_channel_state();

	DeviceMode determineDeviceMode() const;
	void set_all_channels(bool set);
	void populate_group(shared_ptr<sigrok::ChannelGroup> group,
		const vector<shared_ptr<data::SignalBase>> sigs);
	void showEvent(QShowEvent *event);
	void clear_all_logic_triggers();
	void update_channel_radio_texts();

	Session &session_;
	QFormLayout layout_;
	bool updating_channels_;

	DeviceMode device_mode_;
	bool is_usb2_;
	int current_mode_;

	QComboBox* threshold_selector_1_;
	QComboBox* threshold_selector_2_;
	QDoubleSpinBox* voltage_level_spinbox_;
	QComboBox* mode_selector_;
	QComboBox* adc_channel_selector_;
	QComboBox* adc_precision_selector_;
	QLabel* adc_channel_label_;
	QLabel* adc_precision_label_;
	QLabel* threshold_label_;
	QGroupBox* logic_props_box_;
	QGroupBox* analog_props_box_;

	vector<QCheckBox*> logic_checkboxes_;
	vector<QCheckBox*> analog_checkboxes_;

	map<QCheckBox*, shared_ptr<data::SignalBase>> checkbox_signal_map_;
	map<shared_ptr<sigrok::ChannelGroup>, QLabel*> group_label_map_;
	vector<shared_ptr<binding::Device>> group_bindings_;
	QSignalMapper check_box_mapper_;

	map<int, float> threshold_value_map_;

	uint16_t channel_max_;
	bool skip_hardware_command_;
};

}  // namespace popups
}  // namespace pv

#endif // PULSEVIEW_PV_POPUPS_CHANNELS_HPP
