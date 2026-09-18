/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2026 Q2H2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <cassert>
#include <algorithm>

#include <QDebug>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVariant>
#include <QScreen>

#include "decoder_channel_dialog.hpp"

#include <pv/binding/decoder.hpp>
#include <pv/data/decodesignal.hpp>
#include <pv/data/decode/decoder.hpp>
#include <pv/data/signalbase.hpp>
#include <pv/session.hpp>

using pv::data::decode::DecodeChannel;
using std::dynamic_pointer_cast;
using std::sort;
using std::vector;

namespace pv
{
	namespace dialogs
	{

		DecoderChannelDialog::DecoderChannelDialog(Session &session,
												   shared_ptr<data::DecodeSignal> signal,
												   const vector<const srd_decoder *> &decoders,
												   QWidget *parent) : QDialog(parent),
																	  session_(session),
																	  signal_(signal),
																	  decoders_(decoders)
		{
			setWindowTitle(tr("Decoder Configuration"));
			setMinimumWidth(400);
			setMinimumHeight(650);
			build_ui();
			populate_channels();
			populate_options();

			if (parent)
				move(parent->window()->geometry().center() - rect().center());
		}

		DecoderChannelDialog::~DecoderChannelDialog()
		{
			qDebug() << "~DecoderChannelDialog: destroying options_group_ before bindings_";
			// options_group_ owns the form widgets created by add_properties_to_form().
			// These widgets hold Property objects whose getter/setter lambdas capture
			// 'this' of binding::Decoder objects stored in bindings_.
			// C++ destroys derived members BEFORE ~QWidget() deletes children.
			// So bindings_ would be destroyed before form widgets, causing use-after-free.
			// Fix: manually delete options_group_ now, while bindings_ is still alive.
			if (options_group_)
			{
				options_group_->setParent(nullptr);
				delete options_group_;
				options_group_ = nullptr;
			}
			qDebug() << "~DecoderChannelDialog: done";
		}

		void DecoderChannelDialog::build_ui()
		{
			root_layout_ = new QVBoxLayout(this);

			// Title
			QString decoder_names;
			for (size_t i = 0; i < decoders_.size(); i++)
			{
				if (i > 0)
					decoder_names += QString::fromUtf8(" \xe2\x86\x92 ");
				decoder_names += QString::fromUtf8(decoders_[i]->name);
			}
			title_label_ = new QLabel(
				tr("<b>%1</b><br/><i>%2</i>")
					.arg(QString::fromUtf8(decoders_.front()->longname),
						 decoder_names));
			title_label_->setWordWrap(true);
			root_layout_->addWidget(title_label_);

			// Scroll area for the rest
			QScrollArea *scroll = new QScrollArea();
			scroll->setWidgetResizable(true);
			QWidget *scroll_content = new QWidget();
			QVBoxLayout *scroll_layout = new QVBoxLayout(scroll_content);

			// --- Channel Mapping ---
			QGroupBox *ch_group = new QGroupBox(tr("Channel Mapping"));
			channel_form_ = new QFormLayout(ch_group);
			scroll_layout->addWidget(ch_group);

			// --- Decoder Options ---
			options_group_ = new QGroupBox(tr("Decoder Options"));
			options_form_ = new QFormLayout(options_group_);
			scroll_layout->addWidget(options_group_);

			scroll_layout->addStretch();
			scroll->setWidget(scroll_content);
			root_layout_->addWidget(scroll);

			// Buttons
			button_box_ = new QDialogButtonBox(
				QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
			button_box_->button(QDialogButtonBox::Ok)->setText(tr("Apply && Decode"));
			button_box_->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
			connect(button_box_, SIGNAL(accepted()), this, SLOT(on_accept()));
			connect(button_box_, SIGNAL(rejected()), this, SLOT(reject()));
			root_layout_->addWidget(button_box_);
		}

		void DecoderChannelDialog::populate_channels()
		{
			const vector<DecodeChannel> all_channels = signal_->get_channels();

			// Build sorted view: non-RX channels first (e.g. UART: TX before RX, so D0=TX, D1=RX)
			vector<const DecodeChannel *> sorted_channels;
			for (const DecodeChannel &ch : all_channels)
				sorted_channels.push_back(&ch);
			sort(sorted_channels.begin(), sorted_channels.end(),
				 [](const DecodeChannel *a, const DecodeChannel *b)
				 {
					 bool a_is_rx = a->name.toLower().contains("rx");
					 bool b_is_rx = b->name.toLower().contains("rx");
					 if (a_is_rx != b_is_rx)
						 return b_is_rx; // non-rx first
					 return a->id < b->id;
				 });

			// Collect available logic signals
			const auto sigs = session_.signalbases();
			vector<shared_ptr<data::SignalBase>> sig_list(sigs.begin(), sigs.end());
			sort(sig_list.begin(), sig_list.end(),
				 [](const shared_ptr<data::SignalBase> &a,
					const shared_ptr<data::SignalBase> &b)
				 {
					 return a->name() < b->name();
				 });

			// Populate combo boxes for each channel (in sorted order)
			for (const DecodeChannel *ch_ptr : sorted_channels)
			{
				const DecodeChannel &ch = *ch_ptr;
				bool belongs_to_our_decoder = false;
				for (const srd_decoder *d : decoders_)
				{
					if (ch.decoder_ && ch.decoder_->get_srd_decoder() == d)
					{
						belongs_to_our_decoder = true;
						break;
					}
				}
				if (!belongs_to_our_decoder)
					continue;

				QComboBox *combo = new QComboBox(this);
				combo->addItem(tr("-- Not assigned --"), QVariant::fromValue((quintptr)0));

				int best_match_idx = 0;
				bool has_auto_match = false;

				for (size_t i = 0; i < sig_list.size(); i++)
				{
					const auto &s = sig_list[i];
					if (s->logic_data() && s->enabled())
					{
						combo->addItem(s->name(), QVariant::fromValue(
													  (quintptr)(i + 1)));

						QString ch_name = ch.name.toLower();
						QString s_name = s->name().toLower();
						if (!has_auto_match &&
							(ch_name.contains(s_name) || s_name.contains(ch_name)))
						{
							best_match_idx = combo->count() - 1;
							has_auto_match = true;
						}
					}
				}

				if (!has_auto_match && sig_list.size() > 0)
				{
					int seq_idx = 1;
					for (const DecodeChannel &other_ch : all_channels)
					{
						if (other_ch.id >= ch.id)
							break;
						seq_idx++;
					}
					if (seq_idx < combo->count())
						best_match_idx = seq_idx;
				}

				combo->setCurrentIndex(best_match_idx);

				const QString required_flag = ch.is_optional ? QString() : QString(" *");
				channel_form_->addRow(
					tr("<b>%1</b> (%2)%3")
						.arg(ch.name, ch.desc, required_flag),
					combo);

				channel_combos_[ch.id] = combo;
			}
		}

		void DecoderChannelDialog::populate_options()
		{
			// Get decoder stack from the signal
			const vector<shared_ptr<data::decode::Decoder>> &stack =
				signal_->decoder_stack();

			// Create bindings for each decoder to expose its options
			for (const shared_ptr<data::decode::Decoder> &dec : stack)
			{
				// Only show options for decoders we just added
				bool is_ours = false;
				for (const srd_decoder *d : decoders_)
				{
					if (dec->get_srd_decoder() == d)
					{
						is_ours = true;
						break;
					}
				}
				if (!is_ours)
					continue;

				shared_ptr<binding::Decoder> binding(
					new binding::Decoder(signal_, dec));
				binding->add_properties_to_form(options_form_, true);
				bindings_.push_back(binding);
			}
		}

		void DecoderChannelDialog::on_accept()
		{
			qDebug() << "DecoderChannelDialog::on_accept() start";
			// Build signal list (same order as populate_channels)
			vector<shared_ptr<data::SignalBase>> sig_list;
			for (const auto &s : session_.signalbases())
				sig_list.push_back(s);
			sort(sig_list.begin(), sig_list.end(),
				 [](const shared_ptr<data::SignalBase> &a,
					const shared_ptr<data::SignalBase> &b)
				 {
					 return a->name() < b->name();
				 });

			for (const auto &pair : channel_combos_)
			{
				uint16_t channel_id = pair.first;
				QComboBox *combo = pair.second;

				size_t idx = combo->currentData(Qt::UserRole).value<quintptr>();
				if (idx > 0 && idx <= sig_list.size())
				{
					qDebug() << "  assign_signal channel" << channel_id << "->" << sig_list[idx - 1]->name();
					signal_->assign_signal(channel_id, sig_list[idx - 1]);
				}
			}

			qDebug() << "DecoderChannelDialog::on_accept() calling accept()";
			accept();
			qDebug() << "DecoderChannelDialog::on_accept() after accept()";
		}

	} // namespace dialogs
} // namespace pv
