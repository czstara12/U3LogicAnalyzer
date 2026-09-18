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

#include <map>

#include <QApplication>
#include <QCheckBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QString>
#include <QRadioButton>
#include <QGroupBox>
#include <QStringList>
#include <QDebug>
#include <QTimer>
#include <QMessageBox>
#include <QDoubleSpinBox>
#include <QMap>
#include <cmath>

#include "channels.hpp"
#include <pv/session.hpp>
#include <pv/util.hpp>
#include <pv/binding/device.hpp>
#include <pv/data/logic.hpp>
#include <pv/data/logicsegment.hpp>
#include <pv/data/signalbase.hpp>
#include <pv/devices/device.hpp>

using std::make_shared;
using std::map;
using std::shared_ptr;
using std::unordered_set;
using std::vector;

using pv::data::SignalBase;
using pv::data::Logic;
using pv::data::LogicSegment;

using sigrok::Channel;
using sigrok::ChannelGroup;
using sigrok::Device;

using namespace pv::util;

namespace pv {
namespace popups {

Channels::Channels(Session &session, QString device_name, QWidget *parent) :
	Popup(parent),
	session_(session),
	device_name_(device_name),
	updating_channels_(false),
	device_mode_(DeviceMode::OTHER),
	is_usb2_(false),
	current_mode_(0),
	threshold_selector_1_(nullptr),
	threshold_selector_2_(nullptr),
	voltage_level_spinbox_(nullptr),
	mode_selector_(nullptr),
	adc_channel_selector_(nullptr),
	adc_precision_selector_(nullptr),
	adc_channel_label_(nullptr),
	adc_precision_label_(nullptr),
	threshold_label_(nullptr),
	logic_props_box_(nullptr),
	analog_props_box_(nullptr),
	check_box_mapper_(this),
	channel_max_(16),
	skip_hardware_command_(false)
{
	// ch569初始化阈值映射表
	threshold_value_map_ = {
		{0, 4.2f},
		{1, 3.5f},
		{2, 3.1f},
		{3, 2.0f},
		{4, 1.0f}
	};

	for (int i = 0; i < 4; ++i) {
		radioChannels[i] = nullptr;
	}

	setLayout(&layout_);

	device_mode_ = determineDeviceMode();

	init_ui();

	setMinimumSize(500, 400);
}

QSize Channels::sizeHint() const
{
	return QSize(500, 400);
}


Channels::DeviceMode Channels::determineDeviceMode() const
{
	WchDeviceType wch_type = get_wch_device_type(device_name_);

	if (wch_type == WchDeviceType::None) {
		return DeviceMode::OTHER;
	}

	bool is_usb2 = is_usb2_;

	if (wch_type == WchDeviceType::CH569) {
		return is_usb2 ? DeviceMode::CH569_USB2 : DeviceMode::CH569_USB3;
	}

	// CH32H417默认为逻辑分析仪模式
	if (wch_type == WchDeviceType::CH32H417) {
		return is_usb2 ? DeviceMode::CH32H417_USB2_LOGIC : DeviceMode::CH32H417_USB3_LOGIC;
	}

	return DeviceMode::OTHER;
}

// ============================================================================
// UI初始化
// ============================================================================

void Channels::init_ui()
{
	const shared_ptr<sigrok::Device> device = session_.device()->device();
	assert(device);

	map<shared_ptr<Channel>, shared_ptr<SignalBase>> signal_map;
	unordered_set<shared_ptr<SignalBase>> sigs;
	for (const shared_ptr<data::SignalBase>& b : session_.signalbases()) {
		sigs.insert(b);
	}
	for (const shared_ptr<SignalBase>& sig : sigs) {
		signal_map[sig->channel()] = sig;
	}

	if (device_mode_ != DeviceMode::OTHER) {
		QGroupBox* options_box = new QGroupBox("配置", this);
		options_box->setAlignment(Qt::AlignHCenter);
		QFormLayout* options_layout = new QFormLayout();
		QVBoxLayout* vbox_layout = new QVBoxLayout();

		init_threshold_selectors(options_layout);
		init_channel_radios(vbox_layout);

		options_box->setLayout(options_layout);
		options_layout->addRow(vbox_layout);
		layout_.addRow(options_box);
	}

	for (auto& entry : device->channel_groups()) {
		const shared_ptr<ChannelGroup> group = entry.second;
		vector<shared_ptr<SignalBase>> group_sigs;
		for (auto& channel : group->channels()) {
			const auto iter = signal_map.find(channel);
			if (iter == signal_map.end())
				break;
			group_sigs.push_back((*iter).second);
			signal_map.erase(iter);
		}
		populate_group(group, group_sigs);
	}

	vector<shared_ptr<SignalBase>> global_analog_sigs, global_logic_sigs;
	for (auto& channel : device->channels()) {
		const auto iter = signal_map.find(channel);
		if (iter != signal_map.end()) {
			const shared_ptr<SignalBase> signal = (*iter).second;
			if (signal->type() == SignalBase::AnalogChannel)
				global_analog_sigs.push_back(signal);
			else
				global_logic_sigs.push_back(signal);
		}
	}

	populate_group(nullptr, global_logic_sigs);
	populate_group(nullptr, global_analog_sigs);

	connect(&check_box_mapper_, SIGNAL(mapped(QWidget*)),
		this, SLOT(on_channel_checked(QWidget*)));

	init_channel_state();

	QWidget* bottom_spacer = new QWidget(this);
	bottom_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	bottom_spacer->setFixedHeight(1);
	layout_.addRow(bottom_spacer);
}

void Channels::init_wch_ui()
{
	// WCH设备的UI已在populate_group中创建
}

void Channels::init_other_ui()
{
	// 非WCH设备无额外UI
}

void Channels::init_channel_state()
{
	updating_channels_ = true;

	for (auto* cb : logic_checkboxes_) {
		cb->setChecked(false);
		auto it = checkbox_signal_map_.find(cb);
		if (it != checkbox_signal_map_.end()) {
			it->second->set_enabled(false);
		}
	}
	for (auto* cb : analog_checkboxes_) {
		cb->setChecked(false);
		auto it = checkbox_signal_map_.find(cb);
		if (it != checkbox_signal_map_.end()) {
			it->second->set_enabled(false);
		}
	}

	if (device_mode_ == DeviceMode::OTHER) {
		for (auto* cb : logic_checkboxes_) {
			cb->setChecked(true);
			auto it = checkbox_signal_map_.find(cb);
			if (it != checkbox_signal_map_.end()) {
				it->second->set_enabled(true);
			}
		}
	} else {
		for (int i = 0; i < 4; ++i) {
			if (radioChannels[i] && radioChannels[i]->isChecked()) {
				channel_max_ = 16;
				updating_channels_ = false;
				channelSelect(i);
				updating_channels_ = true;
				break;
			}
		}
	}

	updating_channels_ = false;
}

void Channels::init_channel_radios(QVBoxLayout* layout)
{
	QStringList channel_modes;
	bool is_ch32h417 = (device_mode_ == DeviceMode::CH32H417_USB3_LOGIC ||
	                 device_mode_ == DeviceMode::CH32H417_USB2_LOGIC ||
	                 device_mode_ == DeviceMode::CH32H417_ADC);

	if (is_ch32h417) {
		// CH32H417: 根据USB版本显示不同的采样率
		if (device_mode_ == DeviceMode::CH32H417_USB2_LOGIC) {
			// C32H417 USB2.0: 2/4/8通道最高40MHz，16通道最高20MHz
			channel_modes << tr("Use 2 channels (Max 40MHz)");
			channel_modes << tr("Use 4 channels (Max 40MHz)");
			channel_modes << tr("Use 8 channels (Max 40MHz)");
			channel_modes << tr("Use 16 channels (Max 20MHz)");
		} else {
			// CH32H417 USB3.0: 最高200MHz
			channel_modes << tr("Use 2 channels (Max 200MHz)");
			channel_modes << tr("Use 4 channels (Max 200MHz)");
			channel_modes << tr("Use 8 channels (Max 200MHz)");
			channel_modes << tr("Use 16 channels (Max 200MHz)");
		}
	} else if (device_mode_ == DeviceMode::CH569_USB3) {
		// CH569 USB3.0
		channel_modes << tr("Use 2 channels (Max 1GHz)");
		channel_modes << tr("Use 4 channels (Max 500MHz)");
		channel_modes << tr("Use 8 channels (Max 250MHz)");
		channel_modes << tr("Use 16 channels (Max 125MHz)");
	} else if (device_mode_ == DeviceMode::CH569_USB2) {
		 //TODO
		channel_modes << tr("Use 2 channels (Max 500MHz)");
		channel_modes << tr("Use 4 channels (Max 250MHz)");
		channel_modes << tr("Use 8 channels (Max 125MHz)");
		channel_modes << tr("Use 16 channels (Max 62.5MHz)");
	}

	for (int i = 0; i < channel_modes.size(); ++i) {
		radioChannels[i] = new QRadioButton(channel_modes[i]);
		layout->addWidget(radioChannels[i]);
		connect(radioChannels[i], &QRadioButton::clicked, this, [this, i]() {
			channelSelect(i);
		});
	}

	if (channel_modes.size() > 0) {
		radioChannels[channel_modes.size() - 1]->setChecked(true);
	}
}

void Channels::init_threshold_selectors(QFormLayout* layout)
{
	bool is_ch569 = (device_mode_ == DeviceMode::CH569_USB3 ||
	                 device_mode_ == DeviceMode::CH569_USB2);
	bool is_ch32h417 = (device_mode_ == DeviceMode::CH32H417_USB3_LOGIC ||
	                 device_mode_ == DeviceMode::CH32H417_USB2_LOGIC);

	if (is_ch569) {
		// CH569有两组阈值
		QStringList thresholds = {"0.5V", "0.8V", "0.9V", "1.3V", "1.6V"};

		threshold_selector_1_ = new QComboBox();
		threshold_selector_1_->setMinimumWidth(100);
		threshold_selector_1_->addItems(thresholds);
		threshold_selector_1_->setCurrentIndex(4);  // CH569默认1.6V
		connect(threshold_selector_1_, SIGNAL(currentIndexChanged(int)),
			this, SLOT(on_set_threshold_value_1(int)));
		layout->addRow(tr("Trigger Threshold(0~7 Channel)"), threshold_selector_1_);

		threshold_selector_2_ = new QComboBox();
		threshold_selector_2_->setMinimumWidth(100);
		threshold_selector_2_->addItems(thresholds);
		threshold_selector_2_->setCurrentIndex(4);
		connect(threshold_selector_2_, SIGNAL(currentIndexChanged(int)),
			this, SLOT(on_set_threshold_value_2(int)));
		layout->addRow(tr("Trigger Threshold(8~15 Channel)"), threshold_selector_2_);

		threshold_label_ = nullptr;
		adc_channel_selector_ = nullptr;
	} else if (is_ch32h417) {
		mode_selector_ = new QComboBox();
		mode_selector_->setMinimumWidth(100);
		mode_selector_->addItem(tr("Logic Analyzer"));
		mode_selector_->addItem(tr("ADC"));
		connect(mode_selector_, SIGNAL(currentIndexChanged(int)),
			this, SLOT(on_mode_changed(int)));
		layout->addRow(tr("Device Mode"), mode_selector_);

		voltage_level_spinbox_ = new QDoubleSpinBox();
		voltage_level_spinbox_->setRange(0.8, 2.3);
		voltage_level_spinbox_->setSingleStep(0.1);
		voltage_level_spinbox_->setDecimals(1);
		voltage_level_spinbox_->setValue(2.3);
		voltage_level_spinbox_->setSuffix("V");
		voltage_level_spinbox_->setStyleSheet(
			"QDoubleSpinBox {"
				"padding-right: 24px;"
			"}"
			"QDoubleSpinBox::up-button {"
				"subcontrol-origin: margin;"
				"subcontrol-position: right top;"
				"width: 18px; height: 14px;"
				"margin-top: 1px;"
			"}"
			"QDoubleSpinBox::down-button {"
				"subcontrol-origin: margin;"
				"subcontrol-position: right bottom;"
				"width: 18px; height: 14px;"
				"margin-bottom: 1px;"
			"}"
		);
		connect(voltage_level_spinbox_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
			this, &Channels::on_voltage_level_changed);
		threshold_label_ = new QLabel(tr("Voltage Level"));
		layout->addRow(threshold_label_, voltage_level_spinbox_);

		adc_channel_selector_ = new QComboBox();
		adc_channel_selector_->setMinimumWidth(100);
		adc_channel_selector_->addItem(tr("ADC Channel 0"));
		adc_channel_selector_->addItem(tr("ADC Channel 1"));
		adc_channel_selector_->setVisible(false);
		connect(adc_channel_selector_, SIGNAL(currentIndexChanged(int)),
			this, SLOT(on_adc_channel_changed(int)));
		adc_channel_label_ = new QLabel(tr("ADC Channel"));
		adc_channel_label_->setVisible(false);
		layout->addRow(adc_channel_label_, adc_channel_selector_);

		adc_precision_selector_ = new QComboBox();
		adc_precision_selector_->setMinimumWidth(100);
		adc_precision_selector_->addItem(tr("8-bit"));
		adc_precision_selector_->addItem(tr("10-bit"));
		adc_precision_selector_->setVisible(false);
		connect(adc_precision_selector_, SIGNAL(currentIndexChanged(int)),
			this, SLOT(on_adc_precision_changed(int)));
		adc_precision_label_ = new QLabel(tr("ADC Precision"));
		adc_precision_label_->setVisible(false);
		layout->addRow(adc_precision_label_, adc_precision_selector_);

		threshold_selector_1_ = nullptr;
		threshold_selector_2_ = nullptr;
	}
}

// ============================================================================
// populate_group
// ============================================================================

void Channels::populate_group(shared_ptr<ChannelGroup> group,
	const vector<shared_ptr<SignalBase>> sigs)
{
	using pv::binding::Device;

	shared_ptr<Device> binding;
	if (group) {
		binding = make_shared<Device>(group);
	}

	bool is_logic_group = true;
	if (!sigs.empty()) {
		is_logic_group = (sigs[0]->type() == SignalBase::LogicChannel);
	}

	if ((device_mode_ == DeviceMode::CH32H417_USB3_LOGIC ||
	     device_mode_ == DeviceMode::CH32H417_USB2_LOGIC) && !is_logic_group) {
		QGridLayout* grid = nullptr;
		if (analog_props_box_) {
			QFormLayout* form = qobject_cast<QFormLayout*>(analog_props_box_->layout());
			if (form) {
				QLayoutItem* item = form->itemAt(0);
				if (item) {
					grid = qobject_cast<QGridLayout*>(item->layout());
				}
			}
		}

		int row = 0, col = 0;
		if (grid) {
			int count = grid->count();
			row = count / 8;
			col = count % 8;
		}

		for (const shared_ptr<SignalBase>& sig : sigs) {
			assert(sig);

			QCheckBox* checkbox = new QCheckBox(sig->display_name());
			analog_checkboxes_.push_back(checkbox);

			check_box_mapper_.setMapping(checkbox, checkbox);
			connect(checkbox, SIGNAL(toggled(bool)), this, SLOT(checkbox_toggled(bool)));

			checkbox_signal_map_[checkbox] = sig;
		}

		if (!analog_props_box_) {
			QFormLayout* form_layout = new QFormLayout();
			grid = new QGridLayout();
			form_layout->addRow(grid);

			analog_props_box_ = new QGroupBox("通道", this);
			analog_props_box_->setAlignment(Qt::AlignHCenter);
			analog_props_box_->setLayout(form_layout);
			analog_props_box_->setVisible(false);
			layout_.addRow(analog_props_box_);
		}

		for (const shared_ptr<SignalBase>& sig : sigs) {
			for (auto* cb : analog_checkboxes_) {
				auto it = checkbox_signal_map_.find(cb);
				if (it != checkbox_signal_map_.end() && it->second == sig) {
					grid->addWidget(cb, row, col);
					if (++col >= 8) {
						col = 0;
						row++;
					}
					break;
				}
			}
		}

		return;
	}

	QHBoxLayout* group_layout = new QHBoxLayout();
	layout_.addRow(group_layout);

	if ((!sigs.empty() || (binding && !binding->properties().empty())) && group) {
		QLabel *label = new QLabel(
			QString("<h3>%1</h3>").arg(group->name().c_str()));
		group_label_map_[group] = label;
	}

	QFormLayout* form_layout = new QFormLayout();
	QGridLayout* grid = new QGridLayout();
	int row = 0, col = 0;

	for (const shared_ptr<SignalBase>& sig : sigs) {
		assert(sig);

		QCheckBox* checkbox = new QCheckBox(sig->display_name());

		if (sig->type() == SignalBase::LogicChannel) {
			logic_checkboxes_.push_back(checkbox);
		} else if (sig->type() == SignalBase::AnalogChannel) {
			analog_checkboxes_.push_back(checkbox);
		}

		check_box_mapper_.setMapping(checkbox, checkbox);
		connect(checkbox, SIGNAL(toggled(bool)), this, SLOT(checkbox_toggled(bool)));

		grid->addWidget(checkbox, row, col);
		checkbox_signal_map_[checkbox] = sig;

		if (++col >= 8 || &sig == &sigs.back()) {
			col = 0;
			row++;
		}
	}

	form_layout->addRow(grid);

	if (sigs.empty()) {
		return;
	}

	QGroupBox* props_box = new QGroupBox("通道", this);
	props_box->setAlignment(Qt::AlignHCenter);
	props_box->setLayout(form_layout);

	if ((device_mode_ == DeviceMode::CH32H417_USB3_LOGIC ||
	     device_mode_ == DeviceMode::CH32H417_USB2_LOGIC) && is_logic_group) {
		logic_props_box_ = props_box;
	}

	layout_.addRow(props_box);

	if (binding) {
		binding->add_properties_to_form(&layout_, true);
		group_bindings_.push_back(binding);
	}
	
}

// ============================================================================
// 通道选择
// ============================================================================

void Channels::channelSelect(uint16_t index)
{
	updating_channels_ = true;

	clear_all_logic_triggers();

	struct ChannelConfig {
		int channel_count;
		uint64_t max_rate_usb3;
	};

	static const ChannelConfig configs[] = {
		{2, SR_GHZ(1)},      // 2通道
		{4, SR_MHZ(500)},    // 4通道
		{8, SR_MHZ(250)},    // 8通道
		{16, SR_MHZ(125)}    // 16通道
	};

	const ChannelConfig& config = configs[index];
	channel_max_ = config.channel_count;

	uint64_t max_sample_rate = 0;
	bool emit_signal = true;

	switch (device_mode_) {
	case DeviceMode::CH569_USB3:
		max_sample_rate = config.max_rate_usb3;
		break;

	case DeviceMode::CH569_USB2:
		max_sample_rate = config.max_rate_usb3 / 2;
		break;

	case DeviceMode::CH32H417_USB3_LOGIC:
		max_sample_rate = SR_MHZ(200);
		if (index >= 2) {
			channel_count_change(config.channel_count);
		} else {
			channel_count_change(8);
		}
		emit_signal = false;  // CH32H417采样率固定，不需要更新采样率
		break;

	case DeviceMode::CH32H417_USB2_LOGIC:
		// CH32H417 USB2.0: 8通道最高40MHz，16通道最高20MHz
		switch (index) {
		case 0: // 2通道
			max_sample_rate = SR_MHZ(40);
			channel_count_change(8);  // 硬件实际使用8通道
			break;
		case 1: // 4通道
			max_sample_rate = SR_MHZ(40);
			channel_count_change(8);  // 硬件实际使用8通道
			break;
		case 2: // 8通道
			max_sample_rate = SR_MHZ(40);
			channel_count_change(8);
			break;
		case 3: // 16通道
			max_sample_rate = SR_MHZ(20);
			channel_count_change(16);
			break;
		}
		emit_signal = true;  // USB2.0模式下采样率随通道数变化，需要更新采样率列表
		break;

	case DeviceMode::CH32H417_ADC:
		emit_signal = false;
		break;

	default:
		max_sample_rate = config.max_rate_usb3;
		break;
	}

	vector<QCheckBox*>* target_checkboxes = &logic_checkboxes_;
	if (device_mode_ == DeviceMode::CH32H417_ADC) {
		target_checkboxes = &analog_checkboxes_;
	}

	for (int i = 0; i < target_checkboxes->size(); ++i) {
		bool should_enable = (i < channel_max_);
		(*target_checkboxes)[i]->setChecked(should_enable);

		auto it = checkbox_signal_map_.find((*target_checkboxes)[i]);
		if (it != checkbox_signal_map_.end()) {
			it->second->set_enabled(should_enable);
		}
	}

	if (emit_signal) {
		max_sample_rate_change(max_sample_rate);
	}

	updating_channels_ = false;

	session_.update_signals();

	channel_selection_changed();
	}

// ============================================================================
// set_all_channels
// ============================================================================

void Channels::set_all_channels(bool set)
{
	updating_channels_ = true;

	if (device_mode_ != DeviceMode::OTHER) {
		vector<QCheckBox*>* target_checkboxes = &logic_checkboxes_;
		if (device_mode_ == DeviceMode::CH32H417_ADC) {
			target_checkboxes = &analog_checkboxes_;
		}

		for (int i = 0; i < target_checkboxes->size(); ++i) {
			bool should_enable = (i < channel_max_) && set;
			(*target_checkboxes)[i]->setChecked(should_enable);

			auto it = checkbox_signal_map_.find((*target_checkboxes)[i]);
			if (it != checkbox_signal_map_.end()) {
				it->second->set_enabled(should_enable);
			}
		}
	} else {
		for (auto& entry : checkbox_signal_map_) {
			QCheckBox* cb = entry.first;
			const shared_ptr<SignalBase> sig = entry.second;
			assert(sig);
			sig->set_enabled(set);
			cb->setChecked(set);
		}
	}

	updating_channels_ = false;
}

void Channels::clear_all_logic_triggers()
{
	vector<shared_ptr<data::SignalBase>> v = session_.signalbases();
	for (int i = 0; i < v.size(); ++i) {
		QString signal_name = "D" + QString::number(i);
		for (auto& s : v) {
			if (s->internal_name() == signal_name) {
				s->set_none_trigger();
			}
		}
	}
}

void Channels::on_set_threshold_value_1(int index)
{
	auto it = threshold_value_map_.find(index);
	if (it != threshold_value_map_.end()) {
		threshold_value_change_1(it->second);
	}
}

void Channels::on_set_threshold_value_2(int index)
{
	auto it = threshold_value_map_.find(index);
	if (it != threshold_value_map_.end()) {
		threshold_value_change_2(it->second);
	}
}

void Channels::set_default_bank_threshold(float threshold_1, float threshold_2)
{
	if (device_mode_ == DeviceMode::CH32H417_USB3_LOGIC ||
	    device_mode_ == DeviceMode::CH32H417_USB2_LOGIC) {
		if (voltage_level_spinbox_) {
			voltage_level_spinbox_->setValue(threshold_1);
			on_voltage_level_changed(threshold_1);
		}
		return;
	}

	int index = 0;
	for (auto& it : threshold_value_map_) {
		if (it.second == threshold_1) {
			if (threshold_selector_1_) {
				threshold_selector_1_->setCurrentIndex(index);
				on_set_threshold_value_1(index);
			}
		}

		// CH569有两路阈值
		if (it.second == threshold_2) {
			if (threshold_selector_2_) {
				threshold_selector_2_->setCurrentIndex(index);
				on_set_threshold_value_2(index);
			}
		}
		index++;
	}
}

// ============================================================================
// 模式切换 (CH32H417)
// ============================================================================

void Channels::on_mode_changed(int index)
{
	if (index == current_mode_) {
		return;
	}

	current_mode_ = index;
	updating_channels_ = true;

	if (index == 0) {
		device_mode_ = is_usb2_ ? DeviceMode::CH32H417_USB2_LOGIC : DeviceMode::CH32H417_USB3_LOGIC;

		for (auto* cb : analog_checkboxes_) {
			cb->setChecked(false);
			auto it = checkbox_signal_map_.find(cb);
			if (it != checkbox_signal_map_.end()) {
				it->second->set_enabled(false);
			}
		}

		if (logic_props_box_) logic_props_box_->setVisible(true);
		if (analog_props_box_) analog_props_box_->setVisible(false);

		for (int i = 0; i < 4; ++i) {
			if (radioChannels[i]) radioChannels[i]->setVisible(true);
		}
		if (voltage_level_spinbox_) voltage_level_spinbox_->setVisible(true);
		if (threshold_label_) threshold_label_->setVisible(true);
		if (adc_channel_selector_) adc_channel_selector_->setVisible(false);
		if (adc_channel_label_) adc_channel_label_->setVisible(false);
		if (adc_precision_selector_) adc_precision_selector_->setVisible(false);
		if (adc_precision_label_) adc_precision_label_->setVisible(false);

		for (int i = 0; i < 4; ++i) {
			if (radioChannels[i] && radioChannels[i]->isChecked()) {
				channelSelect(i);
				break;
			}
		}
	} else {
		device_mode_ = DeviceMode::CH32H417_ADC;

		for (auto* cb : logic_checkboxes_) {
			cb->setChecked(false);
			auto it = checkbox_signal_map_.find(cb);
			if (it != checkbox_signal_map_.end()) {
				it->second->set_enabled(false);
			}
		}

		if (logic_props_box_) logic_props_box_->setVisible(false);
		if (analog_props_box_) analog_props_box_->setVisible(true);

		for (int i = 0; i < 4; ++i) {
			if (radioChannels[i]) radioChannels[i]->setVisible(false);
		}
		if (voltage_level_spinbox_) voltage_level_spinbox_->setVisible(false);
		if (threshold_label_) threshold_label_->setVisible(false);
		if (adc_channel_selector_) adc_channel_selector_->setVisible(true);
		if (adc_channel_label_) adc_channel_label_->setVisible(true);
		if (adc_precision_selector_) adc_precision_selector_->setVisible(true);
		if (adc_precision_label_) adc_precision_label_->setVisible(true);

		if (analog_checkboxes_.size() >= 2) {
			for (auto* cb : analog_checkboxes_) {
				cb->setVisible(true);
				cb->setEnabled(true);
			}

			int adc_idx = adc_channel_selector_ ? adc_channel_selector_->currentIndex() : 0;
			analog_checkboxes_[0]->setChecked(adc_idx == 0);
			analog_checkboxes_[1]->setChecked(adc_idx == 1);

			auto it0 = checkbox_signal_map_.find(analog_checkboxes_[0]);
			if (it0 != checkbox_signal_map_.end()) {
				it0->second->set_enabled(adc_idx == 0);
			}
			auto it1 = checkbox_signal_map_.find(analog_checkboxes_[1]);
			if (it1 != checkbox_signal_map_.end()) {
				it1->second->set_enabled(adc_idx == 1);
			}
		}
	}

	updating_channels_ = false;

	// 模式切换后通道启用/禁用状态变化，需要更新view中的signalbase列表
	// (restore_setup可能已将disabled通道从view中remove_signalbase，
	//  切换模式后需通过update_signals将重新enabled的通道添加回view)
	session_.update_signals();

	mode_changed(index);
}

void Channels::on_adc_channel_changed(int index)
{
	if (analog_checkboxes_.size() >= 2) {
		updating_channels_ = true;

		analog_checkboxes_[0]->setVisible(true);
		analog_checkboxes_[1]->setVisible(true);

		analog_checkboxes_[0]->setChecked(index == 0);
		analog_checkboxes_[1]->setChecked(index == 1);

		auto it0 = checkbox_signal_map_.find(analog_checkboxes_[0]);
		if (it0 != checkbox_signal_map_.end()) {
			it0->second->set_enabled(index == 0);
		}

		auto it1 = checkbox_signal_map_.find(analog_checkboxes_[1]);
		if (it1 != checkbox_signal_map_.end()) {
			it1->second->set_enabled(index == 1);
		}

		updating_channels_ = false;
	}

	// ADC通道切换后需要更新view中的signalbase列表
	session_.update_signals();
	adc_channel_changed(index);
	qDebug() << "ADC channel changed to:" << index + 1;
}

void Channels::on_adc_precision_changed(int index)
{
	adc_precision_changed(index);
	qDebug() << "ADC precision changed to:" << (index == 0 ? "8-bit" : "10-bit");
}

void Channels::checkbox_toggled(bool state)
{
	if (updating_channels_) {
		return;
	}

	QCheckBox* checkBox = qobject_cast<QCheckBox*>(QObject::sender());

	if (device_mode_ == DeviceMode::CH32H417_ADC) {
		int clicked_index = -1;
		for (int i = 0; i < analog_checkboxes_.size(); ++i) {
			if (checkBox == analog_checkboxes_[i]) {
				clicked_index = i;
				break;
			}
		}

		if (clicked_index >= 0 && clicked_index < 2) {
			updating_channels_ = true;

			if (state) {
				if (adc_channel_selector_) {
					adc_channel_selector_->blockSignals(true);
					adc_channel_selector_->setCurrentIndex(clicked_index);
					adc_channel_selector_->blockSignals(false);
				}

				int other_index = (clicked_index == 0) ? 1 : 0;
				analog_checkboxes_[clicked_index]->setChecked(true);
				analog_checkboxes_[other_index]->setChecked(false);

				auto it_clicked = checkbox_signal_map_.find(analog_checkboxes_[clicked_index]);
				if (it_clicked != checkbox_signal_map_.end()) {
					it_clicked->second->set_enabled(true);
				}
				auto it_other = checkbox_signal_map_.find(analog_checkboxes_[other_index]);
				if (it_other != checkbox_signal_map_.end()) {
					it_other->second->set_enabled(false);
				}

				adc_channel_changed(clicked_index);
				qDebug() << "ADC channel changed by checkbox click to:" << clicked_index + 1;
			} else {
				checkBox->setChecked(true);
			}

			updating_channels_ = false;
			session_.update_signals();
			return;
		}
	}

	vector<QCheckBox*>* target_checkboxes = &logic_checkboxes_;
	if (device_mode_ == DeviceMode::CH32H417_ADC) {
		target_checkboxes = &analog_checkboxes_;
	}

	bool is_valid = false;
	for (int i = 0; i < channel_max_; ++i) {
		if (i < target_checkboxes->size() && checkBox == (*target_checkboxes)[i]) {
			is_valid = true;
			break;
		}
	}

	if (is_valid) {
		check_box_mapper_.map();
		auto it = checkbox_signal_map_.find(checkBox);
		if (it != checkbox_signal_map_.end()) {
			auto sig = it->second;
			assert(sig);
			sig->set_enabled(state);
		}
		session_.update_signals();
		session_.signals_changed();
	} else {
		if (state && device_mode_ != DeviceMode::OTHER) {
			QMessageBox msg(this);
			msg.setIcon(QMessageBox::Information);
			msg.setWindowTitle(tr("Information"));
			msg.setText(tr("The current mode does not support opening this channel."));
			msg.addButton(tr("确定"), QMessageBox::AcceptRole);
			msg.exec();
			checkBox->setChecked(false);
		}
	}
}

void Channels::on_channel_checked(QWidget* widget)
{
	if (updating_channels_) {
		return;
	}

	QCheckBox* check_box = qobject_cast<QCheckBox*>(widget);
	assert(check_box);

	auto iter = checkbox_signal_map_.find(check_box);
	assert(iter != checkbox_signal_map_.end());

	const shared_ptr<SignalBase> s = (*iter).second;
	assert(s);

	s->set_enabled(check_box->isChecked());
}

void Channels::showEvent(QShowEvent* event)
{
	pv::widgets::Popup::showEvent(event);

	const shared_ptr<sigrok::Device> device = session_.device()->device();
	assert(device);

	for (auto& entry : device->channel_groups()) {
		const shared_ptr<ChannelGroup> group = entry.second;
		if (group_label_map_.count(group) > 0) {
			QLabel* label = group_label_map_[group];
			label->setText(QString("<h3>%1</h3>").arg(group->name().c_str()));
		}
	}

	updating_channels_ = true;

	for (auto& entry : checkbox_signal_map_) {
		QCheckBox* cb = entry.first;
		const shared_ptr<SignalBase> sig = entry.second;
		assert(sig);
		cb->setChecked(sig->enabled());
		cb->setText(sig->display_name());
	}

	updating_channels_ = false;
}

void Channels::on_voltage_level_changed(double value)
{
	// 电压到A1命令值的映射表
	static const QMap<double, uint16_t> voltage_to_raw = {
		{2.3, 33},
		{2.2, 93},
		{2.1, 123},
		{2.0, 200},
		{1.9, 225},
		{1.8, 280},
		{1.7, 308},
		{1.6, 400},
		{1.5, 428},
		{1.4, 500},
		{1.3, 545},
		{1.2, 574},
		{1.1, 620},
		{1.0, 675},
		{0.9, 725},
		{0.8, 751},
	};

	uint16_t raw_value = 33;
	if (voltage_to_raw.contains(value)) {
		raw_value = voltage_to_raw[value];
	} else {
		double closest_voltage = 2.3;
		double min_diff = std::abs(value - closest_voltage);
		for (auto it = voltage_to_raw.begin(); it != voltage_to_raw.end(); ++it) {
			double diff = std::abs(value - it.key());
			if (diff < min_diff) {
				min_diff = diff;
				closest_voltage = it.key();
				raw_value = it.value();
			}
		}
	}

	qDebug() << "Voltage level changed to:" << value << "V, raw value:" << raw_value;
	threshold_value_raw_changed(raw_value);
	threshold_value_change_ch32h417(static_cast<float>(value));
}

// ============================================================================
// set_default_channel
// ============================================================================

void Channels::set_default_channel()
{
	// 默认通道配置已在构造函数中处理
}

// ============================================================================
// set_usb2_mode
// ============================================================================

void Channels::set_usb2_mode(bool is_usb2)
{
	is_usb2_ = is_usb2;
	DeviceMode new_mode = determineDeviceMode();

	if (new_mode != device_mode_) {
		device_mode_ = new_mode;

		update_channel_radio_texts();
	}
}

void Channels::update_channel_radio_texts()
{
	QStringList channel_modes;

	bool is_ch32h417 = (device_mode_ == DeviceMode::CH32H417_USB3_LOGIC ||
	                 device_mode_ == DeviceMode::CH32H417_USB2_LOGIC ||
	                 device_mode_ == DeviceMode::CH32H417_ADC);

	if (is_ch32h417) {
		if (device_mode_ == DeviceMode::CH32H417_USB2_LOGIC) {
			channel_modes << tr("Use 2 channels (Max 40MHz)");
			channel_modes << tr("Use 4 channels (Max 40MHz)");
			channel_modes << tr("Use 8 channels (Max 40MHz)");
			channel_modes << tr("Use 16 channels (Max 20MHz)");
		} else {
			channel_modes << tr("Use 2 channels (Max 200MHz)");
			channel_modes << tr("Use 4 channels (Max 200MHz)");
			channel_modes << tr("Use 8 channels (Max 200MHz)");
			channel_modes << tr("Use 16 channels (Max 200MHz)");
		}
	} else if (device_mode_ == DeviceMode::CH569_USB3) {
		channel_modes << tr("Use 2 channels (Max 1GHz)");
		channel_modes << tr("Use 4 channels (Max 500MHz)");
		channel_modes << tr("Use 8 channels (Max 250MHz)");
		channel_modes << tr("Use 16 channels (Max 125MHz)");
	} else if (device_mode_ == DeviceMode::CH569_USB2) {
		// TODO
		channel_modes << tr("Use 2 channels (Max 500MHz)");
		channel_modes << tr("Use 4 channels (Max 250MHz)");
		channel_modes << tr("Use 8 channels (Max 125MHz)");
		channel_modes << tr("Use 16 channels (Max 62.5MHz)");
	}

	// 更新RadioButton的文字
	for (int i = 0; i < channel_modes.size() && i < 4; ++i) {
		if (radioChannels[i]) {
			radioChannels[i]->setText(channel_modes[i]);
		}
	}
}

}  // namespace popups
}  // namespace pv
