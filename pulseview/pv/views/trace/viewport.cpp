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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <QDateTime>
#include "signal.hpp"
#include "view.hpp"
#include "viewitempaintparams.hpp"
#include "viewport.hpp"

#include <pv/session.hpp>

#include <QPainterPath>
#include <QMouseEvent>
#include <QScreen>
#include <QWindow>

#include <QDebug>

using std::abs;
using std::back_inserter;
using std::copy;
using std::dynamic_pointer_cast;
using std::none_of; // NOLINT. Used in assert()s.
using std::shared_ptr;
using std::stable_sort;
using std::vector;
extern QPoint pos_;
namespace pv {
namespace views {
namespace trace {
const QColor LightBlue = QColor(17, 133, 209, 200);
const QColor Orange = QColor(238, 178, 17, 255);
Viewport::Viewport(View &parent) :
	ViewWidget(parent),
	pinch_zoom_active_(false)
{
	setAutoFillBackground(true);
	setBackgroundRole(QPalette::Base);

	// Set up settings and event handlers
	GlobalSettings settings;
	allow_vertical_dragging_ = false;

	GlobalSettings::add_change_handler(this);
}

Viewport::~Viewport()
{
	GlobalSettings::remove_change_handler(this);
}

shared_ptr<ViewItem> Viewport::get_mouse_over_item(const QPoint &pt)
{
	const ViewItemPaintParams pp(rect(), view_.scale(), view_.offset());
	const vector< shared_ptr<ViewItem> > items(this->items());
	for (auto i = items.rbegin(); i != items.rend(); i++)
		if ((*i)->enabled() && (*i)->hit_box_rect(pp).contains(pt))
			return *i;
	return nullptr;
}

void Viewport::item_hover(const shared_ptr<ViewItem> &item, QPoint pos)
{
	if (item && item->is_draggable(pos))
		setCursor(dynamic_pointer_cast<ViewItem>(item) ?
			Qt::SizeHorCursor : Qt::SizeVerCursor);
	else
		unsetCursor();
}

void Viewport::drag()
{
	drag_offset_ = view_.offset();

	if (allow_vertical_dragging_)
		drag_v_offset_ = view_.owner_visual_v_offset();
}

void Viewport::drag_by(const QPoint &delta)
{
	if (drag_offset_ == boost::none)
		return;

	view_.set_scale_offset(view_.scale(),
		(*drag_offset_ - delta.x() * view_.scale()));

	if (allow_vertical_dragging_)
		view_.set_v_offset(-drag_v_offset_ - delta.y());

	bezier_start_ = QPointF(0,0);
}

void Viewport::drag_release()
{
	drag_offset_ = boost::none;
}

vector< shared_ptr<ViewItem> > Viewport::items()
{
	vector< shared_ptr<ViewItem> > items;
	const vector< shared_ptr<ViewItem> > view_items(
		view_.list_by_type<ViewItem>());
	copy(view_items.begin(), view_items.end(), back_inserter(items));
	const vector< shared_ptr<TimeItem> > time_items(view_.time_items());
	copy(time_items.begin(), time_items.end(), back_inserter(items));
	return items;
}

bool Viewport::touch_event(QTouchEvent *event)
{
	QList<QTouchEvent::TouchPoint> touchPoints = event->touchPoints();

	if (touchPoints.count() != 2) {
		pinch_zoom_active_ = false;
		return false;
	}
	if (event->device()->type() == QTouchDevice::TouchPad) {
		return false;
	}

	const QTouchEvent::TouchPoint &touchPoint0 = touchPoints.first();
	const QTouchEvent::TouchPoint &touchPoint1 = touchPoints.last();

	if (!pinch_zoom_active_ ||
	    (event->touchPointStates() & Qt::TouchPointPressed)) {
		pinch_offset0_ = (view_.offset() + view_.scale() * touchPoint0.pos().x()).convert_to<double>();
		pinch_offset1_ = (view_.offset() + view_.scale() * touchPoint1.pos().x()).convert_to<double>();
		pinch_zoom_active_ = true;
	}

	double w = touchPoint1.pos().x() - touchPoint0.pos().x();
	if (abs(w) >= 1.0) {
		const double scale =
			fabs((pinch_offset1_ - pinch_offset0_) / w);
		double offset = pinch_offset0_ - touchPoint0.pos().x() * scale;
		if (scale > 0)
			view_.set_scale_offset(scale, offset);
	}

	if (event->touchPointStates() & Qt::TouchPointReleased) {
		pinch_zoom_active_ = false;

		if (touchPoint0.state() & Qt::TouchPointReleased) {
			// Primary touch released
			drag_release();
		} else {
			// Update the mouse down fields so that continued
			// dragging with the primary touch will work correctly
			mouse_down_point_ = touchPoint0.pos().toPoint();
			drag();
		}
	}

	return true;
}

void CalcVertexes(double startX, double startY, double endX, double endY, double& x1, double& y1, double& x2, double& y2)
{
    double arrowLength = 7;  
    double arrowDegrees = 0.5;  
    double angle = atan2(endY - startY, endX - startX) + 3.1415926;
    x1 = endX + arrowLength * cos(angle - arrowDegrees);
    y1 = endY + arrowLength * sin(angle - arrowDegrees);
    x2 = endX + arrowLength * cos(angle + arrowDegrees);
    y2 = endY + arrowLength * sin(angle + arrowDegrees);
}

void Viewport::paintEvent(QPaintEvent*)
{
	typedef void (ViewItem::*LayerPaintFunc)(
		QPainter &p, ViewItemPaintParams &pp);
	LayerPaintFunc layer_paint_funcs[] = {
		&ViewItem::paint_back, &ViewItem::paint_mid,
		&ViewItem::paint_fore, nullptr};

	vector< shared_ptr<ViewItem> > row_items(view_.list_by_type<ViewItem>());
	assert(none_of(row_items.begin(), row_items.end(),
		[](const shared_ptr<ViewItem> &r) { return !r; }));

	stable_sort(row_items.begin(), row_items.end(),
		[](const shared_ptr<ViewItem> &a, const shared_ptr<ViewItem> &b) {
			return a->drag_point(QRect()).y() < b->drag_point(QRect()).y(); });

	const vector< shared_ptr<TimeItem> > time_items(view_.time_items());
	assert(none_of(time_items.begin(), time_items.end(),
		[](const shared_ptr<TimeItem> &t) { return !t; }));

	QPainter p(this);

	// Disable antialiasing for high-DPI displays
	bool use_antialiasing =
		window()->windowHandle()->screen()->devicePixelRatio() < 2.0;
	p.setRenderHint(QPainter::Antialiasing, use_antialiasing);
	for (LayerPaintFunc *paint_func = layer_paint_funcs;
			*paint_func; paint_func++) {
		ViewItemPaintParams time_pp(rect(), view_.scale(), view_.offset());
		for (const shared_ptr<TimeItem>& t : time_items)
			(t.get()->*(*paint_func))(p, time_pp);
		
		ViewItemPaintParams row_pp(rect(), view_.scale(), view_.offset());
		for (const shared_ptr<ViewItem>& r : row_items)
			(r.get()->*(*paint_func))(p, row_pp);
	}
	p.end();
	if (bezier_start_ !=  QPointF(0,0) && bezier_end_ != QPointF(0,0)) {
		QPainter painter(this);
		painter.setPen(QPen(Qt::yellow, 1));
		painter.setRenderHint(QPainter::Antialiasing);

		painter.setPen(Qt::yellow);
		painter.drawEllipse(bezier_start_, 2, 2);
		painter.drawEllipse(bezier_end_, 2, 2);

		QPointF midPoint((bezier_start_.x() + bezier_end_.x()) / 2, (bezier_start_.y() + bezier_end_.y()) / 2);

		double dx = bezier_end_.x() - bezier_start_.x();
		double dy = bezier_end_.y() - bezier_start_.y();

		double distance = qMin(qAbs(dx), qAbs(dy)) / 2;

		// 计算两个控制点的位置
		QPointF controlPoint1(midPoint.x() - dy / std::sqrt(dx * dx + dy * dy) * distance,
							midPoint.y() + dx / std::sqrt(dx * dx + dy * dy) * distance);
		QPointF controlPoint2(midPoint.x() + dy / std::sqrt(dx * dx + dy * dy) * distance,
							midPoint.y() - dx / std::sqrt(dx * dx + dy * dy) * distance);

		// 创建贝塞尔曲线路径
		QPainterPath path;
		path.moveTo(bezier_start_);
		path.cubicTo(controlPoint1, controlPoint2, bezier_end_);

		// 绘制贝塞尔曲线
		painter.setPen(QPen(Qt::yellow, 1.5)); // 曲线颜色和宽度
		painter.drawPath(path);
	
		int typical_width = painter.boundingRect(0, 0, INT_MAX, INT_MAX,
			Qt::AlignLeft | Qt::AlignTop, bezier_width_).width();
		typical_width = typical_width + 50;
		const double width = this->width();
		const double height = view_.viewport()->height();
		const double left = view_.hover_point().x();
		const double top = view_.hover_point().y();
		const double right = left + typical_width;
		const double bottom = top + 20;
		QPointF org_pos = QPointF(right > width ? left - typical_width : left, bottom > height ? top - 20 : top);
		QRectF measure_rect = QRectF(org_pos.x(), org_pos.y(), (double)typical_width, 20.0);
		QRectF measure1_rect = QRectF(org_pos.x(), org_pos.y(), (double)typical_width, 20.0);
		painter.setPen(Qt::NoPen);
		painter.setBrush(LightBlue);
		painter.drawRect(measure_rect);
		painter.setPen(Orange);	
		painter.drawText(measure1_rect, Qt::AlignRight | Qt::AlignVCenter,
				tr("Width: ") + bezier_width_);			
		painter.end();
	} else if (measure_end_ != QPointF(0,0)) {
		QPainter p(this);
		p.setPen(QPen(Qt::yellow, 1));
		QLineF l1(measure_start_, measure_end_);
		p.drawLine(l1);
		//arrow
		double x1, y1, x2, y2;
		CalcVertexes(measure_start_.x(), measure_start_.y(), measure_end_.x(), measure_end_.y(), x1, y1, x2, y2);
		p.drawLine(measure_end_.x(), measure_end_.y(), x1, y1); 
		p.drawLine(measure_end_.x(), measure_end_.y(), x2, y2); 
		CalcVertexes(measure_end_.x(), measure_end_.y(), measure_start_.x(), measure_start_.y(), x1, y1, x2, y2);
		p.drawLine(measure_start_.x(), measure_start_.y(), x1, y1); 
		p.drawLine(measure_start_.x(), measure_start_.y(), x2, y2);
		int typical_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
			Qt::AlignLeft | Qt::AlignTop, width_).width();
		typical_width = std::max(typical_width, p.boundingRect(0, 0, INT_MAX, INT_MAX,
			Qt::AlignLeft | Qt::AlignTop, period_).width());
		typical_width = std::max(typical_width, p.boundingRect(0, 0, INT_MAX, INT_MAX,
			Qt::AlignLeft | Qt::AlignTop, freq_).width());
		typical_width = std::max(typical_width, p.boundingRect(0, 0, INT_MAX, INT_MAX,
			Qt::AlignLeft | Qt::AlignTop, duty_cycle_).width());
		typical_width = typical_width + 100;
		const double width = this->width();
		const double height = view_.viewport()->height();
		const double left = view_.hover_point().x();
		const double top = view_.hover_point().y();
		const double right = left + typical_width;
		const double bottom = top + 80;
		QPointF org_pos = QPointF(right > width ? left - typical_width : left, bottom > height ? top - 80 : top);
		QRectF measure_rect = QRectF(org_pos.x(), org_pos.y(), (double)typical_width, 80.0);
		QRectF measure1_rect = QRectF(org_pos.x(), org_pos.y(), (double)typical_width, 20.0);
		QRectF measure2_rect = QRectF(org_pos.x(), org_pos.y() + 20, (double)typical_width, 20.0);
		QRectF measure3_rect = QRectF(org_pos.x(), org_pos.y() + 40, (double)typical_width, 20.0);
		QRectF measure4_rect = QRectF(org_pos.x(), org_pos.y() + 60, (double)typical_width, 20.0);
		p.setBrush(Qt::yellow);
		p.drawEllipse(measure_mid_, 2, 2); 
		p.setPen(Qt::NoPen);
		p.setBrush(LightBlue);
		p.drawRect(measure_rect);
		p.setPen(Orange);
		p.drawText(measure1_rect, Qt::AlignRight | Qt::AlignVCenter,
				tr("Width: ") + width_);
		p.drawText(measure2_rect, Qt::AlignRight | Qt::AlignVCenter,
				tr("Period: ") + period_);
		p.drawText(measure3_rect, Qt::AlignRight | Qt::AlignVCenter,
				tr("Frequency: ") + freq_);
		p.drawText(measure4_rect, Qt::AlignRight | Qt::AlignVCenter,
				tr("Duty Cycle: ") + duty_cycle_);
		p.end();
    }
}

void Viewport::mouseDoubleClickEvent(QMouseEvent *event)
{
	assert(event);

	// if (event->buttons() & Qt::LeftButton)
	// 	view_.zoom(2.0, event->x());
	// else if (event->buttons() & Qt::RightButton)
	// 	view_.zoom(-2.0, event->x());
	bezier_start_ = QPointF(0,0);
	add_rule_flag(event);
}

void Viewport::wheelEvent(QWheelEvent *event)
{
	assert(event);

	int delta = event->delta();

	if (event->orientation() == Qt::Vertical) {
		if (event->modifiers() & Qt::ControlModifier) {
			// Vertical scrolling with the control key pressed
			// is intrepretted as vertical scrolling
			view_.set_v_offset(-view_.owner_visual_v_offset() -
				(delta * height()) / (8 * 120));
		} else {
			// Vertical scrolling is interpreted as zooming in/out
			double steps = delta / 250.0;
			double scale = view_.scale() * pow(2.0, -steps);
			pv::util::Timestamp cursor_offset = view_.offset() + view_.scale() * event->x();
			pv::util::Timestamp new_scale = std::max(std::min(scale, view_.current_max_scale_), 1e-12);
			pv::util::Timestamp new_offset = cursor_offset - new_scale * event->x();
			new_offset = std::max(0.00, std::min((double)(view_.current_max_offset_ - new_scale * view_.viewport_width()), (double)new_offset));

			// if ((double)new_offset + new_scale * view_.viewport_width() > view_.current_max_offset_){
			// 	new_offset = view_.current_max_offset_ - new_scale * view_.viewport_width();
			// }
			// if (new_offset < 0){
			// 	new_offset = 0;
			// }
			view_.set_scale_offset((double)new_scale, new_offset);
		}
	} else if (event->orientation() == Qt::Horizontal) {
		// Horizontal scrolling is interpreted as moving left/right
		view_.set_scale_offset(view_.scale(),
			delta * view_.scale() + view_.offset());
	}
	if (bezier_start_ != QPointF(0,0)) {
		float new_point_x = view_.update_bezier_start();
		bezier_start_.setX(new_point_x);
		bezier_end_ = view_.hover_point();
		SetMeasurePoint(QPointF(0, 0), QPointF(0, 0), QPointF(0, 0));
	} else {
		view_.renew_measure();
	}

	event->accept();
}

void Viewport::on_setting_changed(const QString &key, const QVariant &value)
{
	if (key == GlobalSettings::Key_View_AllowVerticalDragging)
		allow_vertical_dragging_ = value.toBool();
	allow_vertical_dragging_ = false;
}

void Viewport::SetMeasurePoint(QPointF q1, QPointF q2, QPointF q3)
{
	measure_start_ = q1;
	measure_mid_ = q2;
	measure_end_ = q3;
}


} // namespace trace
} // namespace views
} // namespace pv
