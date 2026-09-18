/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2014 Martin Ling <martin-sigrok@earth.li>
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

#ifndef PULSEVIEW_PV_APPLICATION_HPP
#define PULSEVIEW_PV_APPLICATION_HPP

#include <vector>

#include <QApplication>
#include <QStringList>
#include <QTranslator>
#include <QProcess>
#include <libsigrokcxx/libsigrokcxx.hpp>

#include "devicemanager.hpp"
#include "globalsettings.hpp"

using std::shared_ptr;
using std::pair;
using std::vector;

class Application : public QApplication, public pv::GlobalSettingsInterface
{
	Q_OBJECT

public:
	Application(int &argc, char* argv[]);

	const QStringList get_languages() const;
	const QString get_language_editors(const QString& language) const;
	void switch_language(const QString& language);

	void on_setting_changed(const QString &key, const QVariant &value);

	void collect_version_info(pv::DeviceManager &device_manager);
	void print_version_info();

	vector< pair<QString, QString> > get_version_info() const;
	vector<QString> get_fw_path_list() const;
	vector<QString> get_pd_path_list() const;
	vector< pair<QString, QString> > get_driver_list() const;
	vector< pair<QString, QString> > get_input_format_list() const;
	vector< pair<QString, QString> > get_output_format_list() const;
	vector< pair<QString, QString> > get_pd_list() const;

	void set_hardware_version(uint64_t hardware_version);
	void renew_version_info();

private:
	bool notify(QObject *receiver, QEvent *event);

	vector< pair<QString, QString> > version_info_;
	vector<QString> fw_path_list_;
	vector<QString> pd_path_list_;
	vector< pair<QString, QString> > driver_list_;
	vector< pair<QString, QString> > input_format_list_;
	vector< pair<QString, QString> > output_format_list_;
	vector< pair<QString, QString> > pd_list_;
	QProcess * UpdateFwVersion_;
	QTranslator app_translator_, qt_translator_, qtbase_translator_;
	uint64_t hd_version_ = 0;
};

#endif // PULSEVIEW_PV_APPLICATION_HPP
