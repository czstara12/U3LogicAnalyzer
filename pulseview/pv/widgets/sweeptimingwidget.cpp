/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
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

#include "sweeptimingwidget.hpp"
#include <QDebug>
#include <cassert>
#include <cstdlib>
#include <vector>
#include <string>
#include <QString>
#include <extdef.h>
#include <QComboBox>
#include <QAbstractItemView>
#include <QFontMetrics>

using std::abs;
using std::vector;

namespace pv {
namespace widgets {

SweepTimingWidget::SweepTimingWidget(const char *suffix,
	QWidget *parent) :
	QWidget(parent),
	suffix_(suffix),
	layout_(this),
	value_(this),
	list_(this),
	value_type_(None)
{
	pData = new uint64_t[50];
	setContentsMargins(0, 0, 0, 0);

	value_.setDecimals(0);
	value_.setSuffix(QString::fromUtf8(suffix));

	connect(&list_, SIGNAL(currentIndexChanged(int)),
		this, SIGNAL(value_changed()));
	connect(&list_, SIGNAL(editTextChanged(const QString&)),
		this, SIGNAL(value_changed()));

	connect(&value_, SIGNAL(editingFinished()),
		this, SIGNAL(value_changed()));

	setLayout(&layout_);
	layout_.setMargin(0);
	layout_.addWidget(&list_);
	layout_.addWidget(&value_);

	show_none();
	

	list_.setStyleSheet("QComboBox{combobox-popup:0;}");
	list_.setMaxVisibleItems(10);
}

SweepTimingWidget::~SweepTimingWidget()
{
	if (pData != nullptr){
		delete[] pData;
		pData = nullptr;
	}
}

uint64_t SweepTimingWidget::get_current_data(uint16_t index)
{
	if(index > 50)
		return 0;
	else {
		if (pData != nullptr)
			return pData[index];
		else
			return 0;
	}
}

uint16_t SweepTimingWidget::get_current_index()
{
	return list_.currentIndex();
}

void SweepTimingWidget::set_current_index(uint16_t index)
{
	if (index > list_.count())
		return;
	list_.setCurrentIndex(index);
}

void SweepTimingWidget::show_list_time(const uint64_t *vals, size_t count)
{
	value_type_ = List;

	list_.clear();
	for (size_t i = 0; i < count; i++) {
		pData[i] = vals[i];
		QString str = QString::fromStdString(formatTranFer((double)(vals[i]) / 10000.00));
		list_.addItem(str);
	}
	list_.setSizeAdjustPolicy(QComboBox::AdjustToContents);
	list_.setMinimumWidth(110);

	value_.hide();
	list_.show();
}

string SweepTimingWidget::formatTranFer(double num)
{
	char str[512] = "";
    string strRes;
    int n = 0;
    double temp = num;
	double res;
    if (temp < 1)
    {
        while(temp < 1)
        {
            temp = temp*10*10*10;
            n++;
        }
        switch(n)
        {
            case 1 :
            sprintf(str, "%.2f %s", num * 1000, "ms");
                break;
            case 2 :
                sprintf(str, "%.2f %s", num * 1000000, "us");
                break;
            case 3 :
                sprintf(str, "%.2f %s", num * 1000000, "ns");
                break;
        }
        return str;
    }
    else
    {
        if (temp < 60)
        {
            sprintf(str, "%lld s", (uint64_t)temp);
            return str;
        }
        else if (temp < 3600)
        {
			res = temp / 60.0;
            sprintf(str, "%.2f mins", res);
            return str;
        }
        else if (temp >= 3600)
        {
			res = temp / 3600.0;
            sprintf(str, "%.2f hours", res);
            return str;
        } 
    }
	return str;
}

void SweepTimingWidget::allow_user_entered_values(bool value)
{
	list_.setEditable(value);
}

void SweepTimingWidget::show_none()
{
	value_type_ = None;
	value_.hide();
	list_.hide();
}

void SweepTimingWidget::show_min_max_step(uint64_t min, uint64_t max,
	uint64_t step)
{
	assert(max > min);
	assert(step > 0);

	value_type_ = MinMaxStep;

	value_.setRange(min, max);
	value_.setSingleStep(step);

	value_.show();
	list_.hide();
}

void SweepTimingWidget::show_list(const uint64_t *vals, size_t count)
{
	value_type_ = List;

	list_.clear();
	for (size_t i = 0; i < count; i++) {
		pData[i] = vals[i];
		char *const s = sr_si_string_u64(vals[i], suffix_);
		QString text = QString::fromUtf8(s);
		list_.addItem(text, QVariant::fromValue(vals[i]));
		g_free(s);
	}
	list_.setSizeAdjustPolicy(QComboBox::AdjustToContents);
	list_.setMinimumWidth(125);

	value_.hide();
	list_.show();
}

void SweepTimingWidget::show_125_list(uint64_t min, uint64_t max)
{
	assert(max > min);
	uint16_t num = 0;
	// Create a 1-2-5-10 list of entries.
	const unsigned int FineScales[] = {1, 2, 5};
	uint64_t value, decade;
	unsigned int fine;
	vector<uint64_t> values;

	// Compute the starting decade
	for (decade = 1; decade * 10 <= min; decade *= 10);

	// Compute the first entry
	for (fine = 0; fine < countof(FineScales); fine++)
		if (FineScales[fine] * decade >= min)
			break;

	assert(fine < countof(FineScales));

	// Add the minimum entry if it's not on the 1-2-5 progression
	if (min != FineScales[fine] * decade)
		values.push_back(min);

	while ((value = FineScales[fine] * decade) < max) {
		values.push_back(value);
		++num;
		if (num == 49)
			break;
		if (++fine >= countof(FineScales))
			fine = 0, decade *= 10;
	}

	// Add the max value
	values.push_back(max);

	// Make a C array, and give it to the sweep timing widget
	uint64_t * values_array = new uint64_t[values.size()];
	copy(values.begin(), values.end(), values_array);
	QString str = suffix_;
	if (str == "s")
	{
		show_list_time(values_array, values.size());
	}
	else
	{
		show_list(values_array, values.size());
	}
	delete[] values_array;
}

uint64_t SweepTimingWidget::value() const
{
	switch (value_type_) {
	case None:
		return 0;
	case MinMaxStep:
		return (uint64_t)value_.value();
	case List:
	{
		if (list_.isEditable()) {
			uint64_t value;
			sr_parse_sizestring(list_.currentText().toUtf8().data(), &value);
			return value;
		}

		const int index = list_.currentIndex();
		return (index >= 0) ? list_.itemData(index).value<uint64_t>() : 0;
	}
	default:
		// Unexpected value type
		assert(false);
		return 0;
	}
}

void SweepTimingWidget::set_time_value(uint64_t value, uint64_t sampleRate)
{
	int best_match = list_.count() - 1;
	bool is_best_match = false;
	for (int i = 0; i < list_.count(); i++) {
		if (((double)value / (double)sampleRate) * 10000 == pData[i]){
			best_match = i;
			is_best_match = true;
			break;
		}
	}
	if (is_best_match == false)
		best_match = 0;
	list_.setCurrentIndex(best_match);
}

void SweepTimingWidget::set_value(uint64_t value)
{
	value_.setValue(value);

	if (list_.isEditable()) {
		char *const s = sr_si_string_u64(value, suffix_);
		list_.lineEdit()->setText(QString::fromUtf8(s));
		g_free(s);
	} else {
		int best_match = list_.count() - 1;
		int64_t best_variance = INT64_MAX;

		for (int i = 0; i < list_.count(); i++) {
			const int64_t this_variance = abs(
				(int64_t)value - list_.itemData(i).value<int64_t>());
			if (this_variance < best_variance) {
				best_variance = this_variance;
				best_match = i;
			}
		}

		list_.setCurrentIndex(best_match);
	}
}

}  // namespace widgets
}  // namespace pv
