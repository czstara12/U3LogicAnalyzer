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

#ifndef PULSEVIEW_PV_DIALOGS_DECODER_CHANNEL_DIALOG_HPP
#define PULSEVIEW_PV_DIALOGS_DECODER_CHANNEL_DIALOG_HPP

#include <map>
#include <memory>
#include <vector>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <libsigrokdecode/libsigrokdecode.h>

using std::map;
using std::shared_ptr;
using std::vector;

namespace pv {

class Session;

namespace binding {
class Decoder;
}

namespace data {
class DecodeSignal;
class SignalBase;
namespace decode {
class Decoder;
}
}

namespace dialogs {

class DecoderChannelDialog : public QDialog
{
	Q_OBJECT

public:
	DecoderChannelDialog(Session &session,
		shared_ptr<data::DecodeSignal> signal,
		const vector<const srd_decoder*> &decoders,
		QWidget *parent = nullptr);
	~DecoderChannelDialog();

private:
	void build_ui();
	void populate_channels();
	void populate_options();

private Q_SLOTS:
	void on_accept();

private:
	Session &session_;
	shared_ptr<data::DecodeSignal> signal_;
	vector<const srd_decoder*> decoders_;

	QVBoxLayout *root_layout_;
	QLabel *title_label_;
	QFormLayout *channel_form_;
	QGroupBox *options_group_;
	QFormLayout *options_form_;
	QDialogButtonBox *button_box_;

	// channel_id -> combo box mapping
	map<uint16_t, QComboBox*> channel_combos_;

	// Keep bindings alive for option getters/setters
	vector<shared_ptr<binding::Decoder>> bindings_;
};

} // namespace dialogs
} // namespace pv

#endif // PULSEVIEW_PV_DIALOGS_DECODER_CHANNEL_DIALOG_HPP
