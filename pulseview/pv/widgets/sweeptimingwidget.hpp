/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnalyzer is based on PulseView.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#ifndef PULSEVIEW_PV_WIDGETS_SWEEPTIMINGWIDGET_HPP
#define PULSEVIEW_PV_WIDGETS_SWEEPTIMINGWIDGET_HPP

#include <libsigrok/libsigrok.h>
#include <string>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QWidget>
#include <vector>
using namespace std;
namespace pv {
namespace widgets {

class SweepTimingWidget : public QWidget
{
	Q_OBJECT

private:
	enum ValueType
	{
		None,
		MinMaxStep,
		List
	};

public:
	SweepTimingWidget(const char *suffix, QWidget *parent = nullptr);
	~SweepTimingWidget();
	void allow_user_entered_values(bool value);

	void show_none();
	void show_min_max_step(uint64_t min, uint64_t max, uint64_t step);
	void show_list(const uint64_t *vals, size_t count);
	void show_list_time(const uint64_t *vals, size_t count);
	string formatTranFer(double num);
	void show_125_list(uint64_t min, uint64_t max);

	uint64_t value() const;
	void set_value(uint64_t value);

	uint64_t* pData = nullptr;
	uint64_t get_current_data(uint16_t index);
	uint16_t get_current_index();
	void set_time_value(uint64_t value, uint64_t sampleRate);
	void set_current_index(uint16_t index);

Q_SIGNALS:
	void value_changed();

private:
	const char *const suffix_;

	QHBoxLayout layout_;

	QDoubleSpinBox value_;
	QComboBox list_;

	ValueType value_type_;
};

}  // namespace widgets
}  // namespace pv

#endif // PULSEVIEW_PV_WIDGETS_SWEEPTIMINGWIDGET_HPP
