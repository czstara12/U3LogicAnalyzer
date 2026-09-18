/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2012-14 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include <cassert>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <sys/stat.h>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QApplication>

#include "devicemanager.hpp"
#include "mainwindow.hpp"

#include <pv/dialogs/decoder_channel_dialog.hpp>
#include "session.hpp"
#include "util.hpp"

#include "data/analog.hpp"
#include "data/analogsegment.hpp"
#include "data/decode/decoder.hpp"
#include "data/logic.hpp"
#include "data/logicsegment.hpp"
#include "data/mathsignal.hpp"
#include "data/signalbase.hpp"

#include "devices/hardwaredevice.hpp"
#include "devices/inputfile.hpp"
#include "devices/sessionfile.hpp"

#include "toolbars/mainbar.hpp"

#include "views/trace/analogsignal.hpp"
#include "views/trace/decodetrace.hpp"
#include "views/trace/logicsignal.hpp"
#include "views/trace/signal.hpp"
#include "views/trace/view.hpp"
#include <libusb-1.0/libusb.h>
#include <libsigrokcxx/libsigrokcxx.hpp>
#include <fstream>
using namespace std;
#ifdef ENABLE_FLOW
#include <gstreamermm.h>
#include <libsigrokflow/libsigrokflow.hpp>
#endif

#ifdef ENABLE_DECODE
#include <libsigrokdecode/libsigrokdecode.h>
#include "data/decodesignal.hpp"
#endif

using std::bad_alloc;
using std::dynamic_pointer_cast;
using std::find_if;
using std::function;
using std::list;
using std::lock_guard;
using std::make_pair;
using std::make_shared;
using std::map;
using std::max;
using std::move;
using std::mutex;
using std::pair;
using std::recursive_mutex;
using std::runtime_error;
using std::shared_ptr;
using std::string;
#ifdef ENABLE_FLOW
using std::unique_lock;
#endif
using std::unique_ptr;
using std::vector;

using sigrok::Analog;
using sigrok::Channel;
using sigrok::ConfigKey;
using sigrok::DatafeedCallbackFunction;
using sigrok::Error;
using sigrok::InputFormat;
using sigrok::Logic;
using sigrok::Meta;
using sigrok::Packet;
using sigrok::Session;

using Glib::VariantBase;

#ifdef ENABLE_FLOW
using Gst::Bus;
using Gst::ElementFactory;
using Gst::Pipeline;
#endif

using pv::data::SignalGroup;
using pv::util::Timestamp;
using pv::views::trace::Signal;
using pv::views::trace::AnalogSignal;
using pv::views::trace::LogicSignal;
using namespace pv::util;
volatile int done_;
namespace pv {

shared_ptr<sigrok::Context> Session::sr_context;

Session::Session(DeviceManager &device_manager, QString name) :
	shutting_down_(false),
	device_manager_(&device_manager),
	default_name_(name),
	name_(name),
	capture_state_(Stopped),
	cur_samplerate_(0),
	data_saved_(true)
{
	// Use this name also for the QObject instance
	setObjectName(name_);
	done_ = 0;
	out_of_memory_ = false;
	connect(this, SIGNAL(notify_acq_done()), this, SLOT(on_notify_acq_done()));
}

Session::~Session()
{
	shutting_down_ = true;

	// Stop and join to the thread
	stop_capture();

	for (SignalGroup* group : signal_groups_) {
		group->clear();
		delete group;
	}
	out_of_memory_ = false;
}

void Session::update_device_manager(DeviceManager *device_manager)
{
	device_manager_ = device_manager;
}

DeviceManager& Session::device_manager()
{
	return *device_manager_;
}

DeviceManager* Session::device_manager_ptr()
{
	return device_manager_;
}

const DeviceManager& Session::device_manager() const
{
	return *device_manager_;
}

shared_ptr<sigrok::Session> Session::session() const
{
	if (!device_)
		return shared_ptr<sigrok::Session>();
	return device_->session();
}

shared_ptr<devices::Device> Session::device() const
{
	return device_;
}

QString Session::name() const
{
	return name_;
}

void Session::set_name(QString name)
{
	if (default_name_.isEmpty())
		default_name_ = name;

	name_ = name;

	// Use this name also for the QObject instance
	setObjectName(name_);

	name_changed();
}

QString Session::save_path() const
{
	return save_path_;
}

void Session::set_save_path(QString path)
{
	save_path_ = path;
}

const vector< shared_ptr<views::ViewBase> > Session::views() const
{
	return views_;
}

shared_ptr<views::ViewBase> Session::main_view() const
{
	return main_view_;
}

void Session::set_main_bar(shared_ptr<pv::toolbars::MainBar> main_bar)
{
	main_bar_ = main_bar;
}

shared_ptr<pv::toolbars::MainBar> Session::main_bar() const
{
	return main_bar_;
}

bool Session::data_saved() const
{
	return data_saved_;
}


void Session::save_setup(QSettings &settings) const
{
	int i;
	int decode_signal_count = 0;
	int gen_signal_count = 0;

	// Save sample count and sample rate from MainBar
	if (main_bar_) {
		main_bar_->save_setup(settings);
	}

	// Save channels and decoders
	for (const shared_ptr<data::SignalBase>& base : signalbases_) {
#ifdef ENABLE_DECODE
		if (base->is_decode_signal()) {
			settings.beginGroup("decode_signal" + QString::number(decode_signal_count++));
			base->save_settings(settings);
			settings.endGroup();
		} else
#endif
		if (base->is_generated()) {
			settings.beginGroup("generated_signal" + QString::number(gen_signal_count++));
			settings.setValue("type", base->type());
			base->save_settings(settings);
			settings.endGroup();
		} else {
			settings.beginGroup(base->internal_name());
			base->save_settings(settings);
			settings.endGroup();
		}
	}

	settings.setValue("decode_signals", decode_signal_count);
	settings.setValue("generated_signals", gen_signal_count);

	// Save view states and their signal settings
	// Note: main_view must be saved as view0
	i = 0;
	settings.beginGroup("view" + QString::number(i++));
	main_view_->save_settings(settings);
	settings.endGroup();

	for (const shared_ptr<views::ViewBase>& view : views_) {
		if (view != main_view_) {
			settings.beginGroup("view" + QString::number(i++));
			settings.setValue("type", view->get_type());
			view->save_settings(settings);
			settings.endGroup();
		}
	}

	settings.setValue("views", i);

	int view_id = 0;
	i = 0;
	for (const shared_ptr<views::ViewBase>& vb : views_) {
		shared_ptr<views::trace::View> tv = dynamic_pointer_cast<views::trace::View>(vb);
		if (tv) {
			for (const shared_ptr<views::trace::TimeItem>& time_item : tv->time_items()) {

				const shared_ptr<views::trace::Flag> flag =
					dynamic_pointer_cast<views::trace::Flag>(time_item);
				if (flag) {
					if (!flag->enabled())
						continue;

					settings.beginGroup("meta_obj" + QString::number(i++));
					settings.setValue("type", "time_marker");
					settings.setValue("assoc_view", view_id);
					GlobalSettings::store_timestamp(settings, "time", flag->time());
					settings.setValue("text", flag->get_text());
					settings.endGroup();
				}
			}

			if (tv->cursors_shown()) {
				settings.beginGroup("meta_obj" + QString::number(i++));
				settings.setValue("type", "selection");
				settings.setValue("assoc_view", view_id);
				const shared_ptr<views::trace::CursorPair> cp = tv->cursors();
				GlobalSettings::store_timestamp(settings, "start_time", cp->first()->time());
				GlobalSettings::store_timestamp(settings, "end_time", cp->second()->time());
				settings.endGroup();
			}
		}

		view_id++;
	}
	settings.setValue("meta_objs", i);
}

void Session::save_settings(QSettings &settings) const
{
	map<string, string> dev_info;
	list<string> key_list;

	if (device_) {
		shared_ptr<devices::HardwareDevice> hw_device =
			dynamic_pointer_cast< devices::HardwareDevice >(device_);

		if (hw_device) {
			WchDeviceType dev_type = get_wch_device_type_by_driver(
				hw_device->hardware_device()->driver()->name());
			if (dev_type == WchDeviceType::CH569)
				settings.setValue("device_type", "USB3.0(CH569) Logic");
			else if (dev_type == WchDeviceType::CH32H417)
				settings.setValue("device_type", "USB3.0(CH32H417) Logic");
			else
				settings.setValue("device_type", "hardware");
			settings.beginGroup("device");

			key_list.emplace_back("vendor");
			key_list.emplace_back("model");
			key_list.emplace_back("version");
			key_list.emplace_back("serial_num");
			key_list.emplace_back("connection_id");

			dev_info = device_manager_.load()->get_device_info(device_);

			for (string& key : key_list) {
				if (dev_info.count(key))
					settings.setValue(QString::fromUtf8(key.c_str()),
							QString::fromUtf8(dev_info.at(key).c_str()));
				else
					settings.remove(QString::fromUtf8(key.c_str()));
			}

			settings.endGroup();
		}
#if 0
		// Having saved the data to srzip overrides the current device. This is
		// a crappy hack around the fact that saving e.g. an imported file to
		// srzip would require changing the underlying libsigrok device
		if (!save_path_.isEmpty()) {
			QFileInfo fi = QFileInfo(QDir(save_path_), name_);
			settings.setValue("device_type", "sessionfile");
			settings.beginGroup("device");
			settings.setValue("filename", fi.absoluteFilePath());
			settings.endGroup();
		} else {
			shared_ptr<devices::SessionFile> sessionfile_device =
				dynamic_pointer_cast<devices::SessionFile>(device_);

			if (sessionfile_device) {
				settings.setValue("device_type", "sessionfile");
				settings.beginGroup("device");
				settings.setValue("filename", QString::fromStdString(
					sessionfile_device->full_name()));
				settings.endGroup();
			}

			shared_ptr<devices::InputFile> inputfile_device =
				dynamic_pointer_cast<devices::InputFile>(device_);

			if (inputfile_device) {
				settings.setValue("device_type", "inputfile");
				settings.beginGroup("device");
				inputfile_device->save_meta_to_settings(settings);
				settings.endGroup();
			}
		}
#endif
		save_setup(settings);
	}
}

void Session::restore_setup(QSettings &settings)
{
#if 1
	// Restore sample count and sample rate to MainBar
	if (main_bar_) {
		main_bar_->restore_setup(settings);
	}

	// Restore channels
	for (shared_ptr<data::SignalBase> base : signalbases_) {
		QString group_key = base->internal_name();
		if (!settings.childGroups().contains(group_key)) {
			QString prefix = (base->type() == data::SignalBase::AnalogChannel) ? "A" : "D";
			QString fallback_key = prefix + QString::number(base->index());
			if (settings.childGroups().contains(fallback_key))
				group_key = fallback_key;
		}
		settings.beginGroup(group_key);
		base->restore_settings(settings);
		settings.endGroup();
	}

	for (shared_ptr<data::SignalBase> base : signalbases_) {
		if (!base->enabled() && !base->is_generated()) {
			for (shared_ptr<views::ViewBase>& viewbase : views_) {
				viewbase->remove_signalbase(base);
			}
		}
	}

	// Restore generated signals
	int gen_signal_count = settings.value("generated_signals").toInt();

	for (int i = 0; i < gen_signal_count; i++) {
		settings.beginGroup("generated_signal" + QString::number(i));
		SignalBase::ChannelType type = (SignalBase::ChannelType)settings.value("type").toInt();
		shared_ptr<data::SignalBase> signal;

		switch (type) {
			case SignalBase::MathChannel:
				signal = make_shared<data::MathSignal>(*this);
				break;
			case SignalBase::DecodeChannel:
				signal = make_shared<data::DecodeSignal>(*this);
				break;
			default:
				qWarning() << tr("Can't restore generated signal of unknown type %1 (%2)") \
				.arg((int)type) \
				.arg(settings.value("name").toString());
				break;
		}
		if (signal) {
			add_generated_signal(signal);
			signal->restore_settings(settings);
		}
		settings.endGroup();
	}

	// Restore decoders
#if 1
	int decode_signal_count = settings.value("decode_signals").toInt();
	for (int i = 0; i < decode_signal_count; i++) {
		settings.beginGroup("decode_signal" + QString::number(i));
		shared_ptr<data::DecodeSignal> signal = add_decode_signal();
		signal->restore_settings(settings);
		settings.endGroup();
	}
#endif

	// Restore views
	int views = settings.value("views").toInt();

	for (int i = 0; i < views; i++) {
		settings.beginGroup("view" + QString::number(i));
		if (i > 0) {
			views::ViewType type = (views::ViewType)settings.value("type").toInt();
			if (type != views::ViewTypeDecoderBinary && type != views::ViewTypeTabularDecoder)
				add_view(type, this);
			views_.back()->restore_settings(settings);
		} else
			main_view_->restore_settings(settings);

		settings.endGroup();
	}

	// Restore meta objects like markers and cursors
	int meta_objs = settings.value("meta_objs").toInt();

	for (int i = 0; i < meta_objs; i++) {
		settings.beginGroup("meta_obj" + QString::number(i));

		shared_ptr<views::ViewBase> vb;
		shared_ptr<views::trace::View> tv;
		if (settings.contains("assoc_view"))
			vb = views_.at(settings.value("assoc_view").toInt());

		if (vb)
			tv = dynamic_pointer_cast<views::trace::View>(vb);

		const QString type = settings.value("type").toString();

		if ((type == "time_marker") && tv) {
			Timestamp ts = GlobalSettings::restore_timestamp(settings, "time");
			shared_ptr<views::trace::Flag> flag = tv->add_flag(ts);
			flag->set_text(settings.value("text").toString());
		}

		if ((type == "selection") && tv) {
			Timestamp start = GlobalSettings::restore_timestamp(settings, "start_time");
			Timestamp end = GlobalSettings::restore_timestamp(settings, "end_time");
			tv->set_cursors(start, end);
			tv->show_cursors();
		}

		settings.endGroup();
	}
#endif
}

void Session::restore_settings(QSettings &settings)
{
	shared_ptr<devices::Device> device;

	const QString device_type = settings.value("device_type").toString();
	if (device_type == "USB3.0(CH569) Logic" || device_type == "USB3.0(CH32H417) Logic") {
		bool already_demo = false;
		if (device_) {
			shared_ptr<devices::HardwareDevice> hw_dev =
				dynamic_pointer_cast<devices::HardwareDevice>(device_);
			if (hw_dev && hw_dev->hardware_device()->driver()->name() == "demo") {
				already_demo = true;
				qDebug() << "Current device is already demo, skipping restore_settings";
			}
		}

		if (already_demo) {
			return;
		}

		const list< shared_ptr<devices::HardwareDevice> > &devices =
			device_manager_.load()->devices();

		shared_ptr<devices::HardwareDevice> connected_device;
		for (auto dev : devices) {
			WchDeviceType dev_type = get_wch_device_type_by_driver(
				dev->hardware_device()->driver()->name());
			if (dev_type == WchDeviceType::CH569) {
				connected_device = dev;
				break;
			} else if (dev_type == WchDeviceType::CH32H417 && !connected_device) {
				connected_device = dev;
			}
		}

		if (connected_device) {
			device = connected_device;
			qDebug() << "Found connected device, using it instead of saved settings";
		}

		if (device)
			set_device(device);
		else {
			// 没有找到任何设备，使用默认设备选择demo
			qDebug() << "No device found, selecting default device";
			set_default_device();
		}

		bool is_demo = false;
		if (device_) {
			shared_ptr<devices::HardwareDevice> hw_dev =
				dynamic_pointer_cast<devices::HardwareDevice>(device_);
			if (hw_dev && hw_dev->hardware_device()->driver()->name() == "demo") {
				is_demo = true;
			}
		}

		if (!is_demo) {
			restore_setup(settings);
		}
	}
#if 0
	QString filename;
	if ((device_type == "sessionfile") || (device_type == "inputfile")) {
		if (device_type == "sessionfile") {
			settings.beginGroup("device");
			filename = settings.value("filename").toString();
			settings.endGroup();

			if (QFileInfo(filename).isReadable())
				device = make_shared<devices::SessionFile>(device_manager_.context(),
					filename.toStdString());
		}

		if (device_type == "inputfile") {
			settings.beginGroup("device");
			device = make_shared<devices::InputFile>(device_manager_.context(),
				settings);
			settings.endGroup();
		}


		if (device) {
			set_device(device);
			restore_setup(settings);

			start_capture([](QString infoMessage) {
				// TODO Emulate noquote()
				qDebug() << "Session error:" << infoMessage; });

			set_name(QString::fromStdString(
				dynamic_pointer_cast<devices::File>(device)->display_name(device_manager_)));

			if (!filename.isEmpty()) {
				// Only set the save path if we load an srzip file
				if (device_type == "sessionfile")
					set_save_path(QFileInfo(filename).absolutePath());

				set_name(QFileInfo(filename).fileName());
			}
		}
	}
#endif
}

void Session::on_show_Error()
{
	out_of_memory_ = true;
	switch (get_capture_state()) {
		case Session::Stopped:	
			break;
		case Session::AwaitingTrigger:
		case Session::Running:
			stop_capture();
			break;
	}
	mainwindow_show_error(tr("Out of memory, acquisition stopped."));
}

void Session::on_info_received()
{
	mainwindow_show_info(tr("this feature is currently not supported."));
}

void Session::on_renew_last_decoder_name(QString name)
{
	decoder_name_last_ = name;
}

void Session::select_device(shared_ptr<devices::Device> device)
{
	try {
		if (device){
			set_device(device);
		}
		else {
			set_default_device();
		}
	} catch (const QString &e) {
		// MainWindow::show_session_error(tr("Failed to select device"), e);
	}
}

void Session::set_device(shared_ptr<devices::Device> device)
{
	assert(device);
	// Ensure we are not capturing before setting the device
	stop_capture();
	if (device_)
		device_->close();
	device_.reset();
	// Revert name back to default name (e.g. "Session 1") as the data is gone
	name_ = default_name_;
	name_changed();
	save_path_.clear();

	// Remove all stored data and reset all views
	for (shared_ptr<views::ViewBase> view : views_) {
		view->clear_signalbases();
#ifdef ENABLE_DECODE
		view->clear_decode_signals();
#endif
		view->reset_view_state();
	}

	for (SignalGroup* group : signal_groups_) {
		group->clear();
		delete group;
	}
	signal_groups_.clear();

	for (const shared_ptr<data::SignalData>& d : all_signal_data_)
		d->clear();

	all_signal_data_.clear();
	signalbases_.clear();
	cur_logic_segment_.reset();
	
	for (auto& entry : cur_analog_segments_) {
		shared_ptr<sigrok::Channel>(entry.first).reset();
		shared_ptr<data::AnalogSegment>(entry.second).reset();
	}
	logic_data_.reset();

	signals_changed();

	device_ = move(device);

	try {
		device_->open();
	} catch (const QString &e) {
		device_.reset();
		// MainWindow::show_session_error(tr("Failed to open device"), e);
	}
	
	if (device_) {
		device_->session()->add_datafeed_callback([=]
			(shared_ptr<sigrok::Device> device, shared_ptr<Packet> packet) {
				data_feed_in(device, packet);
			});

		update_signals();
	}
	device_changed();
}

void Session::set_default_device()
{
	shared_ptr<devices::Device> default_device = NULL;
	const list< shared_ptr<devices::HardwareDevice> > &devices =
		device_manager_.load()->devices();
	if (!devices.empty()) {
		default_device = devices.front();

		for (auto dev : devices) {
			WchDeviceType dev_type = get_wch_device_type_by_driver(
				dev->hardware_device()->driver()->name());
			if (dev_type == WchDeviceType::CH569) {
				default_device = dev;
				break;
			} else if (dev_type == WchDeviceType::CH32H417 && default_device == devices.front()) {
				default_device = dev;
			}
		}
	}
	if (default_device != NULL){
      set_device(default_device);
    }
}	

bool Session::using_file_device() const
{
	shared_ptr<devices::SessionFile> sessionfile_device =
		dynamic_pointer_cast<devices::SessionFile>(device_);

	shared_ptr<devices::InputFile> inputfile_device =
		dynamic_pointer_cast<devices::InputFile>(device_);

	return (sessionfile_device || inputfile_device);
}

/**
 * Convert generic options to data types that are specific to InputFormat.
 *
 * @param[in] user_spec Vector of tokenized words, string format.
 * @param[in] fmt_opts Input format's options, result of InputFormat::options().
 *
 * @return Map of options suitable for InputFormat::create_input().
 */
map<string, Glib::VariantBase>
Session::input_format_options(vector<string> user_spec,
		map<string, shared_ptr<Option>> fmt_opts)
{
	map<string, Glib::VariantBase> result;

	for (auto& entry : user_spec) {
		/*
		 * Split key=value specs. Accept entries without separator
		 * (for simplified boolean specifications).
		 */
		string key, val;
		size_t pos = entry.find("=");
		if (pos == std::string::npos) {
			key = entry;
			val = "";
		} else {
			key = entry.substr(0, pos);
			val = entry.substr(pos + 1);
		}

		/*
		 * Skip user specifications that are not a member of the
		 * format's set of supported options. Have the text input
		 * spec converted to the required input format specific
		 * data type.
		 */
		auto found = fmt_opts.find(key);
		if (found == fmt_opts.end()) {
			qCritical() << "Supplied input option" << QString::fromStdString(key) <<
				"is not a valid option for this input module, it will be ignored!";
			continue;
		}

		shared_ptr<Option> opt = found->second;
		result[key] = opt->parse_string(val);
	}

	return result;
}

void Session::load_init_file(const string &file_name,
	const string &format, const string &setup_file_name)
{
	shared_ptr<InputFormat> input_format;
	map<string, Glib::VariantBase> input_opts;

	if (!format.empty()) {
		const map<string, shared_ptr<InputFormat> > formats =
			device_manager_.load()->context()->input_formats();
		auto user_opts = pv::util::split_string(format, ":");
		string user_name = user_opts.front();
		user_opts.erase(user_opts.begin());
		const auto iter = find_if(formats.begin(), formats.end(),
			[=](const pair<string, shared_ptr<InputFormat> > f) {
				return f.first == user_name; });
		if (iter == formats.end()) {
			MainWindow::show_session_error(tr("Error"),
				tr("Unexpected input format: %1").arg(QString::fromStdString(format)));
			return;
		}
		input_format = (*iter).second;
		input_opts = input_format_options(user_opts,
			input_format->options());
	}

	load_file(QString::fromStdString(file_name), QString::fromStdString(setup_file_name),
		input_format, input_opts);
}

void Session::load_file(QString file_name, QString setup_file_name,
	shared_ptr<sigrok::InputFormat> format, const map<string, Glib::VariantBase> &options)
{	
	const QString errorMessage(
		QString("Failed to load file %1").arg(file_name));

	// In the absence of a caller's format spec, try to auto detect.
	// Assume "sigrok session file" upon lookup miss.
	if (!format)
		format = device_manager_.load()->context()->input_format_match(file_name.toStdString());
	try {
		if (format)
			set_device(shared_ptr<devices::Device>(
				new devices::InputFile(
					device_manager_.load()->context(),
					file_name.toStdString(),
					format, options)));
		else
			set_device(shared_ptr<devices::Device>(
				new devices::SessionFile(
					device_manager_.load()->context(),
					file_name.toStdString())));
	} catch (Error& e) {
		MainWindow::show_session_error(tr("Failed to load %1").arg(file_name), e.what());
		set_default_device();
		main_bar_->update_device_list();
		return;
	}

	// Use the input file with .pvs extension if no setup file was given
	if (setup_file_name.isEmpty()) {
		setup_file_name = file_name;
		setup_file_name.truncate(setup_file_name.lastIndexOf('.'));
		setup_file_name.append(".pvs");
	}

	if (QFileInfo::exists(setup_file_name) && QFileInfo(setup_file_name).isReadable()) {
		QSettings settings_storage(setup_file_name, QSettings::IniFormat);
		restore_setup(settings_storage);
	}

	main_bar_->update_device_list();

	start_capture([&, errorMessage](QString infoMessage) {
		MainWindow::show_session_error(errorMessage, infoMessage); });

	// Only set save path if we loaded an srzip file
	if (dynamic_pointer_cast<devices::SessionFile>(device_))
		set_save_path(QFileInfo(file_name).absolutePath());

	set_name(QFileInfo(file_name).fileName());
}

Session::capture_state Session::get_capture_state() const
{
	lock_guard<mutex> lock(sampling_mutex_);
	return capture_state_;
}
void Session::start_capture(function<void (const QString)> error_handler)
{
	if (!device_) {
		// error_handler(tr("No active device set, can't start acquisition."));
		mainwindow_show_error(tr("No active device set, can't start acquisition."));
		return;
	}
	
	stop_capture();

	// Check that at least one channel is enabled
	const shared_ptr<sigrok::Device> sr_dev = device_->device();
	if (sr_dev) {
		const auto channels = sr_dev->channels();
		if (!any_of(channels.begin(), channels.end(),
			[](shared_ptr<Channel> channel) {
				return channel->enabled(); })) {
			// error_handler(tr("No channels enabled."));
			mainwindow_show_error(tr("No channels enabled."));
			return;
		}
	}

	// Clear signal data
	for (const shared_ptr<data::SignalData>& d : all_signal_data_)
		d->clear();

	trigger_list_.clear();
	segment_sample_count_.clear();

	// Revert name back to default name (e.g. "Session 1") for real devices
	// as the (possibly saved) data is gone. File devices keep their name.
	shared_ptr<devices::HardwareDevice> hw_device =
		dynamic_pointer_cast< devices::HardwareDevice >(device_);

	if (hw_device) {
		name_ = default_name_;
		name_changed();
	}
	get_repeat_acquisition(is_repeat_acquisition_);
	
	if (sampling_thread_.joinable()) {
		sampling_thread_.join();
	}
	if (is_repeat_acquisition_){
		if (main_bar_->get_sample_count_index() < 3)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!pre_trigger_ptr_ && is_repeat_acquisition_)
		pre_trigger_ptr_ = device_->session()->trigger();
	// Begin the session
	sampling_thread_ = std::thread(&Session::sample_thread_proc, this, error_handler);
}

void Session::stop_capture()
{
	if (pre_trigger_ptr_ != nullptr && is_repeat_acquisition_){
		device_->session()->set_trigger(pre_trigger_ptr_);
		pre_trigger_ptr_ = nullptr;
	}
	is_repeat_acquisition_ = false;
	if (get_capture_state() != Stopped){
		device_->stop();
	}
	// Check that sampling stopped
	if (sampling_thread_.joinable())
		sampling_thread_.join();
}

void Session::register_view(shared_ptr<views::ViewBase> view)
{
	if (views_.empty())
		main_view_ = view;

	views_.push_back(view);

	// Add all device signals
	update_signals();

	// Add all other signals
	vector< shared_ptr<data::SignalBase> > view_signalbases = view->signalbases();

	for (const shared_ptr<data::SignalBase>& signalbase : signalbases_) {
		const int sb_exists = count_if(
			view_signalbases.cbegin(), view_signalbases.cend(),
			[=](const shared_ptr<data::SignalBase> &sb) {
				return sb == signalbase;
			});

		// Add the signal to the view if it doesn't have it yet
		if (!sb_exists)
			switch (signalbase->type()) {
			case data::SignalBase::AnalogChannel:
			case data::SignalBase::LogicChannel:
			case data::SignalBase::MathChannel:
				view->add_signalbase(signalbase);
				break;
			case data::SignalBase::DecodeChannel:
#ifdef ENABLE_DECODE
				view->add_decode_signal(dynamic_pointer_cast<data::DecodeSignal>(signalbase));
#endif
				break;
			}
	}

	signals_changed();
}

void Session::deregister_view(shared_ptr<views::ViewBase> view)
{
	views_.erase(std::remove_if(views_.begin(), views_.end(),
		[=](shared_ptr<views::ViewBase> v) { return v == view; }),
		views_.end());

	if (views_.empty()) {
		main_view_.reset();

		// Without a view there can be no main bar
		main_bar_.reset();
	}
}

bool Session::has_view(shared_ptr<views::ViewBase> view)
{
	for (shared_ptr<views::ViewBase>& v : views_)
		if (v == view)
			return true;

	return false;
}

double Session::get_samplerate() const
{
	double samplerate = 0.0;
	for (const shared_ptr<pv::data::SignalData>& d : all_signal_data_) {
		assert(d);
		const vector< shared_ptr<pv::data::Segment> > segments =
			d->segments();
		for (const shared_ptr<pv::data::Segment>& s : segments){
			samplerate = max(samplerate, s->samplerate());
		}
	}
	// If there is no sample rate given we use samples as unit
	if (samplerate == 0.0)
		samplerate = 1.0;

	return samplerate;
}

uint32_t Session::get_highest_segment_id() const
{
	return highest_segment_id_;
}

uint64_t Session::get_segment_sample_count(uint32_t segment_id) const
{
	if (segment_id < segment_sample_count_.size())
		return segment_sample_count_[segment_id];
	else
		return 0;
}

vector<util::Timestamp> Session::get_triggers(uint32_t segment_id) const
{
	vector<util::Timestamp> result;

	for (const pair<uint32_t, util::Timestamp>& entry : trigger_list_)
		if (entry.first == segment_id)
			result.push_back(entry.second);

	return result;
}

const vector< shared_ptr<data::SignalBase> > Session::signalbases() const
{
	return signalbases_;
}

uint32_t Session::get_signal_count(data::SignalBase::ChannelType type) const
{
	return count_if(signalbases_.begin(), signalbases_.end(),
		[=] (shared_ptr<SignalBase> sb) { return sb->type() == type; });
}

uint32_t Session::get_next_signal_index(data::SignalBase::ChannelType type)
{
	next_index_list_[type]++;
	return next_index_list_[type];
}

void Session::add_generated_signal(shared_ptr<data::SignalBase> signal)
{
	signalbases_.push_back(signal);

	for (shared_ptr<views::ViewBase>& view : views_)
		view->add_signalbase(signal);

	update_signals();
}

void Session::remove_generated_signal(shared_ptr<data::SignalBase> signal)
{
	if (shutting_down_)
		return;

	signalbases_.erase(std::remove_if(signalbases_.begin(), signalbases_.end(),
		[=](shared_ptr<data::SignalBase> s) { return s == signal; }),
		signalbases_.end());

	for (shared_ptr<views::ViewBase>& view : views_)
		view->remove_signalbase(signal);

	update_signals();
}

#if 1
shared_ptr<data::DecodeSignal> Session::add_decode_signal()
{
	shared_ptr<data::DecodeSignal> signal;

	try {
		// Create the decode signal
		signal = make_shared<data::DecodeSignal>(*this);

		signalbases_.push_back(signal);

		// Add the decode signal to all views
		for (shared_ptr<views::ViewBase>& view : views_)
			view->add_decode_signal(signal);
	} catch (runtime_error& e) {
		remove_decode_signal(signal);
		return nullptr;
	}
	signals_changed();
	decode_signal_map_.insert(pair<shared_ptr<data::DecodeSignal>, QString>(signal, decoder_name_last_));
	return signal;
}

void Session::remove_decode_signal(shared_ptr<data::DecodeSignal> signal)
{
	if (shutting_down_)
		return;

	// Restore original signal names, but only if no other decoder still uses them
	for (const auto& ch : signal->get_channels()) {
		if (!ch.assigned_signal || ch.assigned_signal->internal_name().isEmpty())
			continue;

		// Check if any other DecodeSignal still references this signal
		bool still_used = false;
		for (const auto& sb : signalbases_) {
			if (sb.get() == signal.get() || !sb->is_decode_signal())
				continue;
			auto other = dynamic_pointer_cast<data::DecodeSignal>(sb);
			if (!other) continue;
			for (const auto& other_ch : other->get_channels()) {
				if (other_ch.assigned_signal == ch.assigned_signal) {
					still_used = true;
					break;
				}
			}
			if (still_used) break;
		}

		if (!still_used) {
			std::const_pointer_cast<data::SignalBase>(ch.assigned_signal)
				->set_name(ch.assigned_signal->internal_name());
		}
	}

	signalbases_.erase(std::remove_if(signalbases_.begin(), signalbases_.end(),
		[=](shared_ptr<data::SignalBase> s) { return s == signal; }),
		signalbases_.end());

	for (shared_ptr<views::ViewBase>& view : views_)
		view->remove_decode_signal(signal);
	decode_signal_map_.erase(signal);
	signals_changed();
}

void Session::create_demo_uart_decoder()
{
	const srd_decoder* uart_decoder = nullptr;
	for (GSList* li = (GSList*)srd_decoder_list(); li; li = li->next) {
		const srd_decoder* d = (srd_decoder*)li->data;
		if (strcmp(d->id, "uart") == 0) {
			uart_decoder = d;
			break;
		}
	}

	if (!uart_decoder) {
		qDebug() << "UART decoder not found";
		return;
	}

	if (views_.empty()) {
		qDebug() << "Views not ready, cannot create decoder";
		return;
	}

	shared_ptr<data::DecodeSignal> signal = add_decode_signal();
	if (!signal) {
		return;
	}

	signal->stack_decoder(uart_decoder, false);

	// Auto-assign TX→D0, RX→D1 for demo device
	// Use assign_signal_no_decode to avoid premature begin_decode()
	{
		shared_ptr<data::SignalBase> d0, d1;
		for (const shared_ptr<data::SignalBase>& sb : signalbases_) {
			if (sb->internal_name() == "D0") d0 = sb;
			if (sb->internal_name() == "D1") d1 = sb;
		}
		const auto channels = signal->get_channels();
		for (const auto& ch : channels) {
			QString ch_name = ch.name.toLower();
			if (ch_name.contains("tx") && d0)
				signal->assign_signal_no_decode(ch.id, d0);
			else if (ch_name.contains("rx") && d1)
				signal->assign_signal_no_decode(ch.id, d1);
		}
	}

	// Set baudrate=115200
	for (GSList* l = uart_decoder->options; l; l = l->next) {
		const srd_decoder_option* opt = (srd_decoder_option*)l->data;
		if (strcmp(opt->id, "baudrate") == 0) {
			GVariant* baud_value = nullptr;
			if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_UINT64)) {
				baud_value = g_variant_new_uint64(115200);
			} else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_UINT32)) {
				baud_value = g_variant_new_uint32(115200);
			} else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_INT64)) {
				baud_value = g_variant_new_int64(115200);
			} else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_INT32)) {
				baud_value = g_variant_new_int32(115200);
			} else {
				qDebug() << "Unknown baudrate type, using default";
				continue;
			}
			if (baud_value && !signal->decoder_stack().empty()) {
				signal->decoder_stack().back()->set_option("baudrate", baud_value);
			}
			break;
		}
	}

	// Set data_format to ascii
	for (GSList* l = uart_decoder->options; l; l = l->next) {
		const srd_decoder_option* opt = (srd_decoder_option*)l->data;
		if (strcmp(opt->id, "format") == 0) {
			GVariant* format_value = g_variant_new_string("ascii");
			if (format_value && !signal->decoder_stack().empty()) {
				signal->decoder_stack().back()->set_option("format", format_value);
			}
			break;
		}
	}

	// All configuration done, start decode once
	signal->begin_decode();
}
#endif

bool Session::all_segments_complete(uint32_t segment_id) const
{
	bool all_complete = true;

	for (const shared_ptr<data::SignalBase>& base : signalbases_)
		if (!base->segment_is_complete(segment_id))
			all_complete = false;

	return all_complete;
}

MetadataObjManager* Session::metadata_obj_manager()
{
	return &metadata_obj_manager_;
}

void Session::set_capture_state(capture_state state)
{
	if (state == capture_state_)
		return;

	if (state == Running)
		acq_time_.restart();
	if (state == Stopped)
		qDebug("Acquisition took %.2f s", acq_time_.elapsed() / 1000.);

	{
		lock_guard<mutex> lock(sampling_mutex_);
		capture_state_ = state;
	}

	capture_state_changed(state);
}

void Session::update_signals()
{
	if (!device_) {
		signalbases_.clear();
		logic_data_.reset();
		for (shared_ptr<views::ViewBase>& view : views_) {
			view->clear_signalbases();
#ifdef ENABLE_DECODE
			view->clear_decode_signals();
#endif
		}
		return;
	}

	lock_guard<recursive_mutex> lock(data_mutex_);

	const shared_ptr<sigrok::Device> sr_dev = device_->device();
	if (!sr_dev) {
		signalbases_.clear();
		logic_data_.reset();
		for (shared_ptr<views::ViewBase>& view : views_) {
			view->clear_signalbases();
#ifdef ENABLE_DECODE
			view->clear_decode_signals();
#endif
		}
		return;
	}

	// Detect what data types we will receive
	auto channels = sr_dev->channels();
	unsigned int logic_channel_count = count_if(
		channels.begin(), channels.end(),
		[] (shared_ptr<Channel> channel) {
			return channel->type() == sigrok::ChannelType::LOGIC; });
	logic_channel_count = main_bar_->get_channel_number();
	logic_channel_count_ = logic_channel_count;
	// Create a common data container for the logic signalbases
	{
		lock_guard<recursive_mutex> data_lock(data_mutex_);

		if (logic_channel_count == 0) {
			logic_data_.reset();
		} else if (!logic_data_) {
			// No existing logic data, create new one
			logic_data_.reset(new data::Logic(logic_channel_count));
			assert(logic_data_);
			for (auto& sb : signalbases_) {
				if (sb->type() == data::SignalBase::LogicChannel)
					sb->set_data(logic_data_);
			}
			for (auto it = all_signal_data_.begin(); it != all_signal_data_.end(); ) {
				if (dynamic_pointer_cast<data::Logic>(*it))
					it = all_signal_data_.erase(it);
				else
					++it;
			}
			all_signal_data_.insert(logic_data_);
		} else if (logic_data_->num_channels() != logic_channel_count) {
			if (logic_channel_count > logic_data_->num_channels()) {
				logic_data_->set_num_channels(logic_channel_count);
			}
		}
	}

	// Create signalbases if necessary
	for (auto channel : sr_dev->channels()) {

		// Try to find the channel in the list of existing signalbases
		const auto iter = find_if(signalbases_.cbegin(), signalbases_.cend(),
			[=](const shared_ptr<SignalBase> &sb) { return sb->channel() == channel; });

		// Not found, let's make a signalbase for it
		if (iter == signalbases_.cend()) {
			shared_ptr<SignalBase> signalbase;
			switch(channel->type()->id()) {
			case SR_CHANNEL_LOGIC:
				signalbase = make_shared<data::SignalBase>(channel, data::SignalBase::LogicChannel);
				signalbases_.push_back(signalbase);

				all_signal_data_.insert(logic_data_);
				signalbase->set_data(logic_data_);

				connect(this, SIGNAL(capture_state_changed(int)),
					signalbase.get(), SLOT(on_capture_state_changed(int)));
				
				break;

			case SR_CHANNEL_ANALOG:
				signalbase = make_shared<data::SignalBase>(channel, data::SignalBase::AnalogChannel);
				signalbases_.push_back(signalbase);

				shared_ptr<data::Analog> data(new data::Analog());
				all_signal_data_.insert(data);
				signalbase->set_data(data);

				connect(this, SIGNAL(capture_state_changed(int)),
					signalbase.get(), SLOT(on_capture_state_changed(int)));
				break;
			}
		}
	}

	// Create and assign default signal groups if needed
	if (signal_groups_.empty()) {
		for (auto& entry : sr_dev->channel_groups()) {
			const shared_ptr<sigrok::ChannelGroup>& group = entry.second;

			if (group->channels().size() <= 1)
				continue;

			SignalGroup* sg = new SignalGroup(QString::fromStdString(entry.first));
			for (const shared_ptr<sigrok::Channel>& channel : group->channels()) {
				for (shared_ptr<data::SignalBase> s : signalbases_) {
					if (s->channel() == channel) {
						sg->append_signal(s);
						break;
					}
				}
			}
			signal_groups_.emplace_back(sg);
		}
	}

	// Update all views
	for (shared_ptr<views::ViewBase>& viewbase : views_) {
		vector< shared_ptr<SignalBase> > view_signalbases =
				viewbase->signalbases();

		// Add all non-decode signalbases that don't yet exist in the view
		for (shared_ptr<SignalBase>& session_sb : signalbases_) {
			if (session_sb->type() == SignalBase::DecodeChannel)
				continue;

			if (!session_sb->enabled())
				continue;

			const auto iter = find_if(view_signalbases.cbegin(), view_signalbases.cend(),
				[=](const shared_ptr<SignalBase> &sb) { return sb == session_sb; });

			if (iter == view_signalbases.cend())
				viewbase->add_signalbase(session_sb);
		}

		// Remove all non-decode signalbases that no longer exist
		for (shared_ptr<SignalBase>& view_sb : view_signalbases) {
			if (view_sb->type() == SignalBase::DecodeChannel)
				continue;

			const auto iter = find_if(signalbases_.cbegin(), signalbases_.cend(),
				[=](const shared_ptr<SignalBase> &sb) {	return sb == view_sb; });

			if (iter == signalbases_.cend())
				viewbase->remove_signalbase(view_sb);
		}
	}

	signals_changed();
}

shared_ptr<data::SignalBase> Session::signalbase_from_channel(
	shared_ptr<sigrok::Channel> channel) const
{
	for (shared_ptr<data::SignalBase> sig : signalbases_) {
		assert(sig);
		if (sig->channel() == channel)
			return sig;
	}
	return shared_ptr<data::SignalBase>();
}

void Session::sample_thread_proc(function<void (const QString)> error_handler)
{
	assert(error_handler);
#ifdef ENABLE_FLOW
	pipeline_ = Pipeline::create();

	source_ = ElementFactory::create_element("filesrc", "source");
	sink_ = RefPtr<AppSink>::cast_dynamic(ElementFactory::create_element("appsink", "sink"));

	pipeline_->add(source_)->add(sink_);
	source_->link(sink_);

	source_->set_property("location", Glib::ustring("/tmp/dummy_binary"));

	sink_->set_property("emit-signals", TRUE);
	sink_->signal_new_sample().connect(sigc::mem_fun(*this, &Session::on_gst_new_sample));

	// Get the bus from the pipeline and add a bus watch to the default main context
	RefPtr<Bus> bus = pipeline_->get_bus();
	bus->add_watch(sigc::mem_fun(this, &Session::on_gst_bus_message));

	// Start pipeline and Wait until it finished processing
	pipeline_done_interrupt_ = false;
	pipeline_->set_state(Gst::STATE_PLAYING);

	unique_lock<mutex> pipeline_done_lock_(pipeline_done_mutex_);
	pipeline_done_cond_.wait(pipeline_done_lock_);

	// Let the pipeline free all resources
	pipeline_->set_state(Gst::STATE_NULL);

#else
	if (!device_)
		return;

	try {
		cur_samplerate_ = device_->read_config<uint64_t>(ConfigKey::SAMPLERATE);
	} catch (Error& e) {
		cur_samplerate_ = 0;
	}
	out_of_memory_ = false;
	{
		lock_guard<recursive_mutex> lock(data_mutex_);
		cur_logic_segment_.reset();
		cur_analog_segments_.clear();
		for (shared_ptr<data::SignalBase> sb : signalbases_)
			sb->clear_sample_data();
	}
	highest_segment_id_ = -1;
	frame_began_ = false;
	try {
		device_->start();
	} catch (Error& e) {
		mainwindow_show_error(e.what());
		return;
	}
	set_capture_state(device_->session()->trigger() ?
		AwaitingTrigger : Running);
	try {
		device_->run();	
	} catch (Error& e) {
		mainwindow_show_error(e.what());
		return;
	} catch (QString& e) {
		mainwindow_show_error(e);
		return;
	}
	
	set_capture_state(Stopped);
	// Confirm that SR_DF_END was received
	if (cur_logic_segment_)
		qDebug() << "WARNING: SR_DF_END was not received.";
#endif

	// Optimize memory usage
	free_unused_memory();

	// We now have unsaved data unless we just "captured" from a file
	shared_ptr<devices::File> file_device =
		dynamic_pointer_cast<devices::File>(device_);

	if (!file_device)
		data_saved_ = false;
	if (is_repeat_acquisition_)
		notify_acq_done();
}

void Session::on_notify_acq_done()
{
	for (const shared_ptr<data::SignalData>& d : all_signal_data_)
		d->clear();
	if (sampling_thread_.joinable())
		sampling_thread_.join();
	usleep(200000);
	start_capture([&](QString message) {
		qDebug() << "Capture failed";
	}); 
	device_->session()->set_trigger(nullptr);
}

void Session::free_unused_memory()
{
	for (const shared_ptr<data::SignalData>& data : all_signal_data_) {
		const vector< shared_ptr<data::Segment> > segments = data->segments();

		for (const shared_ptr<data::Segment>& segment : segments)
			segment->free_unused_memory();
	}
}

void Session::signal_new_segment()
{
	int new_segment_id = 0;

	if ((cur_logic_segment_ != nullptr) || !cur_analog_segments_.empty()) {

		// Determine new frame/segment number, assuming that all
		// signals have the same number of frames/segments
		if (cur_logic_segment_) {
			new_segment_id = logic_data_->get_segment_count() - 1;
		} else {
			shared_ptr<sigrok::Channel> any_channel =
				(*cur_analog_segments_.begin()).first;

			shared_ptr<data::SignalBase> base = signalbase_from_channel(any_channel);
			assert(base);

			shared_ptr<data::Analog> data(base->analog_data());
			assert(data);

			new_segment_id = data->get_segment_count() - 1;
		}
	}

	if (new_segment_id > highest_segment_id_) {
		highest_segment_id_ = new_segment_id;
		segment_sample_count_.emplace_back(0);
		new_segment(highest_segment_id_);
	}
}

void Session::signal_segment_completed()
{
	int segment_id = 0;

	for (const shared_ptr<data::SignalBase>& signalbase : signalbases_) {
		// We only care about analog and logic channels, not derived ones
		if (signalbase->type() == data::SignalBase::AnalogChannel) {
			segment_id = signalbase->analog_data()->get_segment_count() - 1;
			break;
		}

		if (signalbase->type() == data::SignalBase::LogicChannel) {
			segment_id = signalbase->logic_data()->get_segment_count() - 1;
			break;
		}
	}

	if (segment_id >= 0)
		segment_completed(segment_id);
}

#ifdef ENABLE_FLOW
bool Session::on_gst_bus_message(const Glib::RefPtr<Gst::Bus>& bus, const Glib::RefPtr<Gst::Message>& message)
{
	(void)bus;

	if ((message->get_source() == pipeline_) && \
		((message->get_message_type() == Gst::MESSAGE_EOS)))
		pipeline_done_cond_.notify_one();

	// TODO Also evaluate MESSAGE_STREAM_STATUS to receive error notifications

	return true;
}

Gst::FlowReturn Session::on_gst_new_sample()
{
	RefPtr<Gst::Sample> sample = sink_->pull_sample();
	RefPtr<Gst::Buffer> buf = sample->get_buffer();

	for (uint32_t block_id = 0; block_id < buf->n_memory(); block_id++) {
		RefPtr<Gst::Memory> buf_mem = buf->get_memory(block_id);
		Gst::MapInfo mapinfo;
		buf_mem->map(mapinfo, Gst::MAP_READ);

		shared_ptr<sigrok::Packet> logic_packet =
			sr_context->create_logic_packet(mapinfo.get_data(), buf->get_size(), 1);

		try {
			feed_in_logic(dynamic_pointer_cast<sigrok::Logic>(logic_packet->payload()));
		} catch (bad_alloc&) {
			out_of_memory_ = true;
			device_->stop();
			buf_mem->unmap(mapinfo);
			return Gst::FLOW_ERROR;
		}

		buf_mem->unmap(mapinfo);
	}

	return Gst::FLOW_OK;
}
#endif

void Session::feed_in_header()
{
	// Nothing to do here for now
}

void Session::feed_in_meta(shared_ptr<Meta> meta)
{
	for (auto& entry : meta->config()) {
		switch (entry.first->id()) {
		case SR_CONF_SAMPLERATE:
			cur_samplerate_ = g_variant_get_uint64(entry.second.gobj());
			break;
		default:
			qDebug() << "Received meta data key" << entry.first->id() << ", ignoring.";
			break;
		}
	}

	signals_changed();
}

void Session::feed_in_trigger(uint64_t trigger_offs)
{
	// The channel containing most samples should be most accurate
	uint64_t sample_count = 0;
	cur_samplerate_ = device_->read_config<uint64_t>(ConfigKey::SAMPLERATE);
	{
		for (const shared_ptr<pv::data::SignalData>& d : all_signal_data_) {
			assert(d);
			uint64_t temp_count = 0;

			const vector< shared_ptr<pv::data::Segment> > segments =
				d->segments();
			for (const shared_ptr<pv::data::Segment> &s : segments)
				temp_count += s->get_sample_count();

			if (temp_count > sample_count)
				sample_count = temp_count;
		}
	}

	uint32_t segment_id = 0;  // Default segment when no frames are used

	// If a frame began, we'd ideally be able to use the highest segment ID for
	// the trigger. However, as new segments are only created when logic or
	// analog data comes in, this doesn't work if the trigger appears right
	// after the beginning of the frame, before any sample data.
	// For this reason, we use highest segment ID + 1 if no sample data came in
	// yet and the highest segment ID otherwise.
	if (frame_began_) {
		segment_id = highest_segment_id_;
		if (!cur_logic_segment_ && (cur_analog_segments_.size() == 0))
			segment_id++;
	}
	// TODO Create timestamp from segment start time + segment's current sample count
	util::Timestamp timestamp = (double)(sample_count + trigger_offs) / cur_samplerate_;
	trigger_list_.emplace_back(segment_id, timestamp);
	trigger_event(highest_segment_id_, timestamp);
}

void Session::feed_in_frame_begin()
{
	frame_began_ = true;
}

void Session::feed_in_frame_end()
{
	if (!frame_began_)
		return;
	{
		lock_guard<recursive_mutex> lock(data_mutex_);

		if (cur_logic_segment_)
			cur_logic_segment_->set_complete();

		for (auto& entry : cur_analog_segments_) {
			shared_ptr<data::AnalogSegment> segment = entry.second;
			segment->set_complete();
		}

		cur_logic_segment_.reset();
		cur_analog_segments_.clear();
	}

	frame_began_ = false;

	signal_segment_completed();
}

void Session::feed_in_logic(shared_ptr<sigrok::Logic> logic)
{
	if (logic->data_length() == 0) {
		qDebug() << "WARNING: Received logic packet with 0 samples.";
		return;
	}

	if (logic->unit_size() > 8)
		throw QString(tr("Can't handle more than 64 logic channels."));
		
	if (!cur_samplerate_) {
		try {
			cur_samplerate_ = device_->read_config<uint64_t>(ConfigKey::SAMPLERATE);
		} catch (Error& e) {
			// Do nothing
		}
	}

	lock_guard<recursive_mutex> lock(data_mutex_);

	if (!logic_data_) {
		// The only reason logic_data_ would not have been created is
		// if it was not possible to determine the signals when the
		// device was created.
		update_signals();
	}
	if (!cur_logic_segment_) {
		// This could be the first packet after a trigger
		set_capture_state(Running);

		// Create a new data segment
		cur_logic_segment_ = make_shared<data::LogicSegment>(
			*logic_data_, logic_data_->get_segment_count(),
			logic->unit_size() , cur_samplerate_,
			main_bar_->get_sample_count());
		logic_data_->push_segment(cur_logic_segment_);
		if (!is_wch_device(main_bar_->get_vendorName()) && main_bar_->get_vendorName() != "demo") {
			logic_channel_count_ = 16;
		} else {
			logic_channel_count_ = main_bar_->get_channel_number();
		}
		cur_logic_segment_->set_channel_numner(logic_channel_count_);
		signal_new_segment();
		connect(cur_logic_segment_.get(), SIGNAL(memoryError()), this, SLOT(on_show_Error()));
	}
	cur_logic_segment_->append_payload(logic);
	segment_sample_count_[highest_segment_id_] =
		max(segment_sample_count_[highest_segment_id_], cur_logic_segment_->get_sample_count());

	data_received();
}

void Session::feed_in_analog(shared_ptr<sigrok::Analog> analog)
{
	if (analog->num_samples() == 0) {
		qDebug() << "WARNING: Received analog packet with 0 samples.";
		return;
	}

	if (!cur_samplerate_)
		try {
			cur_samplerate_ = device_->read_config<uint64_t>(ConfigKey::SAMPLERATE);
		} catch (Error& e) {
			// Do nothing
		}

	lock_guard<recursive_mutex> lock(data_mutex_);

	const vector<shared_ptr<Channel>> channels = analog->channels();
	bool sweep_beginning = false;

	unique_ptr<float[]> data(new float[analog->num_samples() * channels.size()]);
	analog->get_data_as_float(data.get());

	if (signalbases_.empty())
		update_signals();

	float *channel_data = data.get();

	for (auto& channel : channels) {
		shared_ptr<data::AnalogSegment> segment;

		// Try to get the segment of the channel
		const map< shared_ptr<Channel>, shared_ptr<data::AnalogSegment> >::
			iterator iter = cur_analog_segments_.find(channel);
		if (iter != cur_analog_segments_.end())
			segment = (*iter).second;
		else {
			// If no segment was found, this means we haven't
			// created one yet. i.e. this is the first packet
			// in the sweep containing this segment.
			sweep_beginning = true;

			// Find the analog data associated with the channel
			shared_ptr<data::SignalBase> base = signalbase_from_channel(channel);
			assert(base);

			shared_ptr<data::Analog> data(base->analog_data());
			assert(data);

			// Create a segment, keep it in the maps of channels
			segment = make_shared<data::AnalogSegment>(
				*data, data->get_segment_count(), cur_samplerate_);
			cur_analog_segments_[channel] = segment;

			// Push the segment into the analog data.
			data->push_segment(segment);

			signal_new_segment();
		}

		assert(segment);

		// Append the samples in the segment
		segment->append_interleaved_samples(channel_data++, analog->num_samples(),
			channels.size());

		segment_sample_count_[highest_segment_id_] =
			max(segment_sample_count_[highest_segment_id_], segment->get_sample_count());
	}

	if (sweep_beginning) {
		// This could be the first packet after a trigger
		set_capture_state(Running);
	}

	data_received();
}
void Session::device_detached()
{
	// set_capture_state(Stopped);
	// stop_capture();
	main_bar_->reset_device_selector();
}

void Session::data_feed_in(shared_ptr<sigrok::Device> device,
	shared_ptr<Packet> packet)
{
	(void)device;
	assert(device);
	assert(packet);

	uint64_t trigger_offs = 0;
	if (out_of_memory_)
		return;
	switch (packet->type()->id()) {
	case SR_DF_HEADER:
		feed_in_header();
		break;

	case SR_DF_META:
		feed_in_meta(dynamic_pointer_cast<Meta>(packet->payload()));
		break;      

	case SR_DF_TRIGGER:
		trigger_offs = main_bar_->get_trigger_offset();
		feed_in_trigger(trigger_offs);
		break;

	case SR_DF_LOGIC:
		try {
			feed_in_logic(dynamic_pointer_cast<Logic>(packet->payload()));
		} catch (bad_alloc&) {
			
		}
		break;

	case SR_DF_ANALOG:
		try {
			feed_in_analog(dynamic_pointer_cast<Analog>(packet->payload()));
		} catch (bad_alloc&) {
			out_of_memory_ = true;
			device_->stop();
		}
		break;

	case SR_DF_FRAME_BEGIN:
		feed_in_frame_begin();
		break;

	case SR_DF_FRAME_END:
		feed_in_frame_end();
		break;

	case SR_DF_END:
		// Strictly speaking, this is performed when a frame end marker was
		// received, so there's no point doing this again. However, not all
		// devices use frames, and for those devices, we need to do it here.
		{
			lock_guard<recursive_mutex> lock(data_mutex_);

			if (cur_logic_segment_)
				cur_logic_segment_->set_complete();

			for (auto& entry : cur_analog_segments_) {
				shared_ptr<data::AnalogSegment> segment = entry.second;
				segment->set_complete();
			}

			cur_logic_segment_.reset();
			cur_analog_segments_.clear();
		}
		
		break;

	default:
		break;
	}
}

void Session::on_data_saved()
{
	data_saved_ = true;
}

#ifdef ENABLE_DECODE
void Session::on_new_decoders_selected(vector<const srd_decoder*> decoders)
{
	assert(decoders.size() > 0);

	shared_ptr<data::DecodeSignal> signal = add_decode_signal();

	if (signal)
		for (unsigned int i = 0; i < decoders.size(); i++) {
			const srd_decoder* d = decoders[i];
			signal->stack_decoder(d, false);  // Don't begin_decode yet; dialog will trigger it on OK
		}

	// Auto-open decoder configuration dialog
	if (signal) {
		qDebug() << "Session: creating DecoderChannelDialog";
		dialogs::DecoderChannelDialog dlg(*this, signal, decoders,
			QApplication::activeWindow());
		qDebug() << "Session: calling dlg.exec()";
		if (dlg.exec() == QDialog::Rejected) {
			qDebug() << "Session: dialog cancelled, removing decoder signal";
			remove_decode_signal(signal);
		}
		qDebug() << "Session: dlg.exec() returned, dialog about to be destroyed";
	}
}
#endif

} // namespace pv
