/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2012-2015 Joel Holdsworth <joel@airwebreathe.org.uk>
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
#define ENABLE_DECODE
#include <extdef.h>
#include <QApplication>
#include "pv/application.hpp"
#include <algorithm>
#include <cassert>

#include <QProgressDialog>
#include <QDir>
#include <QFileInfo>

#include <QAction>
#include <QDebug>
#include <QFileDialog>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QSettings>
#include <QToolTip>
#include <QString>
#include "mainbar.hpp"
#include <thread>
#include <boost/algorithm/string/join.hpp>
#include <QTime>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QEventLoop>
#include <QCoreApplication>
#include <pv/data/mathsignal.hpp>
#include <pv/devicemanager.hpp>
#include <pv/devices/hardwaredevice.hpp>
#include <pv/devices/inputfile.hpp>
#include <pv/devices/sessionfile.hpp>
#include <pv/dialogs/connect.hpp>
#include <pv/dialogs/inputoutputoptions.hpp>
#include <pv/dialogs/storeprogress.hpp>
#include <pv/globalsettings.hpp>
#include <pv/mainwindow.hpp>
#include <pv/popups/channels.hpp>
#include <pv/popups/deviceoptions.hpp>
#include <pv/util.hpp>
#include <pv/views/trace/view.hpp>
#include <pv/widgets/exportmenu.hpp>
#include <pv/widgets/importmenu.hpp>
#include "../dialogs/iapdialog.hpp"
#ifdef ENABLE_DECODE
#include <pv/data/decodesignal.hpp>
#include <QtGlobal>
#include <QScreen>

#endif

#include <libsigrokcxx/libsigrokcxx.hpp>

using std::back_inserter;
using std::copy;
using std::list;
using std::make_pair;
using std::make_shared;
using std::map;
using std::max;
using std::min;
using std::pair;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

using sigrok::Capability;
using sigrok::ConfigKey;
using sigrok::Error;
using sigrok::InputFormat;
using sigrok::OutputFormat;

using boost::algorithm::join;
#define WCH_VID 0x1a86
#define WCH_PID_CH569 0x8025
#define WCH_PID_CH32H417 0x5537
#define WCH_PID_CH32H417_HID 0xFE17
namespace pv
{
	namespace toolbars
	{
	MainBar *MainBar::s_mainbar_instance = nullptr;
		const uint64_t MainBar::MinSampleCount = 12500ULL;
		const uint64_t MainBar::MaxSampleCount = 1000000000000ULL;
		const uint64_t MainBar::DefaultSampleCount = 1000000;
		const uint64_t MainBar::MinSampleRate = 25000ULL;
		uint64_t MainBar::MaxSampleRate = 50000000ULL;
		const char *MainBar::SettingOpenDirectory = "MainWindow/OpenDirectory";
		const char *MainBar::SettingSaveDirectory = "MainWindow/SaveDirectory";

		MainBar::MainBar(Session &session, QWidget *parent, pv::views::trace::View *view) : StandardBar(session, parent, view, false),
																							channel_dlg(parent),
																							action_new_view_(new QAction(this)),
																							action_open_(new QAction(this)),
																							action_save_(new QAction(this)),
																							action_save_as_(new QAction(this)),
																							action_save_selection_as_(new QAction(this)),
																							action_restore_setup_(new QAction(this)),
																							action_save_setup_(new QAction(this)),
																							action_connect_(new QAction(this)),
																							run_stop_button_(new QToolButton()),
																							channels_button_ex_(new QToolButton()),
																							model_change_button_(new QToolButton()),
																							new_view_button_(new QToolButton()),
																							open_button_(new QToolButton()),
																							save_button_(new QToolButton()),
																							help_button_(new QToolButton()),
																							device_status_icon_(new QToolButton()),
																							device_selector_(parent, session.device_manager_ptr(), action_connect_),
																							// configure_button_(this),
																							// configure_button_action_(nullptr),
																							// channels_button_(this),
																							channels_button_(nullptr),
																							channels_button_action_(nullptr),
																							// sample_count_(" samples", this),
																							sample_count_("s", this),
																							sample_rate_("Hz", this),
																							updating_sample_rate_(false),
																							updating_sample_count_(false),
																							sample_count_supported_(false),
																							iap_upgrading_(false),
#ifdef ENABLE_DECODE
																							add_decoder_button_(new QToolButton())
#endif
		// add_math_signal_button_(new QToolButton())
		{
			setIconSize(QSize(32, 32));
			channel_number_ = 16;
			channels_ = nullptr;

			// 初始化设备状态图标
			device_status_icon_->setObjectName(QString::fromUtf8("device_status_icon"));
			device_status_icon_->setIcon(QIcon(":/icons/Demo.png"));
			device_status_icon_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			device_status_icon_->setText(tr("Demo"));
			device_status_icon_->setToolTip(tr("Device Status: Demo"));
			device_status_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
			setObjectName(QString::fromUtf8("MainBar"));

			setContextMenuPolicy(Qt::PreventContextMenu);

			setStyleSheet(
				"QToolBar {"
				"    spacing: 2px;"
				"    padding: 4px;"
				"    background: transparent;"
				"}"
				"QToolBar QToolButton {"
				"    margin: 0px;"
				"    padding: 2px;"
				"    border: 1px solid transparent;"
				"    border-radius: 2px;"
				"}"
				"QToolBar QToolButton:hover {"
				"    background: rgba(180, 180, 180, 60);"
				"    border: 1px solid rgba(120, 120, 120, 100);"
				"}"
				"QToolBar QToolButton:pressed {"
				"    background: rgba(150, 150, 150, 80);"
				"}"
				"QToolBar QToolButton#device_status_icon {"
				"    color: white;"
				"}"
				"QToolBar QToolButton#device_status_icon:disabled {"
				"    color: white;"
				"}"
				"QToolBar QToolButton[popupMode='1'] {"  // MenuButtonPopup
				"    padding-right: 8px;"
				"}"
				"QToolBar QToolButton[popupMode='2'] {"  // InstantPopup
				"    padding-right: 6px;"
				"}"
				"QToolBar QToolButton::menu-button {"
				"    width: 12px;"
				"    border: none;"
				"    background: transparent;"
				"}"
				"QToolBar QToolButton::menu-button:hover {"
				"    background: rgba(180, 180, 180, 60);"
				"    border: none;"
				"}"
				"QToolBar QToolButton::menu-arrow {"
				"    width: 8px;"
				"}"
				"QToolBar QToolButton::menu-arrow:hover {"
				"    background: rgba(180, 180, 180, 60);"
				"}"
			);

			// Actions
			action_new_view_->setText(tr("Decoding Result"));
			action_new_view_->setIcon(QIcon::fromTheme("window-new",
													   QIcon(":/icons/decode_result.png")));
			connect(action_new_view_, SIGNAL(triggered(bool)),
					this, SLOT(on_actionNewView_triggered()));

			action_open_->setText(tr("&Open..."));
			action_open_->setIcon(QIcon::fromTheme("document-open",
												   QIcon(":/icons/document-open.png")));
			action_open_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_O));
			connect(action_open_, SIGNAL(triggered(bool)),
					this, SLOT(on_actionOpen_triggered()));

			action_restore_setup_->setText(tr("Restore Session Setu&p..."));
			connect(action_restore_setup_, SIGNAL(triggered(bool)),
					this, SLOT(on_actionRestoreSetup_triggered()));

			action_save_->setText(tr("Save"));
			action_save_->setIcon(QIcon::fromTheme("document-save-as",
												   QIcon(":/icons/document-save-as.png")));
			action_save_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_S));
			connect(action_save_, SIGNAL(triggered(bool)),
					this, SLOT(on_actionSave_triggered()));

			action_save_as_->setText(tr("Save &As..."));
			action_save_as_->setIcon(QIcon::fromTheme("document-save-as",
													  QIcon(":/icons/document-save-as.png")));
			connect(action_save_as_, SIGNAL(triggered(bool)),
					this, SLOT(on_actionSaveAs_triggered()));

			action_save_selection_as_->setText(tr("Save Selected &Range As..."));
			action_save_selection_as_->setIcon(QIcon::fromTheme("document-save-as",
																QIcon(":/icons/document-save-as.png")));
			action_save_selection_as_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_R));
			connect(action_save_selection_as_, SIGNAL(triggered(bool)),
					this, SLOT(on_actionSaveSelectionAs_triggered()));

			action_save_setup_->setText(tr("Save Session Setu&p..."));
			connect(action_save_setup_, SIGNAL(triggered(bool)),
					this, SLOT(on_actionSaveSetup_triggered()));

			widgets::ExportMenu *menu_file_export = new widgets::ExportMenu(this,
																			session.device_manager().context());
			menu_file_export->setTitle(tr("&Export"));
			connect(menu_file_export, SIGNAL(format_selected(shared_ptr<sigrok::OutputFormat>)),
					this, SLOT(export_file(shared_ptr<sigrok::OutputFormat>)));

			widgets::ImportMenu *menu_file_import = new widgets::ImportMenu(this,
																			session.device_manager().context());
			menu_file_import->setTitle(tr("&Import"));
			connect(menu_file_import, SIGNAL(format_selected(shared_ptr<sigrok::InputFormat>)),
					this, SLOT(import_file(shared_ptr<sigrok::InputFormat>)));
			action_connect_->setText(tr("&Connect to Device..."));
			action_connect_->setToolTip(""); // 去除悬浮提示
			// connect(action_connect_, SIGNAL(triggered(bool)),
			// 	this, SLOT(on_actionConnect_triggered()));

			// New view button
			QMenu *menu_new_view = new QMenu();
			connect(menu_new_view, SIGNAL(triggered(QAction *)),
					this, SLOT(on_actionNewView_triggered(QAction *)));

			for (int i = 1; i < views::ViewTypeCount; i++)
			{
				QAction *const action = menu_new_view->addAction(tr(views::ViewTypeNames[i]));
				if (strcmp(views::ViewTypeNames[i], "Binary Decoder Output View") == 0)
					action->setText(tr("Binary Decoder Output View"));
				if (strcmp(views::ViewTypeNames[i], "Tabular Decoder Output View") == 0)
					action->setText(tr("Tabular Decoder Output View"));
				action->setData(QVariant::fromValue(i));
			}

			new_view_button_->setMenu(menu_new_view);
			new_view_button_->setDefaultAction(action_new_view_);
			new_view_button_->setPopupMode(QToolButton::MenuButtonPopup);
			new_view_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

			// model_ button
			QMenu *menu_model_view = new QMenu();
			QAction *actionSingle = menu_model_view->addAction(tr("single"));
			QAction *actionRepeat = menu_model_view->addAction(tr("repeat"));
			actionSingle->setIcon(QIcon(":/icons/Single.png"));
			actionRepeat->setIcon(QIcon(":/icons/Repeat.png"));

			connect(actionSingle, SIGNAL(triggered(bool)), this, SLOT(on_actionSingle_triggered()));
			connect(actionRepeat, SIGNAL(triggered(bool)), this, SLOT(on_actionRepeat_triggered()));

			// help button menu
			QMenu *menu_help_view = new QMenu();
			QAction *action_version_info = menu_help_view->addAction(tr("version info"));
			// QAction *action_feedback = menu_help_view->addAction(tr("feedback"));
			QAction *action_update = menu_help_view->addAction(tr("Update"));
			QAction *action_use_guide = menu_help_view->addAction(tr("User Guide"));
			// QAction *action_about = menu_help_view->addAction(tr("About"));
			connect(action_version_info, SIGNAL(triggered(bool)), this, SLOT(on_action_version_info_triggered()));
			// connect(action_feedback, SIGNAL(triggered(bool)), this, SLOT(on_action_feedback_triggered()));
			connect(action_update, SIGNAL(triggered(bool)), this, SLOT(on_action_update_triggered()));
			connect(action_use_guide, SIGNAL(triggered(bool)), this, SLOT(on_action_use_guide_triggered()));
			// connect(action_about, SIGNAL(triggered(bool)), this, SLOT(on_action_about_triggered()));
			// Open button
			vector<QAction *> open_actions;
			open_actions.push_back(action_open_);
			QAction *separator_o = new QAction(this);
			separator_o->setSeparator(true);
			open_actions.push_back(separator_o);
			open_actions.push_back(action_restore_setup_);

			// help button
			help_button_->setMenu(menu_help_view);
			help_button_->setIcon(QIcon(":/icons/Help.png"));
			help_button_->setPopupMode(QToolButton::MenuButtonPopup);
			help_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			help_button_->setText(tr("Help"));
			connect(help_button_, &QToolButton::clicked, help_button_, &QToolButton::showMenu);

			widgets::ImportMenu *import_menu = new widgets::ImportMenu(this,
																	   session.device_manager().context(), open_actions);
			connect(import_menu, SIGNAL(format_selected(shared_ptr<sigrok::InputFormat>)),
					this, SLOT(import_file(shared_ptr<sigrok::InputFormat>)));

			// run stop button
			run_stop_button_->setIcon(QIcon(":/icons/Start.png"));
			run_stop_button_->setShortcut(QKeySequence(Qt::Key_S));
			run_stop_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			run_stop_button_->setText(tr("Start"));
			connect(run_stop_button_, SIGNAL(clicked()), this, SIGNAL(run_stop_button_clicked()));
			// model change button
			model_change_button_->setMenu(menu_model_view);
			model_change_button_->setIcon(QIcon(":/icons/Single.png")); // default single model
			// model_change_button_->setDefaultAction(actionSingle);
			model_change_button_->setPopupMode(QToolButton::MenuButtonPopup);
			model_change_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			model_change_button_->setText(tr("Model"));
			connect(model_change_button_, &QToolButton::clicked, model_change_button_, &QToolButton::showMenu);
			// open button
			open_button_->setMenu(import_menu);
			open_button_->setDefaultAction(action_open_);
			open_button_->setPopupMode(QToolButton::MenuButtonPopup);
			open_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			open_button_->setText(tr("Open"));
			// Save button
			vector<QAction *> save_actions;
			save_actions.push_back(action_save_);
			save_actions.push_back(action_save_as_);
			save_actions.push_back(action_save_selection_as_);
			QAction *separator_s = new QAction(this);
			separator_s->setSeparator(true);
			save_actions.push_back(separator_s);
			save_actions.push_back(action_save_setup_);

			widgets::ExportMenu *export_menu = new widgets::ExportMenu(this,
																	   session.device_manager().context(), save_actions);
			connect(export_menu, SIGNAL(format_selected(shared_ptr<sigrok::OutputFormat>)),
					this, SLOT(export_file(shared_ptr<sigrok::OutputFormat>)));

			save_button_->setMenu(export_menu);
			save_button_->setDefaultAction(action_save_);
			save_button_->setPopupMode(QToolButton::MenuButtonPopup);
			save_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			save_button_->setText(tr("Save"));
			// Device selector menu
			connect(&device_selector_, SIGNAL(device_selected()),
					this, SLOT(on_device_selected()));

			// Setup the decoder button
#ifdef ENABLE_DECODE
			add_decoder_button_->setIcon(QIcon(":/icons/add-decoder.png"));
			add_decoder_button_->setPopupMode(QToolButton::InstantPopup);
			add_decoder_button_->setToolTip(tr("Add protocol decoder"));
			add_decoder_button_->setShortcut(QKeySequence(Qt::Key_D));
			add_decoder_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			add_decoder_button_->setText(tr("Decoder"));

			connect(add_decoder_button_, SIGNAL(clicked()),
					this, SLOT(on_add_decoder_clicked()));
#endif

			// Setup the math signal button
			// add_math_signal_button_->setIcon(QIcon(":/icons/add-math-signal.svg"));
			// add_math_signal_button_->setPopupMode(QToolButton::InstantPopup);
			// add_math_signal_button_->setToolTip(tr("Add math signal"));
			// add_math_signal_button_->setShortcut(QKeySequence(Qt::Key_M));

			// connect(add_math_signal_button_, SIGNAL(clicked()),
			// 	this, SLOT(on_add_math_signal_clicked()));

			connect(&sample_count_, SIGNAL(value_changed()),
					this, SLOT(on_sample_count_changed()));
			connect(&sample_rate_, SIGNAL(value_changed()),
					this, SLOT(on_sample_rate_changed()));

			sample_count_.show_min_max_step(0, UINT64_MAX, 1);

			set_capture_state(pv::Session::Stopped);

			// configure_button_.setToolTip(tr("Configure Device"));
			// configure_button_.setIcon(QIcon::fromTheme("preferences-system",
			// 	QIcon(":/icons/preferences-system.png")));

			channels_button_.setToolTip(tr("Configure Channels"));
			channels_button_.setIcon(QIcon(":/icons/channels.svg"));
			// channels_button_.set_text_style(tr("Option"), Qt::ToolButtonTextUnderIcon);
			channels_button_.setToolButtonStyle((Qt::ToolButtonStyle)Qt::ToolButtonTextUnderIcon);
			channels_button_.setText(tr("Option"));

			// run_stop_button_->setIcon(QIcon(":/icons/add-decoder.svg"));
			// 	run_stop_button_->setShortcut(QKeySequence(Qt::Key_S));
			// 	run_stop_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			// 	run_stop_button_->setText(tr("Start"));
			// connect(run_stop_button_, SIGNAL(clicked()), this, SIGNAL(run_stop_button_clicked()));
			channels_button_ex_->setIcon(QIcon(":/icons/channels.png"));
			channels_button_ex_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			channels_button_ex_->setToolButtonStyle((Qt::ToolButtonStyle)Qt::ToolButtonTextUnderIcon);
			channels_button_ex_->setText(tr("Option"));
			connect(channels_button_ex_, SIGNAL(clicked()), this, SLOT(channels_button_ex_clicked()));
			add_toolbar_widgets();

			sample_count_.installEventFilter(this);
			sample_rate_.installEventFilter(this);

			// Setup session_ events
			connect(&session_, SIGNAL(capture_state_changed(int)),
					this, SLOT(on_capture_state_changed(int)));
			connect(&session, SIGNAL(device_changed()),
					this, SLOT(on_device_changed()));

			/* CH32H417设备事件信号槽连接 */
			connect(this, SIGNAL(ch32h417_device_event_signal(unsigned long)),
					this, SLOT(on_ch32h417_device_event(unsigned long)));

			update_device_list();
			is_rate_setting_default_ = false;
			is_count_setting_default_ = false;
			update_device_status_icon();
		}

		MainBar::~MainBar()
		{
			s_mainbar_instance = nullptr;

#ifdef _WIN32
			ch375_set_device_notify(0, NULL, NULL);
#endif

			if (channels_)
			{
				delete channels_;
				channels_ = nullptr;
			}

			/* 停止热插拔线程 */
			if (t_hot_plug.joinable())
			{
				hot_plug_listen_ = false;
				t_hot_plug.join();
			}

			/* 清理CH375DLL资源 */
#ifdef _WIN32
			ch375_cleanup();
#endif
			commit_channel_number(0);
		}

		void MainBar::set_vendorName(QString vendorName)
		{
			std::lock_guard<std::mutex> lock(vendorName_mutex);
			m_vendorName = vendorName;
		}

		QString MainBar::get_vendorName()
		{
			std::lock_guard<std::mutex> lock(vendorName_mutex);
			return m_vendorName;
		}

		uint16_t MainBar::get_channel_number() const
		{
			return channel_number_;
		}

		void MainBar::update_device_list()
		{
			device_selector_.update_device_mamager(session_.device_manager_ptr());
			DeviceManager &mgr = session_.device_manager();
			shared_ptr<devices::Device> selected_device = session_.device();
			list<shared_ptr<devices::Device>> devs;
			copy(mgr.devices().begin(), mgr.devices().end(), back_inserter(devs));
			if (std::find(devs.begin(), devs.end(), selected_device) == devs.end())
				devs.push_back(selected_device);
			device_selector_.set_device_list(devs, selected_device);
			update_device_config_widgets();
		}

		void MainBar::set_capture_state(pv::Session::capture_state state)
		{
			bool ui_enabled = (state == pv::Session::Stopped) ? true : false;

			device_selector_.setEnabled(ui_enabled);
			// configure_button_.setEnabled(ui_enabled);
			// channels_button_.setEnabled(ui_enabled);
			channels_button_ex_->setEnabled(ui_enabled);
			// Check if demo device - sample rate/count should stay disabled
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			shared_ptr<devices::HardwareDevice> hw_device =
				std::dynamic_pointer_cast<devices::HardwareDevice>(device);
			bool is_demo = false;
			if (hw_device) {
				string driver_name = hw_device->hardware_device()->driver()->name();
				is_demo = (driver_name == "demo");
			}
			if (is_demo) {
				sample_count_.setEnabled(false);
				sample_rate_.setEnabled(false);
			} else {
				sample_count_.setEnabled(ui_enabled);
				sample_rate_.setEnabled(ui_enabled);
			}
			if (model_change_button_)
				model_change_button_->setEnabled(ui_enabled);
		}

		void MainBar::reset_device_selector()
		{
			device_selector_.reset();
		}

		void MainBar::set_device_selector_name(QString device_selector)
		{
			device_selector_.set_device_list_name(device_selector);
		}

		QAction *MainBar::action_open() const
		{
			return action_open_;
		}

		QAction *MainBar::action_save() const
		{
			return action_save_;
		}

		QAction *MainBar::action_save_as() const
		{
			return action_save_as_;
		}

		QAction *MainBar::action_save_selection_as() const
		{
			return action_save_selection_as_;
		}

		QAction *MainBar::action_connect() const
		{
			return action_connect_;
		}

		void MainBar::update_sample_rate_selector()
		{
			Glib::VariantContainerBase gvar_dict;
			GVariant *gvar_list;
			const uint64_t *elements = nullptr;
			gsize num_elements;
			map<const ConfigKey *, set<Capability>> keys;
			if (updating_sample_rate_)
			{
				sample_rate_.show_none();
				return;
			}

			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;

			assert(!updating_sample_rate_);
			updating_sample_rate_ = true;

			const shared_ptr<sigrok::Device> sr_dev = device->device();

			sample_rate_.allow_user_entered_values(false);
			if (sr_dev->config_check(ConfigKey::EXTERNAL_CLOCK, Capability::GET))
			{
				try
				{
					auto gvar = sr_dev->config_get(ConfigKey::EXTERNAL_CLOCK);
					if (gvar.gobj())
					{
						bool value = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(
										 gvar)
										 .get();
						sample_rate_.allow_user_entered_values(value);
					}
				}
				catch (Error &error)
				{
					// Do nothing
				}
			}

			if (sr_dev->config_check(ConfigKey::SAMPLERATE, Capability::LIST))
			{
				try
				{
					gvar_dict = sr_dev->config_list(ConfigKey::SAMPLERATE);
				}
				catch (Error &error)
				{
					qDebug() << tr("Failed to get sample rate list:") << error.what();
				}
			}
			else
			{
				sample_rate_.show_none();
				updating_sample_rate_ = false;
				return;
			}
			if ((gvar_list = g_variant_lookup_value(gvar_dict.gobj(),
													"samplerate-steps", G_VARIANT_TYPE("at"))))
			{
				elements = (const uint64_t *)g_variant_get_fixed_array(
					gvar_list, &num_elements, sizeof(uint64_t));

				uint64_t minRate = elements[0];
				uint64_t maxRate = elements[1];
				uint64_t stepRate = elements[2];

				g_variant_unref(gvar_list);

				assert(minRate > 0);
				assert(maxRate > 0);
				assert(maxRate > minRate);
				assert(stepRate > 0);

				minRate = max(minRate, MinSampleRate);
				maxRate = min(maxRate, MaxSampleRate);
				if (stepRate == 1)
					sample_rate_.show_125_list(minRate, maxRate);
				else
				{
					// When the step is not 1, we cam't make a 1-2-5-10
					// list of sample rates, because we may not be able to
					// make round numbers. Therefore in this case, show a
					// spin box.
					sample_rate_.show_min_max_step(minRate, maxRate, stepRate);
				}
			}
			else if ((gvar_list = g_variant_lookup_value(gvar_dict.gobj(),
														 "samplerates", G_VARIANT_TYPE("at"))))
			{
				elements = (const uint64_t *)g_variant_get_fixed_array(
					gvar_list, &num_elements, sizeof(uint64_t));
				QString vendor = QString::fromStdString(sr_dev->vendor());
				QString model = QString::fromStdString(sr_dev->model());
				if (vendor == "USB3.0(CH569)")
				{
					// CH569: 采样率数量随通道数变化
					switch (channel_number_)
					{
					case 2:
						num_elements = 18;
						break;
					case 4:
						num_elements = 17;
						break;
					case 8:
						num_elements = 16;
						break;
					default:
						num_elements = 15;
						break;
					}
				}
				else if (model == "USB3.0(CH32H417)")
				{
					// CH32H417: 固定16个采样率，不随通道数变化
					num_elements = 16;
				}
				sample_rate_.show_list(elements, num_elements);
				// if (is_rate_setting_default_){
				// 	sample_rate_.set_current_index(sample_rate_default_index_);
				// }
				g_variant_unref(gvar_list);
			}
			updating_sample_rate_ = false;
			commit_sample_rate();
			update_sample_rate_selector_value();
		}

		void MainBar::update_sample_rate_selector_value()
		{
			if (updating_sample_rate_)
				return;

			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;

			try
			{
				auto gvar = device->device()->config_get(ConfigKey::SAMPLERATE);
				uint64_t samplerate =
					Glib::VariantBase::cast_dynamic<Glib::Variant<guint64>>(gvar).get();
				assert(!updating_sample_rate_);
				updating_sample_rate_ = true;
				sample_rate_.set_value(samplerate);
				updating_sample_rate_ = false;
			}
			catch (Error &error)
			{
				// Do nothing
			}
		}

		void MainBar::update_sample_count_selector()
		{
			if (updating_sample_count_)
				return;

			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;

			const shared_ptr<sigrok::Device> sr_dev = device->device();
			if (sr_dev != NULL)
			{
			}
			else
			{
				return;
			}
			assert(!updating_sample_count_);
			updating_sample_count_ = true;

			if (!sample_count_supported_)
			{
				sample_count_.show_none();
				updating_sample_count_ = false;
				return;
			}

			// static uint64_t sample_count = ((double)sample_count_.get_current_data(sample_count_.get_current_index())) / 10000.00 * (double)sample_rate_.get_current_data(sample_rate_.get_current_index());
			static uint64_t sample_count = 0;
			uint64_t min_sample_count = 0;
			uint64_t max_sample_count = MaxSampleCount;
			bool default_count_set = false;

			if (sample_count == 0)
			{
				sample_count = DefaultSampleCount;
				default_count_set = true;
			}

			if (sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::LIST))
			{
				try
				{
					auto gvar = sr_dev->config_list(ConfigKey::LIMIT_SAMPLES);
					if (gvar.gobj())
						g_variant_get(gvar.gobj(), "(tt)",
									  &min_sample_count, &max_sample_count);
				}
				catch (Error &error)
				{
					qDebug() << tr("Failed to get sample limit list:") << error.what();
				}
			}
			min_sample_count = min(max(min_sample_count, MinSampleCount),
								   max_sample_count);

			// Max data : 18,000,000,000字节   Min data : 100字节
			// max_time = 18000000000 / sample_rate_.get_current_data(sample_rate_.get_current_index())
			// min_time = 0.0001S = 100us
			uint64_t sampleRate = sample_rate_.get_current_data(sample_rate_.get_current_index());
			if (sampleRate < 1ULL || sampleRate > 1000005000ULL)
			{
				sampleRate = 25000;
			}
			double max_time = 18000000000.00 / ((double)sampleRate * (double)(channel_number_ / 8.00));
			shared_ptr<devices::HardwareDevice> hw_dev_for_min_time =
				std::dynamic_pointer_cast<devices::HardwareDevice>(device);
			bool is_demo_min_time = false;
			if (hw_dev_for_min_time) {
				string driver_name = hw_dev_for_min_time->hardware_device()->driver()->name();
				is_demo_min_time = (driver_name == "demo");
			}
			double min_time = is_demo_min_time ? 0.002 : 0.5;
			sample_count_.show_125_list(min_time * 10000, max_time * 10000); // commit sample count

			if (sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::GET))
			{
				auto gvar = sr_dev->config_get(ConfigKey::LIMIT_SAMPLES);
				sample_count = g_variant_get_uint64(gvar.gobj());
			}
			if (sample_count > 0)
			{
				sample_count_.set_time_value(sample_count, sampleRate);
				if (sample_count != ((double)sample_count_.get_current_data(sample_count_.get_current_index())) / 10000.00 * (double)sample_rate_.get_current_data(sample_rate_.get_current_index()))
				{
					commit_sample_count();
				}
			}
			updating_sample_count_ = false;

			// If we show the default rate then make sure the device uses the same
			if (default_count_set)
			{
				commit_sample_count();
			}
		}

		void MainBar::on_update_max_sample_rate(uint64_t maxSampleRate)
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;

			QString vendor = QString::fromStdString(device->device()->vendor());
			QString model = QString::fromStdString(device->device()->model());
			bool is_ch32h417 = (vendor == "USB3.0(CH32H417)" || model == "USB3.0(CH32H417)");

			// CH32H417处理
			if (is_ch32h417)
			{
				bool is_usb2 = channels_ ? channels_->is_usb2() : false;
				uint16_t channel_number = 16;
				uint64_t sample_rate_channel_max = 0;

				if (is_usb2)
				{
					// CH32H417 USB2.0: 2/4/8通道最高40MHz，16通道最高20MHz
					if (maxSampleRate == SR_MHZ(40))
					{
						channel_number = 8;
						sample_rate_channel_max = SR_MHZ(40);
					}
					else if (maxSampleRate == SR_MHZ(20))
					{
						channel_number = 16;
						sample_rate_channel_max = SR_MHZ(20);
					}
				}
				else
				{
					// CH32H417 USB3.0: 采样率固定200MHz
					channel_number = 16;
					sample_rate_channel_max = SR_MHZ(200);
				}

				update_sample_rate_selector();
				update_sample_count_selector();
				return;
			}

			// CH569和其他设备的处理
			uint16_t channel_number = 16;
			uint64_t sample_rate_channel_max = 0;
			MaxSampleRate = maxSampleRate;
			if (MaxSampleRate == SR_GHZ(1))
			{
				channel_number = 2;
			}
			else if (MaxSampleRate == SR_MHZ(500))
			{
				channel_number = 4;
			}
			else if (MaxSampleRate == SR_MHZ(250))
			{
				channel_number = 8;
			}
			else
			{
				channel_number = 16;
			}
			if (channel_number == channel_number_)
			{
				return;
			}
			// commit channel number
			commit_channel_number(channel_number);
			channel_number_ = channel_number;
			switch (channel_number)
			{
			case 2:
				sample_rate_channel_max = SR_GHZ(1);
				break;
			case 4:
				sample_rate_channel_max = SR_MHZ(500);
				break;
			case 8:
				sample_rate_channel_max = SR_MHZ(250);
				break;
			default:
				sample_rate_channel_max = SR_MHZ(125);
				break;
			}

			if (sample_rate_value_ > sample_rate_channel_max)
			{
				update_sample_rate_selector();
				update_sample_count_selector();
			}
			else
			{
				renew_samplecount_samplerate(sample_rate_value_, sample_rate_channel_max);
			}
		}

		void MainBar::on_update_channel_count(uint16_t channelCount)
		{
			// CH32H417专用：处理通道数变化
			// 只接受8或16通道
			if (channelCount != 8 && channelCount != 16)
			{
				return;
			}

			if (channelCount == channel_number_)
			{
				return;
			}

			commit_channel_number(channelCount);
			channel_number_ = channelCount;

			update_sample_count_selector();
		}

		void MainBar::on_update_threshold_value_1(float thresholdValue)
		{
			commit_threshold_value(1, thresholdValue);
			thresholdValue_1_ = thresholdValue;
		}

		void MainBar::on_update_threshold_value_2(float thresholdValue)
		{
			commit_threshold_value(2, thresholdValue);
			thresholdValue_2_ = thresholdValue;
		}

		void MainBar::on_update_threshold_value_ch32h417(float thresholdValue)
		{
			// 只保存阈值，不发送到硬件（硬件发送由 on_threshold_value_raw_changed 处理）
			thresholdValue_ch32h417_ = thresholdValue;
		}

		void MainBar::on_mode_changed(int mode)
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;

			const shared_ptr<sigrok::Device> sr_dev = device->device();

			// 模式名称：0=Logic Analyzer, 1=ADC
			QString mode_str = (mode == 0) ? "Logic Analyzer" : "ADC";

			try
			{
				sr_dev->config_set(ConfigKey::PATTERN_MODE,
								   Glib::Variant<Glib::ustring>::create(mode_str.toStdString()));

				qDebug() << "Device mode set to:" << mode_str;

				// 模式切换后更新采样率列表
				update_sample_rate_selector();
				update_sample_rate_selector_value();
			}
			catch (Error &error)
			{
				qDebug() << tr("Failed to configure pattern mode:") << error.what();
			}
		}

		void MainBar::on_adc_channel_changed(int channel)
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;

			const shared_ptr<sigrok::Device> sr_dev = device->device();

			// ADC通道：0=Channel 0, 1=Channel 1
			try
			{
				sr_dev->config_set(ConfigKey::ADC_CHANNEL,
								   Glib::Variant<guint64>::create((uint64_t)channel));
				qDebug() << "ADC channel set to:" << channel;
			}
			catch (Error &error)
			{
				qDebug() << tr("Failed to configure ADC channel:") << error.what();
			}
		}

		void MainBar::on_adc_precision_changed(int precision)
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;

			const shared_ptr<sigrok::Device> sr_dev = device->device();

			// ADC精度：0=8-bit, 1=10-bit
			uint64_t precision_value = (precision == 0) ? 8 : 10;

			try
			{
				sr_dev->config_set(ConfigKey::ADC_PRECISION,
								   Glib::Variant<guint64>::create(precision_value));
				qDebug() << "ADC precision set to:" << precision_value << "bit";
			}
			catch (Error &error)
			{
				qDebug() << tr("Failed to configure ADC precision:") << error.what();
			}
		}

		void MainBar::on_threshold_value_raw_changed(uint16_t value)
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;
			const shared_ptr<sigrok::Device> sr_dev = device->device();
			try
			{
				sr_dev->config_set(ConfigKey::THRESHOLD_VALUE_1,
								   Glib::Variant<guint64>::create((uint64_t)value));
				qDebug() << "THRESHOLD_VALUE_1 set to:" << value;
			}
			catch (Error &error)
			{
				qDebug() << "Failed to set THRESHOLD_VALUE_1:" << error.what();
			}
		}

		void MainBar::on_channel_selection_changed()
		{
			if (view_) {
				view_->update_signals_height();
			}
		}

		void MainBar::update_device_status_icon()
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device) {
				device_status_icon_->setIcon(QIcon(":/icons/Demo.png"));
				device_status_icon_->setText(tr("Demo"));
				device_status_icon_->setToolTip(tr("Device Status: Demo"));
				return;
			}

			if (session_.using_file_device()) {
				device_status_icon_->setIcon(QIcon(":/icons/File.png"));
				device_status_icon_->setText(tr("File"));
				device_status_icon_->setToolTip(tr("Device Status: File"));
				return;
			}

			shared_ptr<devices::HardwareDevice> hw_device =
				std::dynamic_pointer_cast<devices::HardwareDevice>(device);

			if (hw_device) {
				string driver_name = hw_device->hardware_device()->driver()->name();
				if (driver_name == "demo") {
					device_status_icon_->setIcon(QIcon(":/icons/Demo.png"));
					device_status_icon_->setText(tr("Demo"));
					device_status_icon_->setToolTip(tr("Device Status: Demo"));
					return;
				}
			}

			const shared_ptr<sigrok::Device> sr_dev = device->device();
			QString vendor = QString::fromStdString(sr_dev->vendor());

			if (vendor == "USB3.0(CH32H417)") {
				bool is_usb2 = false;
				if (sr_dev->config_check(ConfigKey::USB_VERSION, Capability::GET)) {
					try {
						auto gvar = sr_dev->config_get(ConfigKey::USB_VERSION);
						if (gvar.gobj()) {
							uint64_t usb_version = Glib::VariantBase::cast_dynamic<Glib::Variant<guint64>>(gvar).get();
							is_usb2 = (usb_version == 2);
						}
					} catch (Error &error) {
						// 默认USB3.0
					}
				}

				if (is_usb2) {
					device_status_icon_->setIcon(QIcon(":/icons/USB20.png"));
					device_status_icon_->setText(tr("USB2.0"));
					device_status_icon_->setToolTip(tr("Device Status: USB 2.0"));
				} else {
					device_status_icon_->setIcon(QIcon(":/icons/USB30.png"));
					device_status_icon_->setText(tr("USB3.0"));
					device_status_icon_->setToolTip(tr("Device Status: USB 3.0"));
				}
			} else if (vendor == "USB3.0(CH569)") {
				device_status_icon_->setIcon(QIcon(":/icons/USB30.png"));
				device_status_icon_->setText(tr("USB3.0"));
				device_status_icon_->setToolTip(tr("Device Status: USB 3.0"));
			} else {
				device_status_icon_->setIcon(QIcon(":/icons/Demo.png"));
				device_status_icon_->setText(tr("Demo"));
				device_status_icon_->setToolTip(tr("Device Status: Demo"));
			}
		}

		void MainBar::renew_samplecount_samplerate(uint64_t samplerate, uint64_t samplerate_max)
		{
			uint16_t sampelrate_index = sample_rate_.get_current_index();
			uint16_t samplecout_index = sample_count_.get_current_index();
			const uint64_t *elements = nullptr;
			Glib::VariantContainerBase gvar_dict;
			GVariant *gvar_list;
			gsize num_elements;
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;
			const shared_ptr<sigrok::Device> sr_dev = device->device();
			if (sr_dev->config_check(ConfigKey::SAMPLERATE, Capability::LIST))
			{
				try
				{
					gvar_dict = sr_dev->config_list(ConfigKey::SAMPLERATE);
				}
				catch (Error &error)
				{
					qDebug() << tr("Failed to get sample rate list:") << error.what();
				}
			}
			else
			{
				sample_rate_.show_none();
				updating_sample_rate_ = false;
				return;
			}
			if ((gvar_list = g_variant_lookup_value(gvar_dict.gobj(),
													"samplerates", G_VARIANT_TYPE("at"))))
			{
				elements = (const uint64_t *)g_variant_get_fixed_array(
					gvar_list, &num_elements, sizeof(uint64_t));
				switch (channel_number_)
				{
				case 2:
					num_elements = 18;
					break;
				case 4:
					num_elements = 17;
					break;
				case 8:
					num_elements = 16;
					break;
				default:
					num_elements = 15;
					break;
				}
				sample_rate_.show_list(elements, num_elements);
				sample_rate_.set_current_index(sampelrate_index);
				sample_count_.set_current_index(samplecout_index);
			}
		}

		void MainBar::set_setting_default()
		{
			QSettings settings;
			// 写入配置信息
			settings.beginGroup("Settings");

			QString vendorName = get_vendorName();
			if (vendorName == "USB3.0(CH32H417)")
			{
				// CH32H417使用GlobalSettings的ThresholdLevel键值
				GlobalSettings gs;
				gs.setValue(GlobalSettings::Key_ThresholdLevel, thresholdValue_ch32h417_);
			}
			else
			{
				// CH569使用原有的两路阈值设置
				settings.setValue("Threshold_bank_1", QString::number(thresholdValue_1_));
				settings.setValue("Threshold_bank_2", QString::number(thresholdValue_2_));
			}

			settings.setValue("sample_count_index", QString::number(sample_count_.get_current_index()));
			settings.setValue("sample_rate_index", QString::number(sample_rate_.get_current_index()));
			settings.setValue("channel_number", QString::number(channel_number_));
			settings.endGroup();
		}

		void MainBar::get_decoder_list_default(QString decoder_name)
		{
			string String = decoder_name.toLocal8Bit().toStdString();
			istringstream iss(String);
			std::string word;
			decoder_list_default_.clear();
			while (iss >> word)
			{
				decoder_list_default_ << QString::fromLocal8Bit(word.data());
			}
		}

		void MainBar::save_setup(QSettings &settings) const
		{
			// Save sample count and sample rate values
			settings.setValue("sample_count", QVariant::fromValue(sample_count_value_));
			settings.setValue("sample_rate", QVariant::fromValue(sample_rate_value_));
		}

		void MainBar::restore_setup(QSettings &settings)
		{
			// Restore sample count and sample rate values
			if (settings.contains("sample_count")) {
				sample_count_value_ = settings.value("sample_count").value<uint64_t>();
			}
			if (settings.contains("sample_rate")) {
				sample_rate_value_ = settings.value("sample_rate").value<uint64_t>();
			}
		}

		void MainBar::get_setting_default()
		{
			QSettings settings;
			if (settings.contains("Settings/sample_count_index"))
			{
				// CH569阈值设置
				threshold_bank_1_default_ = settings.value("Settings/Threshold_bank_1").toFloat();
				threshold_bank_2_default_ = settings.value("Settings/Threshold_bank_2").toFloat();
				channel_number_default_ = settings.value("Settings/channel_number").toInt();
				sample_count_default_index_ = settings.value("Settings/sample_count_index").toUInt();
				sample_rate_default_index_ = settings.value("Settings/sample_rate_index").toUInt();
				is_repeat_acq_default_ = settings.value("Settings/repeat_acquisition").toInt();
				is_count_setting_default_ = true;
				is_rate_setting_default_ = true;
			}
			else
			{
				is_count_setting_default_ = false;
				is_rate_setting_default_ = false;
				return;
			}
		}

		void MainBar::update_device_config_widgets()
		{
			// get default settings
			get_setting_default();
			using namespace pv::popups;

			const shared_ptr<devices::Device> device = device_selector_.selected_device();

			// Hide the widgets if no device is selected
			// channels_button_action_->setVisible(!!device);
			if (!device)
			{
				// configure_button_action_->setVisible(false);
				sample_count_.show_none();
				sample_rate_.show_none();
				return;
			}

			const shared_ptr<sigrok::Device> sr_dev = device->device();
			if (!sr_dev)
				return;

			// Check if this is a demo device
			shared_ptr<devices::HardwareDevice> hw_device =
				std::dynamic_pointer_cast<devices::HardwareDevice>(device);
			bool is_demo_device = false;
			if (hw_device) {
				string driver_name = hw_device->hardware_device()->driver()->name();
				if (driver_name == "demo") {
					is_demo_device = true;
				}
			}

			// Disable sample rate and sample count selectors for demo device
			if (is_demo_device) {
				sample_count_.setEnabled(false);
				sample_rate_.setEnabled(false);
			} else {
				sample_count_.setEnabled(true);
				sample_rate_.setEnabled(true);
			}

			// Update the configure popup
			DeviceOptions *const opts = new DeviceOptions(sr_dev, this);
			// configure_button_action_->setVisible(!opts->binding().properties().empty());
			// configure_button_.set_popup(opts);

			// Update the channels popup
			if (channels_)
			{
				delete channels_;
				channels_ = nullptr;
			}
			channels_ = new Channels(session_, QString::fromStdString(sr_dev->vendor()), this);
			set_vendorName(QString::fromStdString(sr_dev->vendor()));

			// 获取USB版本并设置
			if (sr_dev->config_check(ConfigKey::USB_VERSION, Capability::GET))
			{
				try
				{
					auto gvar = sr_dev->config_get(ConfigKey::USB_VERSION);
					if (gvar.gobj())
					{
						uint64_t usb_version = Glib::VariantBase::cast_dynamic<Glib::Variant<guint64>>(gvar).get();
						channels_->set_usb2_mode(usb_version == 2);
					}
				}
				catch (Error &error)
				{
					qDebug() << "Failed to get USB version:" << error.what();
				}
			}
			connect(channels_, SIGNAL(max_sample_rate_change(uint64_t)), this, SLOT(on_update_max_sample_rate(uint64_t)));
			connect(channels_, SIGNAL(channel_count_change(uint16_t)), this, SLOT(on_update_channel_count(uint16_t)));
			connect(channels_, SIGNAL(threshold_value_change_1(float)), this, SLOT(on_update_threshold_value_1(float)));
			connect(channels_, SIGNAL(threshold_value_change_2(float)), this, SLOT(on_update_threshold_value_2(float)));
			connect(channels_, SIGNAL(threshold_value_change_ch32h417(float)), this, SLOT(on_update_threshold_value_ch32h417(float)));
			connect(channels_, SIGNAL(mode_changed(int)), this, SLOT(on_mode_changed(int)));
			connect(channels_, SIGNAL(adc_channel_changed(int)), this, SLOT(on_adc_channel_changed(int)));
			connect(channels_, SIGNAL(adc_precision_changed(int)), this, SLOT(on_adc_precision_changed(int)));
			connect(channels_, SIGNAL(threshold_value_raw_changed(uint16_t)), this, SLOT(on_threshold_value_raw_changed(uint16_t)));
			connect(channels_, SIGNAL(channel_selection_changed()), this, SLOT(on_channel_selection_changed()));
			// end
			//  channels_button_.set_popup(channels_);
			//  Update supported options.
			sample_count_supported_ = false;

			if (sr_dev->config_check(ConfigKey::LIMIT_SAMPLES, Capability::SET))
				sample_count_supported_ = true;

			// Add notification of reconfigure events
			// Note: No need to disconnect the previous signal as that QObject instance is destroyed
			connect(&opts->binding(), SIGNAL(config_changed()),
					this, SLOT(on_config_changed()));

			// Update sweep timing widgets.
			update_sample_count_selector();
			update_sample_rate_selector();
		}

		void MainBar::renew_default_bank_threshold()
		{
			// channels_->set_default_bank_threshold(threshold_bank_1_default_, threshold_bank_2_default_);
		}

		void MainBar::renew_setting_default()
		{
			// restore settings
			QString vendorName = get_vendorName();
			// 支持 CH569 和 CH32H417 两种设备
			if (vendorName != "USB3.0(CH569)" && vendorName != "USB3.0(CH32H417)")
				return;

			// CH32H417使用GlobalSettings的ThresholdLevel键值
			if (vendorName == "USB3.0(CH32H417)")
			{
				GlobalSettings gs;
				if (gs.contains(GlobalSettings::Key_ThresholdLevel))
				{
					threshold_ch32h417_default_ = gs.value(GlobalSettings::Key_ThresholdLevel).toFloat();
				}
				else
				{
					// CH32H417硬件默认阈值是2.3V
					threshold_ch32h417_default_ = 2.3f;
				}

				// 关闭CH32H417逻辑分析仪模式下的模拟通道A0/A1
				for (const auto &sig : session_.signalbases())
				{
					if (sig->type() == data::SignalBase::AnalogChannel)
					{
						sig->set_enabled(false);
					}
				}
				channels_->set_default_bank_threshold(threshold_ch32h417_default_, threshold_bank_2_default_);
			}
			else
			{
				channels_->set_default_bank_threshold(threshold_bank_1_default_, threshold_bank_2_default_);
			}

			if (channels_)
			{
				switch (channel_number_default_)
				{
				case 2:
					if (channels_->radioChannels[0])
						channels_->radioChannels[0]->click();
					break;
				case 4:
					if (channels_->radioChannels[1])
						channels_->radioChannels[1]->click();
					break;
				case 8:
					if (channels_->radioChannels[2])
						channels_->radioChannels[2]->click();
					break;
				case 16:
					if (channels_->radioChannels[3])
						channels_->radioChannels[3]->click();
					break;
				default:
					break;
				}
			}
			if (is_rate_setting_default_)
			{
				sample_rate_.set_current_index(sample_rate_default_index_);
			}
			updating_sample_rate_ = false;
			commit_sample_rate();
			update_sample_rate_selector_value();
			if (is_count_setting_default_)
			{
				sample_count_.set_current_index(sample_count_default_index_);
				commit_sample_count();
			}
			commit_channel_number(channel_number_);
			// end
		}

		void MainBar::commit_sample_rate()
		{
			uint64_t sample_rate = 0;

			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;
			double sample_count = 0;
			const shared_ptr<sigrok::Device> sr_dev = device->device();

			sample_rate = sample_rate_.value();
			try
			{
				sr_dev->config_set(ConfigKey::SAMPLERATE,
								   Glib::Variant<guint64>::create(sample_rate));
				sample_count = ((double)sample_count_.get_current_data(sample_count_.get_current_index())) / 10000.00 * (double)sample_rate_.get_current_data(sample_rate_.get_current_index());
				sr_dev->config_set(ConfigKey::LIMIT_SAMPLES,
								   Glib::Variant<guint64>::create(sample_count));
				// update_sample_rate_selector();
			}
			catch (Error &error)
			{
				qDebug() << tr("Failed to configure samplerate:") << error.what();
				return;
			}
			// Devices with built-in memory might impose limits on certain
			// configurations, so let's check what sample count the driver
			// lets us use now.
			sample_rate_value_ = sample_rate;
			sample_count_value_ = sample_count;
			update_sample_count_selector();
		}

		void MainBar::commit_channel_number(uint64_t channelNum)
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;
			const shared_ptr<sigrok::Device> sr_dev = device->device();
			try
			{
				sr_dev->config_set(ConfigKey::NUM_LOGIC_CHANNELS,
								   Glib::Variant<guint64>::create((uint64_t)channelNum));
			}
			catch (Error &error)
			{
				// Do nothing...
			}
		}

		void MainBar::commit_threshold_value(int index, float value)
		{
			value *= 10;
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;
			const shared_ptr<sigrok::Device> sr_dev = device->device();
			try
			{
				if (index == 1)
				{
					sr_dev->config_set(ConfigKey::THRESHOLD_VALUE_1,
									   Glib::Variant<guint64>::create((uint64_t)value));
				}
				else
				{
					sr_dev->config_set(ConfigKey::THRESHOLD_VALUE_2,
									   Glib::Variant<guint64>::create((uint64_t)value));
				}
			}
			catch (Error &error)
			{
				// Do nothing...
			}
		}

		uint64_t MainBar::get_trigger_offset()
		{
			uint64_t offset = 0;
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return 0;
			const shared_ptr<sigrok::Device> sr_dev = device->device();
			if (!sr_dev)
				return 0;
			try
			{
				auto gvar = sr_dev->config_get(ConfigKey::TRIGGER_OFFSET);
				if (gvar.gobj())
				{
					offset = Glib::VariantBase::cast_dynamic<Glib::Variant<guint64>>(
								 gvar)
								 .get();
				}
			}
			catch (Error &error)
			{
				// Do nothing
			}
			return offset;
		}

		void MainBar::show_hardware_version()
		{
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;
			const shared_ptr<sigrok::Device> sr_dev = device->device();
			if (!sr_dev)
				return;
			try
			{
				auto gvar = sr_dev->config_get(ConfigKey::HARDWARE_VERSION);
				hardware_version_ =
					Glib::VariantBase::cast_dynamic<Glib::Variant<guint64>>(gvar).get();
			}
			catch (Error &error)
			{
				qDebug() << "auto gvar = sr_dev->config_get(ConfigKey::HARDWARE_VERSION)";
			}
			Application *a = qobject_cast<Application *>(QApplication::instance());
			a->set_hardware_version(hardware_version_);
			a->renew_version_info();
		}

		void MainBar::commit_sample_count()
		{
			uint64_t sample_count = 0;
			const shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device)
				return;
			const shared_ptr<sigrok::Device> sr_dev = device->device();

			sample_count = ((double)sample_count_.get_current_data(sample_count_.get_current_index())) / 10000.00 * (double)sample_rate_.get_current_data(sample_rate_.get_current_index());
			{
				const double bytes_per_sample = (double)(channel_number_ / 8.00);
				const double max_samples =
					18000000000.00 / (bytes_per_sample > 0.0 ? bytes_per_sample : 1.0);
				if (sample_count <= 0)
					sample_count = 0.5 * (double)sample_rate_.get_current_data(sample_rate_.get_current_index());
				else if (sample_count > max_samples)
					sample_count = (uint64_t)max_samples;
			}
			if (sample_count_supported_)
			{
				try
				{
					sr_dev->config_set(ConfigKey::LIMIT_SAMPLES,
									   Glib::Variant<guint64>::create(sample_count));
					//  update_sample_count_selector();
				}
				catch (Error &error)
				{
					qDebug() << tr("Failed to configure sample count:") << error.what();
					return;
				}
			}
			sample_count_value_ = sample_count;
			// Devices with built-in memory might impose limits on certain
			// configurations, so let's check what sample rate the driver
			// lets us use now.
			// update_sample_rate_selector();
		}

		void MainBar::show_session_error(const QString text, const QString info_text)
		{
			QDialog dialog(this);
			dialog.setWindowTitle(tr("错误"));

			// 设置更大的字体
			QFont font = dialog.font();
			font.setPointSize(font.pointSize() + 2);
			dialog.setFont(font);

			// 设置对话框最小宽度，高度自动计算
			dialog.setMinimumWidth(400);
			dialog.adjustSize();

			QVBoxLayout *layout = new QVBoxLayout(&dialog);

			// 创建图标和文本布局
			QHBoxLayout *contentLayout = new QHBoxLayout();

			// 创建图标标签
			QLabel *iconLabel = new QLabel(&dialog);
			iconLabel->setPixmap(QMessageBox::standardIcon(QMessageBox::Warning));
			iconLabel->setAlignment(Qt::AlignTop);
			contentLayout->addWidget(iconLabel);

			// 创建文本标签
			QLabel *label = new QLabel(&dialog);
			label->setText(text + "\n\n" + info_text);
			label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
			label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
			contentLayout->addWidget(label, 1);

			layout->addLayout(contentLayout);

			// 创建按钮
			QHBoxLayout *buttonLayout = new QHBoxLayout();
			buttonLayout->addStretch();
			QPushButton *okBtn = new QPushButton(tr("确定"), &dialog);
			buttonLayout->addWidget(okBtn);
			layout->addLayout(buttonLayout);

			// 连接信号
			connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

			dialog.exec();
		}

		void MainBar::export_file(shared_ptr<OutputFormat> format, bool selection_only, QString file_name)
		{
			using pv::dialogs::StoreProgress;

			// Stop any currently running capture session
			session_.stop_capture();

			QSettings settings;
			const QString dir = settings.value(SettingSaveDirectory).toString();

			pair<uint64_t, uint64_t> sample_range;

			// Selection only? Verify that the cursors are active and fetch their values
			if (selection_only)
			{
				views::trace::View *trace_view =
					qobject_cast<views::trace::View *>(session_.main_view().get());

				if (!trace_view->cursors()->enabled())
				{
					show_session_error(tr("Missing Cursors"), tr("You need to set the "
																 "cursors before you can save the data enclosed by them "
																 "to a session file (e.g. using the Show Cursors button)."));
					return;
				}

				const double samplerate = session_.get_samplerate();

				const pv::util::Timestamp &start_time = trace_view->cursors()->first()->time();
				const pv::util::Timestamp &end_time = trace_view->cursors()->second()->time();

				const uint64_t start_sample = (uint64_t)max(
					0.0, start_time.convert_to<double>() * samplerate);
				const uint64_t end_sample = (uint64_t)max(
					0.0, end_time.convert_to<double>() * samplerate);

				if ((start_sample == 0) && (end_sample == 0))
				{
					// Both cursors are negative and were clamped to 0
					show_session_error(tr("Invalid Range"), tr("The cursors don't "
															   "define a valid range of samples."));
					return;
				}

				sample_range = make_pair(start_sample, end_sample);
			}
			else
			{
				sample_range = make_pair(0, 0);
			}

			// Construct the filter
			const vector<string> exts = format->extensions();
			QString filter = tr("%1 files ").arg(QString::fromStdString(format->description()));

			if (exts.empty())
				filter += "(*)";
			else
				filter += QString("(*.%1);;%2 (*)").arg(QString::fromStdString(join(exts, ", *.")), tr("All Files"));

			// Show the file dialog
			if (file_name.isEmpty())
				file_name = QFileDialog::getSaveFileName(this, tr("Save File"), dir, filter);

			if (file_name.isEmpty())
				return;

			const QString abs_path = QFileInfo(file_name).absolutePath();
			settings.setValue(SettingSaveDirectory, abs_path);

			// Show the options dialog
			map<string, Glib::VariantBase> options;
			if (!format->options().empty())
			{
				dialogs::InputOutputOptions dlg(
					tr("Export %1").arg(QString::fromStdString(format->description())),
					format->options(), this);
				if (!dlg.exec())
					return;
				options = dlg.options();
			}

			if (!selection_only)
			{
				if (format == session_.device_manager().context()->output_formats()["srzip"])
				{
					session_.set_save_path(QFileInfo(file_name).absolutePath());
					session_.set_name(QFileInfo(file_name).fileName());
				}
				else
					session_.set_save_path("");
			}

			StoreProgress *dlg = new StoreProgress(file_name, format, options,
												   sample_range, session_, this);
			dlg->run();
		}

		void MainBar::import_file(shared_ptr<InputFormat> format)
		{
			assert(format);

			QSettings settings;
			const QString dir = settings.value(SettingOpenDirectory).toString();

			// Construct the filter
			const vector<string> exts = format->extensions();
			const QString filter_exts = exts.empty() ? "" : QString::fromStdString("%1 (%2)").arg(tr("%1 files").arg(QString::fromStdString(format->description())), QString::fromStdString("*.%1").arg(QString::fromStdString(join(exts, " *."))));
			const QString filter_all = QString::fromStdString("%1 (%2)").arg(
				tr("All Files"), QString::fromStdString("*"));
			const QString filter = QString::fromStdString("%1%2%3").arg(
				exts.empty() ? "" : filter_exts,
				exts.empty() ? "" : ";;",
				filter_all);

			// Show the file dialog
			const QString file_name = QFileDialog::getOpenFileName(
				this, tr("Import File"), dir, filter);

			if (file_name.isEmpty())
				return;

			// Show the options dialog
			map<string, Glib::VariantBase> options;
			if (!format->options().empty())
			{
				dialogs::InputOutputOptions dlg(
					tr("Import %1").arg(QString::fromStdString(format->description())),
					format->options(), this);
				if (!dlg.exec())
					return;
				options = dlg.options();
			}

			session_.load_file(file_name, "", format, options);
			set_vendorName(file_name);
			const QString abs_path = QFileInfo(file_name).absolutePath();
			settings.setValue(SettingOpenDirectory, abs_path);
		}

		void MainBar::on_device_selected()
		{
			shared_ptr<devices::Device> device = device_selector_.selected_device();
			if (!device) {
				return;
			}

			const shared_ptr<sigrok::Device> sr_dev = device->device();
			QString vendor = QString::fromStdString(sr_dev->vendor());
			QString model = QString::fromStdString(sr_dev->model());

			// 处理 CH569 和 CH32H417 设备的切换
			if (vendor == "USB3.0(CH569)" || vendor == "USB3.0(CH32H417)")
			{
				// 更新设备选择器显示的名称
				QString device_name = (vendor == "USB3.0(CH569)")
										  ? "USB3.0(CH569)"
										  : "USB3.0(CH32H417)";
				QString vendor_name = (vendor == "USB3.0(CH569)")
										  ? "USB3.0(CH569)"
										  : "USB3.0(CH32H417)";
				set_device_selector_name(device_name);
				set_vendorName(vendor_name);

				// 切换到选中的设备
				session_.select_device(device);

				if (vendor == "USB3.0(CH32H417)")
				{
					try
					{
						sr_dev->config_set(ConfigKey::PATTERN_MODE,
										   Glib::Variant<Glib::ustring>::create("Logic Analyzer"));
						qDebug() << "CH32H417 device mode set to Logic Analyzer on device switch";
						update_sample_rate_selector();
						update_sample_rate_selector_value();
					}
					catch (Error &error)
					{
						qDebug() << tr("Failed to set CH32H417 mode on device switch:") << error.what();
					}
				}
				return;
			}

			// 其他设备的处理
			session_.select_device(device);
		}

		void MainBar::hot_plug_listen()
		{
			uint16_t vid = WCH_VID;
			uint16_t pid_ch569 = WCH_PID_CH569;
			/* CH32H417检测已移至CH375SetDeviceNotify回调 */

			// 跟踪CH569设备的插拔状态
			bool ch569_attached = false;
			bool ch569_attached_pre = false;

			libusb_device **devlist;
			struct libusb_device_descriptor des;
			libusb_context *libusb_ctx = NULL;
			int device_count = 0;
			int index = 0;
			int index_time = 0;

			while (hot_plug_listen_)
			{
				device_count = libusb_get_device_list(libusb_ctx, &devlist);
				ch569_attached = false;

				for (index = 0; index < device_count; index++)
				{
					libusb_get_device_descriptor(devlist[index], &des);
					if (des.idVendor == vid && des.idProduct == pid_ch569)
					{
						ch569_attached = true;
						break;
					}
				}
				libusb_free_device_list(devlist, 1);

				if (index_time < 3)
				{
					index_time++;
				}
				else
				{
					// 获取当前选中的设备类型
					QString currentVendorName = get_vendorName();
					bool is_using_ch569 = (currentVendorName == "USB3.0(CH569)");

					// 检查 CH569 状态变化
					if (ch569_attached != ch569_attached_pre)
					{
						if (ch569_attached)
						{
							// CH569 插入，只更新设备列表，不自动切换
							device_attached();
						}
						else if (is_using_ch569)
						{
							// 当前使用的 CH569 被拔出
							set_vendorName("");
							session_.stop_capture();
							divice_detached();
						}
						index_time = 0;
					}
				}

				g_usleep(100000);
				ch569_attached_pre = ch569_attached;
			}
		}

		void MainBar::on_device_changed()
		{
			update_device_list();
			update_device_config_widgets();
			update_device_status_icon();
			renew_setting_default();
		}

		void MainBar::hot_plug_start()
		{
			/* 设置静态实例指针 */
			s_mainbar_instance = this;

			/* 注册unsigned long为Qt元类型，用于跨线程信号传递 */
			qRegisterMetaType<unsigned long>("unsigned long");

			/* 初始化CH375DLL用于CH32H417热插拔检测 */
#ifdef _WIN32
			ch375_init();

			/* 注册CH32H417设备事件通知回调 */
			ch375_set_device_notify(0, NULL, ch32h417_notify_callback);
#endif

			/* 启动CH569轮询线程 */
			if (t_hot_plug.joinable())
			{
				hot_plug_listen_ = false;
				t_hot_plug.join();
			}
			hot_plug_listen_ = true;
			usleep(10000);
			t_hot_plug = std::thread(hot_plug_listen, this);
		}

		void MainBar::hot_plug_stop()
		{
			/* 清除静态实例指针 */
			s_mainbar_instance = nullptr;

			/* 取消CH32H417设备事件通知回调 */
#ifdef _WIN32
			ch375_set_device_notify(0, NULL, NULL);
#endif

			/* 停止CH569轮询线程 */
			if (t_hot_plug.joinable())
			{
				hot_plug_listen_ = false;
				t_hot_plug.join();
			}

			/* 清理CH375DLL */
#ifdef _WIN32
			ch375_cleanup();
#endif
		}

		void MainBar::on_capture_state_changed(int state)
		{
			set_capture_state((pv::Session::capture_state)state);
		}

		void MainBar::on_sample_count_changed()
		{
			if (!updating_sample_count_)
				commit_sample_count();
		}

		void MainBar::on_sample_rate_changed()
		{
			if (!updating_sample_rate_)
				commit_sample_rate();
		}

		void MainBar::on_config_changed()
		{
			// We want to also call update_sample_rate_selector() here in case
			// the user changed the SR_CONF_EXTERNAL_CLOCK option. However,
			// commit_sample_rate() does this already, so we don't call it here

			commit_sample_count();
			commit_sample_rate();
		}

		void MainBar::on_actionSingle_triggered()
		{
			capture_mode_ = Session::Single;
			model_change_button_->setIcon(QIcon(":/icons/Single.png"));
			// model_change_button_->setText(tr("Single"));
			model_change_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
		}

		void MainBar::on_actionRepeat_triggered()
		{
			capture_mode_ = Session::Repeat;
			model_change_button_->setIcon(QIcon(":/icons/Repeat.png"));
			// model_change_button_->setText(tr("Repeat"));
			model_change_button_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
		}

		void MainBar::update_runstop_status(int state)
		{
			if (state == Session::Stopped)
			{
				// set start icon
				run_stop_button_->setIcon(QIcon(":/icons/Start.png"));
				run_stop_button_->setText(tr("Start"));
			}
			else
			{
				run_stop_button_->setIcon(QIcon(":/icons/Stop.png"));
				run_stop_button_->setText(tr("Stop"));
			}
		}

		void MainBar::on_actionNewView_triggered(QAction *action)
		{
			if (action)
				new_view(&session_, action->data().toInt());
			else
				// When the icon of the button is clicked, we create a trace view
				new_view(&session_, views::ViewTypeTabularDecoder);
		}

		void MainBar::on_actionOpen_triggered()
		{
			QSettings settings;
			const QString dir = settings.value(SettingOpenDirectory).toString();

			// Show the dialog
			const QString file_name = QFileDialog::getOpenFileName(
				this, tr("Open File"), dir, tr("sigrok Sessions (*.sr);;"
											   "All Files (*)"));

			if (!file_name.isEmpty())
			{
				session_.load_file(file_name);

				const QString abs_path = QFileInfo(file_name).absolutePath();
				settings.setValue(SettingOpenDirectory, abs_path);
			}
		}

		void MainBar::on_actionSave_triggered()
		{
			// A path is only set if we loaded/saved an srzip file before
			if (session_.save_path().isEmpty())
			{
				on_actionSaveAs_triggered();
				return;
			}

			QFileInfo fi = QFileInfo(QDir(session_.save_path()), session_.name());
			export_file(session_.device_manager().context()->output_formats()["srzip"], false,
						fi.absoluteFilePath());
		}

		void MainBar::on_actionSaveAs_triggered()
		{
			export_file(session_.device_manager().context()->output_formats()["srzip"]);
		}

		void MainBar::on_actionSaveSelectionAs_triggered()
		{
			export_file(session_.device_manager().context()->output_formats()["srzip"], true);
		}

		void MainBar::on_actionSaveSetup_triggered()
		{
			QSettings settings;
			const QString dir = settings.value(SettingSaveDirectory).toString();

			const QString file_name = QFileDialog::getSaveFileName(
				this, tr("Save File"), dir, tr("PulseView Session Setups (*.pvs);;"
											   "All Files (*)"));

			if (file_name.isEmpty())
				return;

			QSettings settings_storage(file_name, QSettings::IniFormat);
			session_.save_setup(settings_storage);
		}

		void MainBar::on_actionRestoreSetup_triggered()
		{
			QSettings settings;
			const QString dir = settings.value(SettingSaveDirectory).toString();

			const QString file_name = QFileDialog::getOpenFileName(
				this, tr("Open File"), dir, tr("PulseView Session Setups (*.pvs);;"
											   "All Files (*)"));

			if (file_name.isEmpty())
				return;

			QSettings settings_storage(file_name, QSettings::IniFormat);
			session_.restore_setup(settings_storage);
		}

		void MainBar::on_actionConnect_triggered()
		{
			// Stop any currently running capture session
			session_.stop_capture();

			dialogs::Connect dlg(this, session_.device_manager());

			// If the user selected a device, select it in the device list. Select the
			// current device otherwise.
			if (dlg.exec())
				session_.select_device(dlg.get_selected_device());

			update_device_list();
		}

		void MainBar::on_add_decoder_clicked()
		{
			show_decoder_selector(&session_);
		}

		void MainBar::on_add_math_signal_clicked()
		{
			// shared_ptr<data::SignalBase> signal = make_shared<data::MathSignal>(session_);
			// session_.add_generated_signal(signal);
		}

		void MainBar::add_toolbar_widgets()
		{
			addWidget(device_status_icon_);
			addWidget(open_button_);
			addWidget(save_button_);
			addSeparator();

			addWidget(&device_selector_);
			if (channels_button_ex_)
				addWidget(channels_button_ex_);
			addWidget(&sample_count_);
			addWidget(&sample_rate_);
			addWidget(model_change_button_);
			addWidget(run_stop_button_);
			addSeparator();

			StandardBar::add_toolbar_widgets();
#ifdef ENABLE_DECODE
			addWidget(add_decoder_button_);
#endif
			addWidget(new_view_button_);
			addSeparator();

			addWidget(help_button_);
		}

		bool MainBar::eventFilter(QObject *watched, QEvent *event)
		{
			if (sample_count_supported_ && (watched == &sample_count_ || watched == &sample_rate_) &&
				(event->type() == QEvent::ToolTip))
			{
				uint64_t sampleCount = ((double)sample_count_.get_current_data(sample_count_.get_current_index())) / 10000.00 * (double)sample_rate_.get_current_data(sample_rate_.get_current_index());
				QHelpEvent *help_event = static_cast<QHelpEvent *>(event);

				QString str = tr("Total Sampling Count: %1").arg(sampleCount);
				QToolTip::showText(help_event->globalPos(), str);

				return true;
			}

			return false;
		}

		uint64_t MainBar::get_sample_count()
		{
			return sample_count_value_;
		}

		uint64_t MainBar::get_sample_rate()
		{
			return sample_rate_value_;
		}

		uint16_t MainBar::get_sample_count_index()
		{
			return sample_count_.get_current_index();
		}

		void MainBar::channels_button_ex_clicked()
		{
			QDialog *dialog = new QDialog(this);
			dialog->setWindowTitle(tr("Device Options"));
			dialog->setModal(true);

			dialog->setWindowFlags(dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);

			dialog->setFixedSize(channels_->sizeHint());

			QVBoxLayout *layout = new QVBoxLayout(dialog);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->addWidget(channels_);
			connect(dialog, &QDialog::finished, this, [this, dialog]()
					{ channels_->setParent(this); channels_->parent_dialog_ = nullptr; });
			dialog->setLayout(layout);

			channels_->parent_dialog_ = dialog;

			QWidget *mainWin = this->window();
			dialog->move(mainWin->geometry().center() - dialog->rect().center());

			dialog->exec();

			delete dialog;
		}

		void MainBar::on_action_version_info_triggered()
		{
			QString softwareVersion = "0.4 based on pulseview 0.5.0-git-7e5c839";
			QString firmwareVersion;
			QString fpgaVersion;
			QString text;
			QString vendorName = get_vendorName();

			// demo设备或无设备时，只显示软件版本
			if (vendorName != "USB3.0(CH32H417)" && vendorName != "USB3.0(CH569)")
			{
				text = QString("<b>软件版本：</b> %1").arg(softwareVersion);
			}
			else
			{
				show_hardware_version();

				if (hardware_version_ == 0)
				{
					firmwareVersion = "null";
					fpgaVersion = "null";
				}
				else if (vendorName == "USB3.0(CH32H417)")
				{
					// CH32H417: fw_version格式为 (固件版本 << 8) | 硬件版本
					// 显示格式: 硬件版本.固件版本 (如 1.1)
					uint8_t fw = (hardware_version_ >> 8) & 0xff; // 固件版本
					uint8_t hw = hardware_version_ & 0xff;		  // 硬件版本
					firmwareVersion = QString::number(hw) + "." + QString::number(fw);

					text = QString(
							   "<b>软件版本：</b> %1<br/>"
							   "<b>固件版本：</b> %2<br/>")
							   .arg(softwareVersion)
							   .arg(firmwareVersion);
				}
				else
				{
					// CH569有固件版本和FPGA版本
					firmwareVersion = QString::number((hardware_version_ >> 16) & 0xff);
					firmwareVersion += ".";
					firmwareVersion += QString::number((hardware_version_ >> 24) & 0xff);
					fpgaVersion = QString::number((hardware_version_ >> 0) & 0xff);
					fpgaVersion += ".";
					fpgaVersion += QString::number((hardware_version_ >> 8) & 0xff);
					text = QString(
							   "<b>软件版本：</b> %1<br/>"
							   "<b>固件版本：</b> %2<br/>"
							   "<b>FPGA版本：</b> %3")
							   .arg(softwareVersion)
							   .arg(firmwareVersion)
							   .arg(fpgaVersion);
				}
			}
			QDialog dialog(this);
				dialog.setWindowTitle(tr("版本信息"));

				// 设置更大的字体
				QFont font = dialog.font();
				font.setPointSize(font.pointSize() + 2);
				dialog.setFont(font);

				// 设置对话框最小宽度，高度自动计算
				dialog.setMinimumWidth(400);
				dialog.adjustSize();

				QVBoxLayout *layout = new QVBoxLayout(&dialog);

				// 创建图标和文本布局
				QHBoxLayout *contentLayout = new QHBoxLayout();

				// 创建图标标签
				QLabel *iconLabel = new QLabel(&dialog);
				iconLabel->setPixmap(QMessageBox::standardIcon(QMessageBox::Information));
				iconLabel->setAlignment(Qt::AlignTop);
				contentLayout->addWidget(iconLabel);

				// 创建文本标签
				QLabel *label = new QLabel(&dialog);
				label->setText(text);
				label->setTextFormat(Qt::RichText);
				label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
				label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
				contentLayout->addWidget(label, 1);

				layout->addLayout(contentLayout);

				// 创建按钮
				QHBoxLayout *buttonLayout = new QHBoxLayout();
				buttonLayout->addStretch();
				QPushButton *okBtn = new QPushButton(tr("确定"), &dialog);
				buttonLayout->addWidget(okBtn);
				layout->addLayout(buttonLayout);

				// 连接信号
				connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

				dialog.exec();
		}

		void MainBar::on_action_feedback_triggered()
		{
		}

		void MainBar::on_action_update_triggered()
		{
			/* 检查当前设备是否为CH32H417（USB模式或HID模式） */
			QString currentVendorName = get_vendorName();
			bool is_ch32h417_usb = (currentVendorName == "USB3.0(CH32H417)");

			bool is_ch32h417_hid = false;
			libusb_device **hid_devlist;
			struct libusb_device_descriptor hid_des;
			libusb_context *hid_ctx = NULL;
			int hid_count = libusb_get_device_list(hid_ctx, &hid_devlist);
			for (int i = 0; i < hid_count; i++) {
				libusb_get_device_descriptor(hid_devlist[i], &hid_des);
				if (hid_des.idVendor == WCH_VID && hid_des.idProduct == WCH_PID_CH32H417_HID) {
					is_ch32h417_hid = true;
					break;
				}
			}
			if (hid_count >= 0)
				libusb_free_device_list(hid_devlist, 1);

			if (!is_ch32h417_usb && !is_ch32h417_hid)
			{
				QDialog dialog(this);
				dialog.setWindowTitle(tr("警告"));

				// 设置更大的字体
				QFont font = dialog.font();
				font.setPointSize(font.pointSize() + 2);
				dialog.setFont(font);

				// 设置对话框最小宽度，高度自动计算
				dialog.setMinimumWidth(350);
				dialog.adjustSize();

				QVBoxLayout *layout = new QVBoxLayout(&dialog);

				// 创建图标和文本布局
				QHBoxLayout *contentLayout = new QHBoxLayout();

				// 创建图标标签
				QLabel *iconLabel = new QLabel(&dialog);
				iconLabel->setPixmap(QMessageBox::standardIcon(QMessageBox::Warning));
				iconLabel->setAlignment(Qt::AlignTop);
				contentLayout->addWidget(iconLabel);

				// 创建文本标签
				QLabel *label = new QLabel(&dialog);
				label->setText(tr("当前设备不支持固件升级。\n请连接CH32H417设备后重试。"));
				label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
				label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
				contentLayout->addWidget(label, 1);

				layout->addLayout(contentLayout);

				// 创建按钮
				QHBoxLayout *buttonLayout = new QHBoxLayout();
				buttonLayout->addStretch();
				QPushButton *okBtn = new QPushButton(tr("确定"), &dialog);
				buttonLayout->addWidget(okBtn);
				layout->addLayout(buttonLayout);

				// 连接信号
				connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

				dialog.exec();
				return;
			}

			/* 弹出文件选择对话框，让用户选择固件文件 */
			QString firmwarePath = QFileDialog::getOpenFileName(this,
																tr("选择固件文件"),
																QCoreApplication::applicationDirPath(),
																tr("固件文件 (*.bin *.BIN *.hex *.HEX);;所有文件 (*.*)"));

			/* 用户取消选择 */
			if (firmwarePath.isEmpty())
			{
				return;
			}

			if (session_.get_capture_state() != Session::Stopped)
			{
				session_.stop_capture();
			}

			/* 创建进度对话框 */
			QProgressDialog *progressDlg = new QProgressDialog(
				tr("正在升级固件，请勿断开USB连接！"), tr("取消"), 0, 100, this);
			progressDlg->setWindowTitle(tr("固件升级"));
			progressDlg->setWindowModality(Qt::WindowModal);
			progressDlg->setMinimumDuration(0);
			progressDlg->setValue(0);
			progressDlg->setCancelButton(nullptr); /* 禁止取消 */

			/* 设置IAP升级标志，防止热插拔检测误报设备掉线 */
			iap_upgrading_ = true;

			pv::dialogs::IAPWorker *worker = new pv::dialogs::IAPWorker(0, firmwarePath, WCH_VID, WCH_PID_CH32H417_HID, is_ch32h417_hid);
			QThread *workerThread = new QThread();
			worker->moveToThread(workerThread);

			connect(workerThread, &QThread::started, worker, &pv::dialogs::IAPWorker::do_work);
			connect(worker, &pv::dialogs::IAPWorker::progress_updated, progressDlg, &QProgressDialog::setValue);
			connect(worker, &pv::dialogs::IAPWorker::finished, [=](bool success)
					{
				progressDlg->close();
				if (!success) {
					iap_upgrading_ = false;
					QDialog dialog(this);
					dialog.setWindowTitle(tr("升级失败"));

					// 设置更大的字体
					QFont font = dialog.font();
					font.setPointSize(font.pointSize() + 2);
					dialog.setFont(font);

					// 设置对话框最小宽度，高度自动计算
					dialog.setMinimumWidth(400);
					dialog.adjustSize();

					QVBoxLayout *layout = new QVBoxLayout(&dialog);

					// 创建图标和文本布局
					QHBoxLayout *contentLayout = new QHBoxLayout();

					// 创建图标标签
					QLabel *iconLabel = new QLabel(&dialog);
					iconLabel->setPixmap(QMessageBox::standardIcon(QMessageBox::Critical));
					iconLabel->setAlignment(Qt::AlignTop);
					contentLayout->addWidget(iconLabel);

					// 创建文本标签
					QLabel *label = new QLabel(&dialog);
					label->setText(tr("固件升级失败！\n请检查设备连接和固件文件。"));
					label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
					label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
					contentLayout->addWidget(label, 1);

					layout->addLayout(contentLayout);

					// 创建按钮
					QHBoxLayout *buttonLayout = new QHBoxLayout();
					buttonLayout->addStretch();
					QPushButton *okBtn = new QPushButton(tr("确定"), &dialog);
					buttonLayout->addWidget(okBtn);
					layout->addLayout(buttonLayout);

					// 连接信号
					connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

					dialog.exec();
				}
				workerThread->quit(); });
			connect(workerThread, &QThread::finished, workerThread, &QThread::deleteLater);
			connect(workerThread, &QThread::finished, worker, &dialogs::IAPWorker::deleteLater);
			connect(workerThread, &QThread::finished, progressDlg, &QProgressDialog::deleteLater);

			workerThread->start();

			return;
		}

		void MainBar::on_action_use_guide_triggered()
		{
			// 获取可执行文件所在目录
			QString appDir = QCoreApplication::applicationDirPath();

			QString pdfPath = appDir + "/doc/U3LogicAnalyzer逻辑分析仪使用手册.pdf";

			pdfPath = QDir::toNativeSeparators(pdfPath);

			QFileInfo fileInfo(pdfPath);
			if (!fileInfo.exists())
			{
				QDialog dialog(this);
				dialog.setWindowTitle(tr("提示信息"));

				// 设置更大的字体
				QFont font = dialog.font();
				font.setPointSize(font.pointSize() + 2);
				dialog.setFont(font);

				// 设置对话框最小宽度，高度自动计算
				dialog.setMinimumWidth(500);
				dialog.adjustSize();

				QVBoxLayout *layout = new QVBoxLayout(&dialog);

				// 创建图标和文本布局
				QHBoxLayout *contentLayout = new QHBoxLayout();

				// 创建图标标签
				QLabel *iconLabel = new QLabel(&dialog);
				iconLabel->setPixmap(QMessageBox::standardIcon(QMessageBox::Information));
				iconLabel->setAlignment(Qt::AlignTop);
				contentLayout->addWidget(iconLabel);

				// 创建文本标签
				QLabel *label = new QLabel(&dialog);
				label->setText(tr("找不到PDF文件：\n%1\n\n请确认 doc 目录和用户手册文件是否存在。").arg(pdfPath));
				label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
				label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
				contentLayout->addWidget(label, 1);

				layout->addLayout(contentLayout);

				// 创建按钮
				QHBoxLayout *buttonLayout = new QHBoxLayout();
				buttonLayout->addStretch();
				QPushButton *okBtn = new QPushButton(tr("确定"), &dialog);
				buttonLayout->addWidget(okBtn);
				layout->addLayout(buttonLayout);

				// 连接信号
				connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

				dialog.exec();
				return;
			}

			QUrl url = QUrl::fromLocalFile(pdfPath);

			bool opened = QDesktopServices::openUrl(url);
			if (!opened)
			{
				QDialog dialog(this);
				dialog.setWindowTitle(tr("打开失败"));

				// 设置更大的字体
				QFont font = dialog.font();
				font.setPointSize(font.pointSize() + 2);
				dialog.setFont(font);

				// 设置对话框最小宽度，高度自动计算
				dialog.setMinimumWidth(400);
				dialog.adjustSize();

				QVBoxLayout *layout = new QVBoxLayout(&dialog);

				// 创建图标和文本布局
				QHBoxLayout *contentLayout = new QHBoxLayout();

				// 创建图标标签
				QLabel *iconLabel = new QLabel(&dialog);
				iconLabel->setPixmap(QMessageBox::standardIcon(QMessageBox::Warning));
				iconLabel->setAlignment(Qt::AlignTop);
				contentLayout->addWidget(iconLabel);

				// 创建文本标签
				QLabel *label = new QLabel(&dialog);
				label->setText(tr("无法打开PDF文件，请确认系统已安装PDF阅读器。"));
				label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
				label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
				contentLayout->addWidget(label, 1);

				layout->addLayout(contentLayout);

				// 创建按钮
				QHBoxLayout *buttonLayout = new QHBoxLayout();
				buttonLayout->addStretch();
				QPushButton *okBtn = new QPushButton(tr("确定"), &dialog);
				buttonLayout->addWidget(okBtn);
				layout->addLayout(buttonLayout);

				// 连接信号
				connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

				dialog.exec();
			}
		}

		void MainBar::on_action_about_triggered()
		{
			QString appName = "U3LogicAnalyzer";
			QString appVersion = "v0.4";
			QString website = "<a href='https://oshwhub.com/q2h2/project_bszkxrnf' style='color:#afe2ff;'>访问我们：https://oshwhub.com/q2h2/project_bszkxrnf</a>";
			QString text = QString(
						   "<b>%1 (%2)</b><br/><br/>"
						   "%3")
						   .arg(appName)
						   .arg(appVersion)
						   .arg(website);

			QDialog dialog(this);
			dialog.setWindowTitle(tr("关于"));

			// 设置更大的字体
			QFont font = dialog.font();
			font.setPointSize(font.pointSize() + 2);
			dialog.setFont(font);

			// 设置对话框最小宽度，高度自动计算
			dialog.setMinimumWidth(500);
			dialog.adjustSize();

			QVBoxLayout *layout = new QVBoxLayout(&dialog);

			// 创建图标和文本布局
			QHBoxLayout *contentLayout = new QHBoxLayout();

			// 创建图标标签
			QLabel *iconLabel = new QLabel(&dialog);
			iconLabel->setPixmap(QPixmap(":/icons/pulseview.png"));
			iconLabel->setAlignment(Qt::AlignTop);
			contentLayout->addWidget(iconLabel);

			// 创建文本标签
			QLabel *label = new QLabel(&dialog);
			label->setText(text);
			label->setTextFormat(Qt::RichText);
			label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
			label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
			label->setOpenExternalLinks(true);
			contentLayout->addWidget(label, 1);

			layout->addLayout(contentLayout);

			// 创建按钮
			QHBoxLayout *buttonLayout = new QHBoxLayout();
			buttonLayout->addStretch();
			QPushButton *okBtn = new QPushButton(tr("确定"), &dialog);
			buttonLayout->addWidget(okBtn);
			layout->addLayout(buttonLayout);

			// 连接信号
			connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

			dialog.exec();
		}

		/* CH32H417热插拔回调函数 - 在DLL线程中执行 */
		void MainBar::ch32h417_notify_callback(unsigned long eventStatus)
		{
			/* 通过QMetaObject::invokeMethod将事件传递到主线程处理 */
			if (s_mainbar_instance)
			{
				QMetaObject::invokeMethod(s_mainbar_instance, "on_ch32h417_device_event",
										  Qt::QueuedConnection, Q_ARG(unsigned long, eventStatus));
			}
		}

		/* CH32H417设备事件处理 - 在主线程中执行 */
		void MainBar::on_ch32h417_device_event(unsigned long eventStatus)
		{
			QString currentVendorName = get_vendorName();
			bool is_using_ch32h417 = (currentVendorName == "USB3.0(CH32H417)");

			if (eventStatus == CH375_DEVICE_ARRIVAL)
			{
				if (iap_upgrading_)
				{
					iap_upgrading_ = false;
					QDialog *dialog = new QDialog(this);
					dialog->setWindowTitle(tr("升级成功"));

					// 设置更大的字体
					QFont font = dialog->font();
					font.setPointSize(font.pointSize() + 2);
					dialog->setFont(font);

					// 设置对话框最小宽度，高度自动计算
					dialog->setMinimumWidth(400);
					dialog->adjustSize();

					QVBoxLayout *layout = new QVBoxLayout(dialog);

					// 创建图标和文本布局
					QHBoxLayout *contentLayout = new QHBoxLayout();

					// 创建图标标签
					QLabel *iconLabel = new QLabel(dialog);
					iconLabel->setPixmap(QMessageBox::standardIcon(QMessageBox::Information));
					iconLabel->setAlignment(Qt::AlignTop);
					contentLayout->addWidget(iconLabel);

					// 创建文本标签
					QLabel *label = new QLabel(dialog);
					label->setText(tr("固件升级成功！\n设备已重新启动。"));
					label->setStyleSheet(QString("font-size: %1pt;").arg(font.pointSize()));
					label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
					contentLayout->addWidget(label, 1);

					layout->addLayout(contentLayout);

					// 创建按钮
					QHBoxLayout *buttonLayout = new QHBoxLayout();
					buttonLayout->addStretch();
					QPushButton *okBtn = new QPushButton(tr("确定"), dialog);
					buttonLayout->addWidget(okBtn);
					layout->addLayout(buttonLayout);

					// 连接信号
					connect(okBtn, &QPushButton::clicked, dialog, &QDialog::accept);

					dialog->setWindowModality(Qt::NonModal);
					dialog->setAttribute(Qt::WA_DeleteOnClose);
					dialog->show();
				}
				/* CH32H417插入 - 如果是IAP升级后的设备重新上线，清除升级标志 */
				device_attached();
			}
			else if (eventStatus == CH375_DEVICE_REMOVE)
			{
				if (iap_upgrading_)
				{
					return;
				}
				if (is_using_ch32h417)
				{
					set_vendorName("");
					session_.stop_capture();
					divice_detached();
				}
		}
	}

} // namespace toolbars

} // namespace pv
