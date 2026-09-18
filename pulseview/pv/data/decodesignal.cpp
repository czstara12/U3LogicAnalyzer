/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnalyzer is based on PulseView.
 *
 * Copyright (C) 2017 Soeren Apel <soeren@apelpie.net>
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

#include <cstring>
#include <forward_list>
#include <limits>
#include <unistd.h>
#include <QDebug>
#include <QMessageBox>
#include "logic.hpp"
#include "logicsegment.hpp"
#include "decodesignal.hpp"
#include "signaldata.hpp"
#include <time.h>
#include <pv/data/decode/decoder.hpp>
#include <pv/data/decode/row.hpp>
#include <pv/globalsettings.hpp>
#include <pv/session.hpp>
#include <fstream>
#include <iostream>
#include <QTime>
#include <chrono>
#include <cinttypes>
using std::dynamic_pointer_cast;
using std::lock_guard;
using std::make_shared;
using std::min;
using std::out_of_range;
using std::shared_ptr;
using std::unique_lock;
using pv::data::decode::AnnotationClass;
using pv::data::decode::DecodeChannel;
using namespace std;
namespace pv {
namespace data {

const double DecodeSignal::DecodeMargin = 1.0;
const double DecodeSignal::DecodeThreshold = 0.2;
// const uint64_t DecodeSignal::DecodeChunkLength = 50 * 1024 * 1024;
using std::bad_alloc;

DecodeSignal::DecodeSignal(pv::Session &session) :
	SignalBase(nullptr, SignalBase::DecodeChannel),
	session_(session),
	srd_session_(nullptr),
	logic_mux_data_invalid_(false),
	stack_config_changed_(true),
	current_segment_id_(0)
{

	DecodeChunkLength = 50 * 1024 * 1024;

	connect(&session_, SIGNAL(capture_state_changed(int)),
		this, SLOT(on_capture_state_changed(int)));
	decode_chunk_ = NULL;
	src_buffer_ = NULL;
	src_buffer_unit_size_ = 0;
}

DecodeSignal::~DecodeSignal()
{
	reset_decode(true);
	if (decode_chunk_ != NULL)
		delete[] decode_chunk_;
	if (src_buffer_ != NULL)
		delete[] src_buffer_;
}

void DecodeSignal::set_name(QString name)
{
	SignalBase::set_name(name);

	update_output_signals();
}

void DecodeSignal::set_color(QColor color)
{
	SignalBase::set_color(color);

	update_output_signals();
}

const vector< shared_ptr<Decoder> >& DecodeSignal::decoder_stack() const
{
	return stack_;
}

void DecodeSignal::stack_decoder(const srd_decoder *decoder, bool restart_decode)
{
	assert(decoder);
	
	// Set name if this decoder is the first in the list or the name is unchanged
	const srd_decoder* prev_dec = stack_.empty() ? nullptr : stack_.back()->get_srd_decoder();
	const QString prev_dec_name = prev_dec ? QString::fromUtf8(prev_dec->name) : QString();

	if ((stack_.empty()) || ((stack_.size() > 0) && (name() == prev_dec_name)))
		set_name(QString::fromUtf8(decoder->name));

	const shared_ptr<Decoder> dec = make_shared<Decoder>(decoder, stack_.size());
	stack_.push_back(dec);

	connect(dec.get(), SIGNAL(annotation_visibility_changed()),
		this, SLOT(on_annotation_visibility_changed()));

	// Include the newly created decode channels in the channel lists
	update_channel_list();

	stack_config_changed_ = true;
	auto_assign_signals(dec);
	commit_decoder_channels();
	update_output_signals();

	decoder_stacked((void*)dec.get());

	if (restart_decode)
		begin_decode();
}

void DecodeSignal::remove_decoder(int index)
{
	assert(index >= 0);
	assert(index < (int)stack_.size());

	// Find the decoder in the stack
	auto iter = stack_.begin() + index;
	assert(iter != stack_.end());

	shared_ptr<Decoder> dec = *iter;

	decoder_removed(dec.get());

	// Delete the element
	stack_.erase(iter);

	// Restore original signal names for both output and input signals
	for (shared_ptr<SignalBase> s : output_signals_) {
		if (!s->internal_name().isEmpty())
			s->set_name(s->internal_name());
	}
	// Also restore names of assigned input signals (decoder channels → D0, D1...)
	for (decode::DecodeChannel& ch : channels_) {
		if (ch.assigned_signal && !ch.assigned_signal->internal_name().isEmpty())
			std::const_pointer_cast<SignalBase>(ch.assigned_signal)->set_name(
				ch.assigned_signal->internal_name());
	}

	// Update channels and decoded data
	stack_config_changed_ = true;
	update_channel_list();
	begin_decode();
}

bool DecodeSignal::toggle_decoder_visibility(int index)
{
	auto iter = stack_.cbegin();
	for (int i = 0; i < index; i++, iter++)
		assert(iter != stack_.end());

	shared_ptr<Decoder> dec = *iter;

	// Toggle decoder visibility
	bool state = false;
	if (dec) {
		state = !dec->visible();
		dec->set_visible(state);
	}
	return state;
}

void DecodeSignal::reset_decode(bool shutting_down)
{
	resume_decode();  // Make sure the decode thread isn't blocked by pausing

	if (stack_config_changed_ || shutting_down)
		stop_srd_session();
	else
		terminate_srd_session();

	if (decode_thread_.joinable()) {
		decode_interrupt_ = true;
		decode_input_cond_.notify_one();
		decode_thread_.join();
	}

	if (logic_mux_thread_.joinable()) {
		logic_mux_interrupt_ = true;
		logic_mux_cond_.notify_one();
		logic_mux_thread_.join();
	}

	current_segment_id_ = 0;
	segments_.clear();

	for (const shared_ptr<decode::Decoder>& dec : stack_)
		if (dec->has_logic_output())
			output_logic_[dec->get_srd_decoder()]->clear();

	logic_mux_data_.reset();
	logic_mux_data_invalid_ = true;
	if (!error_message_.isEmpty()) {
		error_message_.clear();
		// TODO Emulate noquote()
		qDebug().nospace() << name() << ": Error cleared";
	}
	decode_reset();
}

void DecodeSignal::begin_decode()
{
	if (decode_thread_.joinable()) {
		decode_interrupt_ = true;
		decode_input_cond_.notify_one();
		decode_thread_.join();
	}

	if (logic_mux_thread_.joinable()) {
		logic_mux_interrupt_ = true;
		logic_mux_cond_.notify_one();
		logic_mux_thread_.join();
	}

	reset_decode();

	if (stack_.size() == 0) {
		set_error_message(tr("No decoders"));
		return;
	}

	assert(channels_.size() > 0);

	if (get_assigned_signal_count() == 0) {
		set_error_message(tr("There are no channels assigned to this decoder"));
		return;
	}

	// Make sure that all assigned channels still provide logic data
	// (can happen when a converted signal was assigned but the
	// conversion removed in the meanwhile)
	for (decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal && !(ch.assigned_signal->logic_data() != nullptr))
			ch.assigned_signal = nullptr;

	// Check that all decoders have the required channels
	for (const shared_ptr<Decoder>& dec : stack_)
		if (!dec->have_required_channels()) {
			set_error_message(tr("One or more required channels "
				"have not been specified"));
			return;
		}
	
	// Free the logic data and its segment(s) if it needs to be updated
	if (logic_mux_data_invalid_)
		logic_mux_data_.reset();

	if (!logic_mux_data_) {
		const uint32_t ch_count = get_assigned_signal_count();
		logic_mux_unit_size_ = (ch_count + 7) / 8;
		logic_mux_data_ = make_shared<Logic>(ch_count);
	}

	if (get_input_segment_count() == 0)
		set_error_message(tr("No input data"));

	// Make sure the logic output data is complete and up-to-date
	logic_mux_interrupt_ = false;
	logic_mux_thread_ = std::thread(&DecodeSignal::logic_mux_proc, this);

	// Decode the muxed logic data
	decode_interrupt_ = false;
	decode_thread_ = std::thread(&DecodeSignal::decode_proc, this);
	// allow_skip_ = true;
}

void DecodeSignal::pause_decode()
{
	decode_paused_ = true;
}

void DecodeSignal::resume_decode()
{
	// Manual unlocking is done before notifying, to avoid waking up the
	// waiting thread only to block again (see notify_one for details)
	decode_pause_mutex_.unlock();
	decode_pause_cond_.notify_one();
	decode_paused_ = false;
}

bool DecodeSignal::is_paused() const
{
	return decode_paused_;
}

const vector<decode::DecodeChannel> DecodeSignal::get_channels() const
{
	return channels_;
}

void DecodeSignal::auto_assign_signals(const shared_ptr<Decoder> dec)
{
	bool new_assignment = false;

	// Disconnect all input signal notifications so we don't have duplicate connections
	disconnect_input_notifiers();

	// Sort channels: non-RX first (e.g. UART: TX→D0, RX→D1)
	vector<decode::DecodeChannel*> sorted_channels;
	for (decode::DecodeChannel& ch : channels_)
		sorted_channels.push_back(&ch);
	sort(sorted_channels.begin(), sorted_channels.end(),
		[](const decode::DecodeChannel* a, const decode::DecodeChannel* b) {
			bool a_is_rx = a->name.toLower().contains("rx");
			bool b_is_rx = b->name.toLower().contains("rx");
			if (a_is_rx != b_is_rx) return b_is_rx;
			return a->id < b->id;
		});

	// Try to auto-select channels that don't have signals assigned yet
	for (decode::DecodeChannel* ch_ptr : sorted_channels) {
		decode::DecodeChannel& ch = *ch_ptr;
		// If a decoder is given, auto-assign only its channels
		if (dec && (ch.decoder_ != dec))
			continue;

		if (ch.assigned_signal)
			continue;

		QString ch_name = ch.name.toLower();
		ch_name = ch_name.replace(QRegExp("[-_.]"), " ");

		shared_ptr<data::SignalBase> match;
		for (const shared_ptr<data::SignalBase>& s : session_.signalbases()) {
			if (!s->enabled())
				continue;

			QString s_name = s->name().toLower();
			s_name = s_name.replace(QRegExp("[-_.]"), " ");

			if (s->logic_data() &&
				((ch_name.contains(s_name)) || (s_name.contains(ch_name)))) {
				if (!match)
					match = s;
				else {
					// Only replace an existing match if it matches more characters
					int old_unmatched = ch_name.length() - match->name().length();
					int new_unmatched = ch_name.length() - s->name().length();
					if (abs(new_unmatched) < abs(old_unmatched))
						match = s;
				}
			}
		}

		// Prevent using a signal more than once as D1 would match e.g. D1 and D10
		bool signal_not_already_used = true;
		for (decode::DecodeChannel& ch : channels_)
			if (ch.assigned_signal && (ch.assigned_signal == match))
				signal_not_already_used = false;

		if (match && signal_not_already_used) {
			ch.assigned_signal = match;
			new_assignment = true;
		}
	}

	if (new_assignment) {
		// Receive notifications when new sample data is available
		connect_input_notifiers();

		logic_mux_data_invalid_ = true;
		stack_config_changed_ = true;
		commit_decoder_channels();
		channels_updated();
	}
}

void DecodeSignal::assign_signal(const uint16_t channel_id, shared_ptr<const SignalBase> signal)
{
	// Disconnect all input signal notifications so we don't have duplicate connections
	disconnect_input_notifiers();

	for (decode::DecodeChannel& ch : channels_)
		if (ch.id == channel_id) {
			ch.assigned_signal = signal;
			logic_mux_data_invalid_ = true;
		}

	// Receive notifications when new sample data is available
	connect_input_notifiers();

	stack_config_changed_ = true;
	commit_decoder_channels();
	channels_updated();
	begin_decode();
}

void DecodeSignal::assign_signal_no_decode(const uint16_t channel_id, shared_ptr<const SignalBase> signal)
{
	// Disconnect all input signal notifications so we don't have duplicate connections
	disconnect_input_notifiers();

	for (decode::DecodeChannel& ch : channels_)
		if (ch.id == channel_id) {
			ch.assigned_signal = signal;
			logic_mux_data_invalid_ = true;
		}

	// Receive notifications when new sample data is available
	connect_input_notifiers();

	stack_config_changed_ = true;
	commit_decoder_channels();
	channels_updated();
	// Note: begin_decode() is not called here, it will be called when data arrives
}

int DecodeSignal::get_assigned_signal_count() const
{
	// Count all channels that have a signal assigned to them
	return count_if(channels_.begin(), channels_.end(),
		[](decode::DecodeChannel ch) { return ch.assigned_signal.get(); });
}

void DecodeSignal::update_output_signals()
{
	for (const shared_ptr<decode::Decoder>& dec : stack_) {
		assert(dec);

		if (dec->has_logic_output()) {
			const vector<decode::DecoderLogicOutputChannel> logic_channels =
				dec->logic_output_channels();

			// All signals of a decoder share the same LogicSegment, so it's
			// sufficient to check for only the first channel
			const decode::DecoderLogicOutputChannel& first_ch = logic_channels[0];

			bool ch_exists = false;
			for (const shared_ptr<SignalBase>& signal : output_signals_)
				if (signal->internal_name() == first_ch.id)
					ch_exists = true;

			if (!ch_exists) {
				shared_ptr<Logic> logic_data = make_shared<Logic>(logic_channels.size());
				logic_data->set_samplerate(get_samplerate());
				output_logic_[dec->get_srd_decoder()] = logic_data;
				output_logic_muxed_data_[dec->get_srd_decoder()] = vector<uint8_t>();

				shared_ptr<LogicSegment> logic_segment = make_shared<data::LogicSegment>(
					*logic_data, 0, (logic_data->num_channels() + 7) / 8, get_samplerate());
				logic_data->push_segment(logic_segment);

				uint index = 0;
				for (const decode::DecoderLogicOutputChannel& logic_ch : logic_channels) {
					shared_ptr<data::SignalBase> signal =
						make_shared<data::SignalBase>(nullptr, LogicChannel);
					signal->set_internal_name(logic_ch.id);
					signal->set_index(index);
					signal->set_data(logic_data);
					output_signals_.push_back(signal);
					session_.add_generated_signal(signal);
					index++;
				}
			} else {
				shared_ptr<Logic> logic_data = output_logic_[dec->get_srd_decoder()];
				logic_data->set_samplerate(get_samplerate());
				for (shared_ptr<LogicSegment>& segment : logic_data->logic_segments())
					segment->set_samplerate(get_samplerate());
			}
		}
	}

	for (shared_ptr<SignalBase> s : output_signals_) {
		s->set_name(s->internal_name() + " (" + name() + ")");
		s->set_color(color());
	}

	// TODO Assert that all sample rates are the same as the session's
}

void DecodeSignal::set_initial_pin_state(const uint16_t channel_id, const int init_state)
{
	for (decode::DecodeChannel& ch : channels_)
		if (ch.id == channel_id)
			ch.initial_pin_state = init_state;

	stack_config_changed_ = true;
	channels_updated();
	begin_decode();
}

double DecodeSignal::get_samplerate() const
{
	double result = 0;

	// TODO For now, we simply return the first samplerate that we have
	if (segments_.size() > 0)
		result = segments_.front().samplerate;

	return result;
}

const pv::util::Timestamp DecodeSignal::start_time() const
{
	pv::util::Timestamp result;

	// TODO For now, we simply return the first start time that we have
	if (segments_.size() > 0)
		result = segments_.front().start_time;

	return result;
}

int64_t DecodeSignal::get_working_sample_count(uint32_t segment_id) const
{
	// The working sample count is the highest sample number for
	// which all used signals have data available, so go through all
	// channels and use the lowest overall sample count of the segment

	int64_t count = std::numeric_limits<int64_t>::max();
	bool no_signals_assigned = true;

	for (const decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal) {
			if (!ch.assigned_signal->logic_data())
				return 0;

			no_signals_assigned = false;

			const shared_ptr<Logic> logic_data = ch.assigned_signal->logic_data();
			if (logic_data->logic_segments().empty())
				return 0;

			if (segment_id >= logic_data->logic_segments().size())
				return 0;

			const shared_ptr<const LogicSegment> segment = logic_data->logic_segments()[segment_id]->get_shared_ptr();
			if (segment)
				count = min(count, (int64_t)segment->get_sample_count());
		}

	return (no_signals_assigned ? 0 : count);
}

int64_t DecodeSignal::get_decoded_sample_count(uint32_t segment_id,
	bool include_processing) const
{
	lock_guard<mutex> decode_lock(output_mutex_);

	int64_t result = 0;

	if (segment_id >= segments_.size())
		return result;

	if (include_processing)
		result = segments_[segment_id].samples_decoded_incl;
	else
		result = segments_[segment_id].samples_decoded_excl;

	return result;
}

vector<Row*> DecodeSignal::get_rows(bool visible_only)
{
	vector<Row*> rows;

	for (const shared_ptr<Decoder>& dec : stack_) {
		assert(dec);
		if (visible_only && !dec->visible())
			continue;

		for (Row* row : dec->get_rows())
			rows.push_back(row);
	}

	return rows;
}

vector<const Row*> DecodeSignal::get_rows(bool visible_only) const
{
	vector<const Row*> rows;

	for (const shared_ptr<Decoder>& dec : stack_) {
		assert(dec);
		if (visible_only && !dec->visible())
			continue;

		for (const Row* row : dec->get_rows())
			rows.push_back(row);
	}

	return rows;
}

uint64_t DecodeSignal::get_annotation_count(const Row* row, uint32_t segment_id) const
{
	if (segment_id >= segments_.size())
		return 0;

	const DecodeSegment* segment = &(segments_.at(segment_id));

	auto row_it = segment->annotation_rows.find(row);

	const RowData* rd;
	if (row_it == segment->annotation_rows.end())
		return 0;
	else
		rd = &(row_it->second);

	return rd->get_annotation_count();
}

void DecodeSignal::get_annotation_subset(deque<const Annotation*> &dest,
	const Row* row, uint32_t segment_id, uint64_t start_sample,
	uint64_t end_sample) const
{
	lock_guard<mutex> lock(output_mutex_);

	if (segment_id >= segments_.size())
		return;

	const DecodeSegment* segment = &(segments_.at(segment_id));

	auto row_it = segment->annotation_rows.find(row);

	const RowData* rd;
	if (row_it == segment->annotation_rows.end())
		return;
	else
		rd = &(row_it->second);

	rd->get_annotation_subset(dest, start_sample, end_sample);
}

void DecodeSignal::get_annotation_subset(deque<const Annotation*> &dest,
	uint32_t segment_id, uint64_t start_sample, uint64_t end_sample) const
{
	for (const Row* row : get_rows())
		get_annotation_subset(dest, row, segment_id, start_sample, end_sample);
}

uint32_t DecodeSignal::get_binary_data_chunk_count(uint32_t segment_id,
	const Decoder* dec, uint32_t bin_class_id) const
{
	if ((segments_.size() == 0) || (segment_id >= segments_.size()))
		return 0;

	const DecodeSegment *segment = &(segments_[segment_id]);

	for (const DecodeBinaryClass& bc : segment->binary_classes)
		if ((bc.decoder == dec) && (bc.info->bin_class_id == bin_class_id))
			return bc.chunks.size();

	return 0;
}

void DecodeSignal::get_binary_data_chunk(uint32_t segment_id,
	const  Decoder* dec, uint32_t bin_class_id, uint32_t chunk_id,
	const vector<uint8_t> **dest, uint64_t *size)
{
	if (segment_id >= segments_.size())
		return;

	const DecodeSegment *segment = &(segments_[segment_id]);

	for (const DecodeBinaryClass& bc : segment->binary_classes)
		if ((bc.decoder == dec) && (bc.info->bin_class_id == bin_class_id)) {
			if (dest) *dest = &(bc.chunks.at(chunk_id).data);
			if (size) *size = bc.chunks.at(chunk_id).data.size();
			return;
		}
}

void DecodeSignal::get_merged_binary_data_chunks_by_sample(uint32_t segment_id,
	const Decoder* dec, uint32_t bin_class_id, uint64_t start_sample,
	uint64_t end_sample, vector<uint8_t> *dest) const
{
	assert(dest != nullptr);

	if (segment_id >= segments_.size())
		return;

	const DecodeSegment *segment = &(segments_[segment_id]);

	const DecodeBinaryClass* bin_class = nullptr;
	for (const DecodeBinaryClass& bc : segment->binary_classes)
		if ((bc.decoder == dec) && (bc.info->bin_class_id == bin_class_id))
			bin_class = &bc;

	// Determine overall size before copying to resize dest vector only once
	uint64_t size = 0;
	uint64_t matches = 0;
	for (const DecodeBinaryDataChunk& chunk : bin_class->chunks)
		if ((chunk.sample >= start_sample) && (chunk.sample < end_sample)) {
			size += chunk.data.size();
			matches++;
		}
	dest->resize(size);

	uint64_t offset = 0;
	uint64_t matches2 = 0;
	for (const DecodeBinaryDataChunk& chunk : bin_class->chunks)
		if ((chunk.sample >= start_sample) && (chunk.sample < end_sample)) {
			memcpy(dest->data() + offset, chunk.data.data(), chunk.data.size());
			offset += chunk.data.size();
			matches2++;

			// Make sure we don't overwrite memory if the array grew in the meanwhile
			if (matches2 == matches)
				break;
		}
}

void DecodeSignal::get_merged_binary_data_chunks_by_offset(uint32_t segment_id,
	const Decoder* dec, uint32_t bin_class_id, uint64_t start, uint64_t end,
	vector<uint8_t> *dest) const
{
	assert(dest != nullptr);

	if (segment_id >= segments_.size())
		return;

	const DecodeSegment *segment = &(segments_[segment_id]);

	const DecodeBinaryClass* bin_class = nullptr;
	for (const DecodeBinaryClass& bc : segment->binary_classes)
		if ((bc.decoder == dec) && (bc.info->bin_class_id == bin_class_id))
			bin_class = &bc;

	// Determine overall size before copying to resize dest vector only once
	uint64_t size = 0;
	uint64_t offset = 0;
	for (const DecodeBinaryDataChunk& chunk : bin_class->chunks) {
		if (offset >= start)
			size += chunk.data.size();
		offset += chunk.data.size();
		if (offset >= end)
			break;
	}
	dest->resize(size);

	offset = 0;
	uint64_t dest_offset = 0;
	for (const DecodeBinaryDataChunk& chunk : bin_class->chunks) {
		if (offset >= start) {
			memcpy(dest->data() + dest_offset, chunk.data.data(), chunk.data.size());
			dest_offset += chunk.data.size();
		}
		offset += chunk.data.size();
		if (offset >= end)
			break;
	}
}

const DecodeBinaryClass* DecodeSignal::get_binary_data_class(uint32_t segment_id,
	const Decoder* dec, uint32_t bin_class_id) const
{
	if (segment_id >= segments_.size())
		return nullptr;

	const DecodeSegment *segment = &(segments_[segment_id]);

	for (const DecodeBinaryClass& bc : segment->binary_classes)
		if ((bc.decoder == dec) && (bc.info->bin_class_id == bin_class_id))
			return &bc;

	return nullptr;
}

const deque<const Annotation*>* DecodeSignal::get_all_annotations_by_segment(
	uint32_t segment_id) const
{
	if (segment_id >= segments_.size())
		return nullptr;

	const DecodeSegment *segment = &(segments_[segment_id]);

	return &(segment->all_annotations);
}

void DecodeSignal::save_settings(QSettings &settings) const
{
	SignalBase::save_settings(settings);

	settings.setValue("decoders", (int)(stack_.size()));

	// Save decoder stack
	int decoder_idx = 0;
	for (const shared_ptr<Decoder>& decoder : stack_) {
		settings.beginGroup("decoder" + QString::number(decoder_idx++));

		settings.setValue("id", decoder->get_srd_decoder()->id);
		settings.setValue("visible", decoder->visible());

		// Save decoder options
		const map<string, GVariant*>& options = decoder->options();

		settings.setValue("options", (int)options.size());

		// Note: Decoder::options() returns only the options
		// that differ from the default. See binding::Decoder::getter()
		int i = 0;
		for (auto& option : options) {
			settings.beginGroup("option" + QString::number(i));
			settings.setValue("name", QString::fromStdString(option.first));
			GlobalSettings::store_gvariant(settings, option.second);
			settings.endGroup();
			i++;
		}

		// Save row properties
		i = 0;
		for (const Row* row : decoder->get_rows()) {
			settings.beginGroup("row" + QString::number(i));
			settings.setValue("visible", row->visible());
			settings.endGroup();
			i++;
		}

		// Save class properties
		i = 0;
		for (const AnnotationClass* ann_class : decoder->ann_classes()) {
			settings.beginGroup("ann_class" + QString::number(i));
			settings.setValue("visible", ann_class->visible());
			settings.endGroup();
			i++;
		}

		settings.endGroup();
	}

	// Save channel mapping
	settings.setValue("channels", (int)channels_.size());

	for (unsigned int channel_id = 0; channel_id < channels_.size(); channel_id++) {
		auto channel = find_if(channels_.begin(), channels_.end(),
			[&](decode::DecodeChannel ch) { return ch.id == channel_id; });

		if (channel == channels_.end()) {
			qDebug() << "ERROR: Gap in channel index:" << channel_id;
			continue;
		}

		settings.beginGroup("channel" + QString::number(channel_id));

		settings.setValue("name", channel->name);  // Useful for debugging
		settings.setValue("initial_pin_state", channel->initial_pin_state);

		if (channel->assigned_signal)
			settings.setValue("assigned_signal_name", channel->assigned_signal->name());

		settings.endGroup();
	}

	// TODO Save logic output signal settings
}

void DecodeSignal::restore_settings(QSettings &settings)
{
	SignalBase::restore_settings(settings);

	// Restore decoder stack
	GSList *dec_list = g_slist_copy((GSList*)srd_decoder_list());

	int decoders = settings.value("decoders").toInt();

	for (int decoder_idx = 0; decoder_idx < decoders; decoder_idx++) {
		settings.beginGroup("decoder" + QString::number(decoder_idx));

		QString id = settings.value("id").toString();

		for (GSList *entry = dec_list; entry; entry = entry->next) {
			const srd_decoder *dec = (srd_decoder*)entry->data;
			if (!dec)
				continue;

			if (QString::fromUtf8(dec->id) == id) {
				shared_ptr<Decoder> decoder = make_shared<Decoder>(dec, stack_.size());

				connect(decoder.get(), SIGNAL(annotation_visibility_changed()),
					this, SLOT(on_annotation_visibility_changed()));

				stack_.push_back(decoder);
				decoder->set_visible(settings.value("visible", true).toBool());

				// Restore decoder options that differ from their default
				int options = settings.value("options").toInt();

				for (int i = 0; i < options; i++) {
					settings.beginGroup("option" + QString::number(i));
					QString name = settings.value("name").toString();
					qDebug() << "Setting option : name ==  "  << name;
					GVariant *value = GlobalSettings::restore_gvariant(settings);
					decoder->set_option(name.toUtf8(), value);
					settings.endGroup();
				}

				// Include the newly created decode channels in the channel lists
				update_channel_list();

				// Restore row properties
				int i = 0;
				for (Row* row : decoder->get_rows()) {
					settings.beginGroup("row" + QString::number(i));
					row->set_visible(settings.value("visible", true).toBool());
					settings.endGroup();
					i++;
				}

				// Restore class properties
				i = 0;
				for (AnnotationClass* ann_class : decoder->ann_classes()) {
					settings.beginGroup("ann_class" + QString::number(i));
					ann_class->set_visible(settings.value("visible", true).toBool());
					settings.endGroup();
					i++;
				}
				break;
			}
		}
		settings.endGroup();
		channels_updated();
	}

	// Restore channel mapping
	unsigned int channels = settings.value("channels").toInt();

	const vector< shared_ptr<data::SignalBase> > signalbases =
		session_.signalbases();

	for (unsigned int channel_id = 0; channel_id < channels; channel_id++) {
		auto channel = find_if(channels_.begin(), channels_.end(),
			[&](decode::DecodeChannel ch) { return ch.id == channel_id; });

		if (channel == channels_.end()) {
			qDebug() << "ERROR: Non-existant channel index:" << channel_id;
			continue;
		}

		settings.beginGroup("channel" + QString::number(channel_id));

		QString assigned_signal_name = settings.value("assigned_signal_name").toString();

		for (const shared_ptr<data::SignalBase>& signal : signalbases)
			if ((signal->name() == assigned_signal_name) && (signal->type() != SignalBase::DecodeChannel))
				channel->assigned_signal = signal;

		channel->initial_pin_state = settings.value("initial_pin_state").toInt();

		settings.endGroup();
	}

	connect_input_notifiers();

	// Update the internal structures
	stack_config_changed_ = true;
	update_channel_list();
	commit_decoder_channels();
	update_output_signals();

	// TODO Restore logic output signal settings

	begin_decode();
}

bool DecodeSignal::all_input_segments_complete(uint32_t segment_id) const
{
	bool all_complete = true;

	for (const decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal) {
			if (!ch.assigned_signal->logic_data())
				continue;

			const shared_ptr<Logic> logic_data = ch.assigned_signal->logic_data();
			if (logic_data->logic_segments().empty())
				return false;

			if (segment_id >= logic_data->logic_segments().size())
				return false;

			const shared_ptr<const LogicSegment> segment = logic_data->logic_segments()[segment_id]->get_shared_ptr();
			if (segment && !segment->is_complete())
				all_complete = false;
		}

	return all_complete;
}

uint32_t DecodeSignal::get_input_segment_count() const
{
	uint64_t count = std::numeric_limits<uint64_t>::max();
	bool no_signals_assigned = true;

	for (const decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal) {
			no_signals_assigned = false;

			const shared_ptr<Logic> logic_data = ch.assigned_signal->logic_data();
			if (!logic_data || logic_data->logic_segments().empty())
				return 0;

			// Find the min value of all segment counts
			if ((uint64_t)(logic_data->logic_segments().size()) < count)
				count = logic_data->logic_segments().size();
		}

	return (no_signals_assigned ? 0 : count);
}

double DecodeSignal::get_input_samplerate(uint32_t segment_id) const
{
	double samplerate = 0;

	for (const decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal) {
			const shared_ptr<Logic> logic_data = ch.assigned_signal->logic_data();
			if (!logic_data || logic_data->logic_segments().empty())
				continue;

			try {
				const shared_ptr<const LogicSegment> segment =
					logic_data->logic_segments().at(segment_id)->get_shared_ptr();
				if (segment)
					samplerate = segment->samplerate();
			} catch (out_of_range&) {
				// Do nothing
			}
			break;
		}
	return samplerate;
}

uint16_t DecodeSignal::get_input_channel_number(uint32_t segment_id)
{
	uint16_t channel_num = 0;

	for (const decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal) {
			const shared_ptr<Logic> logic_data = ch.assigned_signal->logic_data();
			if (!logic_data || logic_data->logic_segments().empty())
				continue;

			try {
				const shared_ptr<const LogicSegment> segment =
					logic_data->logic_segments().at(segment_id)->get_shared_ptr();
				if (segment){
					// channel_num = (segment->channel_num_ < 8) ? 8 : segment->channel_num_;
					channel_num = segment->channel_num_;
				}
			} catch (out_of_range&) {
				// Do nothing
			}
			break;
		}

	return channel_num;
}

uint16_t DecodeSignal::get_input_unit_size(uint32_t segment_id)
{
	uint16_t unit_size = 0;

	for (const decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal) {
			const shared_ptr<Logic> logic_data = ch.assigned_signal->logic_data();
			if (!logic_data || logic_data->logic_segments().empty())
				continue;

			try {
				const shared_ptr<const LogicSegment> segment =
					logic_data->logic_segments().at(segment_id)->get_shared_ptr();
				if (segment){
					unit_size = segment->unit_size();
				}
			} catch (out_of_range&) {
				// Do nothing
			}
			break;
		}

	return unit_size;
}

Decoder* DecodeSignal::get_decoder_by_instance(const srd_decoder *const srd_dec)
{
	for (shared_ptr<Decoder>& d : stack_)
		if (d->get_srd_decoder() == srd_dec)
			return d.get();

	return nullptr;
}

void DecodeSignal::update_channel_list()
{
	vector<decode::DecodeChannel> prev_channels = channels_;
	channels_.clear();

	uint16_t id = 0;

	// Copy existing entries, create new as needed
	for (shared_ptr<Decoder>& decoder : stack_) {
		const srd_decoder* srd_dec = decoder->get_srd_decoder();
		const GSList *l;

		// Mandatory channels
		for (l = srd_dec->channels; l; l = l->next) {
			const struct srd_channel *const pdch = (struct srd_channel *)l->data;
			bool ch_added = false;

			// Copy but update ID if this channel was in the list before
			for (decode::DecodeChannel& ch : prev_channels)
				if (ch.pdch_ == pdch) {
					ch.id = id++;
					channels_.push_back(ch);
					ch_added = true;
					break;
				}

			if (!ch_added) {
				// Create new entry without a mapped signal
				decode::DecodeChannel ch = {id++, 0, false, nullptr,
					QString::fromUtf8(pdch->name), QString::fromUtf8(pdch->desc),
					SRD_INITIAL_PIN_SAME_AS_SAMPLE0, decoder, pdch};
				channels_.push_back(ch);
			}
		}

		// Optional channels
		for (l = srd_dec->opt_channels; l; l = l->next) {
			const struct srd_channel *const pdch = (struct srd_channel *)l->data;
			bool ch_added = false;

			// Copy but update ID if this channel was in the list before
			for (decode::DecodeChannel& ch : prev_channels)
				if (ch.pdch_ == pdch) {
					ch.id = id++;
					channels_.push_back(ch);
					ch_added = true;
					break;
				}

			if (!ch_added) {
				// Create new entry without a mapped signal
				decode::DecodeChannel ch = {id++, 0, true, nullptr,
					QString::fromUtf8(pdch->name), QString::fromUtf8(pdch->desc),
					SRD_INITIAL_PIN_SAME_AS_SAMPLE0, decoder, pdch};
				channels_.push_back(ch);
			}
		}
	}

	// Invalidate the logic output data if the channel assignment changed
	if (prev_channels.size() != channels_.size()) {
		// The number of channels changed, there's definitely a difference
		logic_mux_data_invalid_ = true;
	} else {
		// Same number but assignment may still differ, so compare all channels
		for (size_t i = 0; i < channels_.size(); i++) {
			const decode::DecodeChannel& p_ch = prev_channels[i];
			const decode::DecodeChannel& ch = channels_[i];

			if ((p_ch.pdch_ != ch.pdch_) ||
				(p_ch.assigned_signal != ch.assigned_signal)) {
				logic_mux_data_invalid_ = true;
				break;
			}
		}

	}
	channels_updated();
}

void DecodeSignal::commit_decoder_channels()
{
	// Submit channel list to every decoder, containing only the relevant channels
	for (shared_ptr<Decoder> dec : stack_) {
		vector<decode::DecodeChannel*> channel_list;

		for (decode::DecodeChannel& ch : channels_)
			if (ch.decoder_ == dec)
				channel_list.push_back(&ch);

		dec->set_channels(channel_list);
	}

	// Channel bit IDs must be in sync with the channel's apperance in channels_
	int id = 0;
	for (decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal)
			ch.bit_id = id++;

	// Rename assigned signals to match decoder channel names (e.g. D0→TX, D1→RX)
	for (decode::DecodeChannel& ch : channels_) {
		if (ch.assigned_signal && !ch.name.isEmpty()) {
			// Save original name before overwriting, so we can restore it later
			auto s = std::const_pointer_cast<SignalBase>(ch.assigned_signal);
			if (s->internal_name().isEmpty())
				s->set_internal_name(s->name());
			QString signal_name = QString::fromUtf8(ch.name.toLatin1().constData());
			s->set_name(signal_name);
		}
	}
}

bool channel_8_switch_4(uint8_t* pSource, uint64_t lBufferSize, uint8_t* pDest)
{
	if (!pDest || !pSource)
		return false;
	uint64_t index = 0;
	for (size_t i = 0; i < lBufferSize;) {
		pDest[index] = pSource[i++];
		pDest[index] = pDest[index] | (pSource[i++] << 4);
		index++;
	}
	return true;
}

bool channel_8_switch_2(uint8_t* pSource, uint64_t lBufferSize, uint8_t* pDest)
{
	if (!pDest || !pSource)
		return false;
	uint64_t index = 0;
	for (size_t i = 0; i < lBufferSize;) {
		pDest[index] = pSource[i++];
		pDest[index] = pDest[index] | (pSource[i++] << 2);
		pDest[index] = pDest[index] | (pSource[i++] << 4);
		pDest[index] = pDest[index] | (pSource[i++] << 6);
		index++;
	}
	return true;
}

void DecodeSignal::mux_logic_samples(uint32_t segment_id, const int64_t start, const int64_t end)
{
	// Enforce end to be greater than start
	if (end <= start)
		return;
	// Fetch the channel segments and their data
	vector<shared_ptr<const LogicSegment> > segments;
	vector<const uint8_t*> signal_data;
	vector<uint8_t> signal_in_bytepos;
	vector<uint8_t> signal_in_bitpos;
	uint8_t* output_zoom = NULL;
	for (decode::DecodeChannel& ch : channels_)
		if (ch.assigned_signal) {
			const shared_ptr<Logic> logic_data = ch.assigned_signal->logic_data();
			if (!logic_data || logic_data->logic_segments().empty())
				continue;

			shared_ptr<const LogicSegment> segment;
			if (segment_id < logic_data->logic_segments().size()) {
				segment = logic_data->logic_segments().at(segment_id)->get_shared_ptr();
			} else {
				qDebug() << "Muxer error for" << name() << ":" << ch.assigned_signal->name() \
					<< "has no logic segment" << segment_id;
				logic_mux_interrupt_ = true;
				return;
			}

			if (!segment)
				return;

			segments.push_back(segment);

			const uint64_t unit_sz = segment->unit_size();
			if (unit_sz == 0)
				return;

			uint8_t* data = new uint8_t[(end - start) * unit_sz];
			if (data == NULL) {
				return;
			}
			segment->get_samples(start, end, data);
			signal_data.push_back(data);

			const int bitpos = ch.assigned_signal->logic_bit_index();
			signal_in_bytepos.push_back(bitpos / 8);
			signal_in_bitpos.push_back(bitpos % 8);
		}

	shared_ptr<LogicSegment> output_segment;
	try {
		output_segment = logic_mux_data_->logic_segments().at(segment_id);
	} catch (out_of_range&) {
		qDebug() << "Muxer error for" << name() << ": no logic mux segment" \
			<< segment_id << "in mux_logic_samples(), mux segments size is" \
			<< logic_mux_data_->logic_segments().size();
		logic_mux_interrupt_ = true;
		return;
	}

	// Perform the muxing of signal data into the output data
	uint8_t* output = new uint8_t[(end - start) * output_segment->unit_size()];
	unsigned int signal_count = signal_data.size();

	for (int64_t sample_cnt = 0; !logic_mux_interrupt_ && (sample_cnt < (end - start));
		sample_cnt++) {

		int bitpos = 0;
		uint8_t bytepos = 0;

		const int out_sample_pos = sample_cnt * output_segment->unit_size();
		for (unsigned int i = 0; i < output_segment->unit_size(); i++)
			output[out_sample_pos + i] = 0;

		for (unsigned int i = 0; i < signal_count; i++) {
			const int in_sample_pos = sample_cnt * segments[i]->unit_size();
			const uint8_t in_sample = 1 &
				((signal_data[i][in_sample_pos + signal_in_bytepos[i]]) >> (signal_in_bitpos[i]));

			// const uint8_t out_sample = output[out_sample_pos + bytepos];

			output[out_sample_pos + bytepos] = output[out_sample_pos + bytepos] | (in_sample << bitpos);

			bitpos++;
			if (bitpos > 7) {
				bitpos = 0;
				bytepos++;
			}
		}
	}
	output_zoom = new uint8_t[(uint64_t)(((end - start) / (double)output_segment->unit_size_temp) + 7)];
	if (output_zoom == NULL) {
		return;
	}
	if (output_segment->channel_num_ == 4){	
		if (channel_8_switch_4(output, end - start, output_zoom) == false){
			return;
		}
	}else if (output_segment->channel_num_ == 2){
		if (channel_8_switch_2(output, end - start, output_zoom) == false){
			return;
		}
	}
	if (output_segment->channel_num_ < 8){
		output_segment->append_payload(output_zoom, (uint64_t)((end - start) / (double)output_segment->unit_size_temp));
	} else{
		output_segment->append_payload(output, (end - start) * output_segment->unit_size());
	}
	delete[] output;
	delete[] output_zoom;
	for (const uint8_t* data : signal_data)
		delete[] data;
}

void DecodeSignal::logic_mux_proc()
{
	uint32_t input_segment_count;
	do {
		input_segment_count = get_input_segment_count();
		if (input_segment_count == 0) {
			// Wait for input data
			unique_lock<mutex> logic_mux_lock(logic_mux_mutex_);
			logic_mux_cond_.wait(logic_mux_lock);
		}
	} while ((!logic_mux_interrupt_) && (input_segment_count == 0));

	if (logic_mux_interrupt_)
		return;

	assert(logic_mux_data_);

	uint32_t segment_id = 0;
	auto mux_start = std::chrono::steady_clock::now();
	uint64_t mux_total_samples = 0;

	// Create initial logic mux segment
	shared_ptr<LogicSegment> output_segment =
		make_shared<LogicSegment>(*logic_mux_data_, segment_id, logic_mux_unit_size_, 0);
	logic_mux_data_->push_segment(output_segment);

	output_segment->set_samplerate(get_input_samplerate(0));
	output_segment->set_channel_numner(get_input_channel_number(0));
	// output_segment->set_unit_size(get_input_unit_size(0));
	// Logic mux data is being updated
	logic_mux_data_invalid_ = false;

	uint64_t samples_to_process;
	do {
		do {
			const uint64_t input_sample_count = get_working_sample_count(segment_id);
			const uint64_t output_sample_count = output_segment->get_sample_count();

			samples_to_process =
				(input_sample_count > output_sample_count) ?
				(input_sample_count - output_sample_count) : 0;

			if (samples_to_process > 0) {
				const uint64_t unit_size = output_segment->unit_size();
				const uint64_t chunk_sample_count = DecodeChunkLength / unit_size;

				uint64_t processed_samples = 0;
				do {
					const uint64_t start_sample = output_sample_count + processed_samples;
					const uint64_t sample_count =
						min(samples_to_process - processed_samples,	chunk_sample_count);
					auto ms_t0 = std::chrono::steady_clock::now();
					mux_logic_samples(segment_id, start_sample, start_sample + sample_count);
					auto ms_t1 = std::chrono::steady_clock::now();
					mux_total_samples += sample_count;
					processed_samples += sample_count;
					// ...and process the newly muxed logic data
					decode_input_cond_.notify_one();
				} while (!logic_mux_interrupt_ && (processed_samples < samples_to_process));
			}
		} while (!logic_mux_interrupt_ && (samples_to_process > 0));

		if (!logic_mux_interrupt_) {
			// samples_to_process is now 0, we've exhausted the currently available input data

			// If the input segments are complete, we've completed this segment
			if (all_input_segments_complete(segment_id)) {
				if (!output_segment->is_complete())
					output_segment->set_complete();

				if (segment_id < get_input_segment_count() - 1) {

					// Process next segment
					segment_id++;

					output_segment =
						make_shared<LogicSegment>(*logic_mux_data_, segment_id,
							logic_mux_unit_size_, 0);
					logic_mux_data_->push_segment(output_segment);

					output_segment->set_samplerate(get_input_samplerate(segment_id));
				} else {
					// Wait for more input data if we're processing the currently last segment
					{
						auto mux_end = std::chrono::steady_clock::now();
						double mux_ms = std::chrono::duration<double, std::milli>(mux_end - mux_start).count();
						// fprintf(stderr, "MUX_TOTAL: time=%.1fms samples=%lu\n", mux_ms, (unsigned long)mux_total_samples);
					}
					return;
				}
			} else {
				// Input segments aren't all complete yet but samples_to_process is 0, wait for more input data
				unique_lock<mutex> logic_mux_lock(logic_mux_mutex_);
				logic_mux_cond_.wait(logic_mux_lock);
			}
		}
	} while (!logic_mux_interrupt_);
}

void DecodeSignal::decode_data(
	const int64_t abs_start_samplenum, const int64_t sample_count,
	const shared_ptr<const LogicSegment> input_segment)
{
	uint64_t start_pos = 0;
	uint64_t end_pos = input_segment->get_sample_count();

	struct DecodeStats {
		uint64_t total_chunks = 0;
		uint64_t srd_session_send_calls = 0;
		uint64_t mipmap_skip_chunks = 0;
		double time_get_samples = 0;
		double time_inline_mux = 0;
		double time_srd_session_send = 0;
		double time_new_annotations = 0;
		double time_progress_lock = 0;
		double time_mipmap_check = 0;
	};
	DecodeStats stats;
	static const int stats_print_interval = 5000;
	auto print_stats = [](const DecodeStats& s, bool final_print) {
		//fprintf(stderr, "DECODE_STAT[%s chunks=%" PRIu64 "]: "
			//"srd_send=%" PRIu64 " mmap_skip=%" PRIu64 "\n"
			//"  TIMING: get_samples=%.1fms inline_mux=%.1fms srd_send=%.1fms "
			//"new_ann=%.1fms prog_lock=%.1fms mmap_check=%.1fms\n",
			//final_print ? "FINAL" : "progress",
			//s.total_chunks, s.srd_session_send_calls, s.mipmap_skip_chunks,
			//s.time_get_samples, s.time_inline_mux, s.time_srd_session_send,
			//s.time_new_annotations, s.time_progress_lock, s.time_mipmap_check);
	};

	static const int64_t DecodeChunkSamples = 4 * 1024 * 1024;


	try {
		if (end_decode_pos_ == 0) {
			end_pos = input_segment->get_sample_count();
		} else {
			end_pos = end_decode_pos_;
		}
		if (start_pos > end_pos) {
			std::swap(start_pos, end_pos);
		}
		end_pos = min(input_segment->get_sample_count(), end_pos);

		struct DirectChMap {
			int src_byte_off;   // byte offset in source sample word
			int src_bit_off;    // bit offset within that byte
			int dst_bit_pos;    // bit position in decode chunk (= bit_id)
			int dec_ch_idx;     // decoder channel index (for constant_mask)
		};
		vector<DirectChMap> ch_map;
		int src_unit_size = 0;
		int dst_unit_size = 0;
		{
			ch_map.clear();
			int max_dst_bit = 0;
			for (const decode::DecodeChannel& ch : channels_) {
				if (!ch.assigned_signal) continue;
				DirectChMap m;
				int src_bit_idx = ch.assigned_signal->logic_bit_index();
				m.src_byte_off = src_bit_idx / 8;
				m.src_bit_off = src_bit_idx % 8;
				m.dst_bit_pos = ch.bit_id;
				m.dec_ch_idx = ch.pdch_->order;
				ch_map.push_back(m);
				if (m.dst_bit_pos + 1 > max_dst_bit) max_dst_bit = m.dst_bit_pos + 1;
			}
			src_unit_size = input_segment->unit_size();
			dst_unit_size = (max_dst_bit + 7) / 8;
			//fprintf(stderr, "DIRECT_MUX: src_unit_size=%d dst_unit_size=%d ch_map_size=%zu\n",
				//src_unit_size, dst_unit_size, ch_map.size());
			//for (size_t ci = 0; ci < ch_map.size(); ci++) {
				//fprintf(stderr, "  ch[%zu]: src_byte=%d src_bit=%d -> dst_bit=%d\n",
					//ci, ch_map[ci].src_byte_off, ch_map[ci].src_bit_off, ch_map[ci].dst_bit_pos);
			//}
		}

		// Allocate source buffer for reading raw LogicSegment data
				if (decode_chunk_ == NULL) {
			decode_chunk_ = new uint8_t[(110 * 1024 * 1024) + 7];
			decode_chunk_tmp_ = decode_chunk_ + (uint64_t)(((110 * 1024 * 1024) + 7) / 2.0);
		}
		// src_buffer for raw data from original LogicSegment (per-instance member)
		if (src_buffer_ == NULL || src_buffer_unit_size_ != src_unit_size) {
			if (src_buffer_) delete[] src_buffer_;
			src_buffer_ = new uint8_t[DecodeChunkSamples * src_unit_size + 7];
			src_buffer_unit_size_ = src_unit_size;
		}

		srd_decoder_inst *base_di = stack_[0]->get_decoder_inst();

		// === Mip-map skip state (persists across chunks) ===
		// Runtime switch for optimization (easy to disable for debugging)
		bool enable_mipmap_skip = true;
		uint64_t prev_constant_mask = 0;
		uint64_t prev_constant_vals = 0;
		bool prev_chunk_all_constant = false;
		// Bitmask of all decoder channels present in ch_map
		uint64_t all_channels_mask = 0;
		for (size_t ci = 0; ci < ch_map.size(); ci++) {
			all_channels_mask |= (1ULL << ch_map[ci].dec_ch_idx);
		}

		for (int64_t i = abs_start_samplenum;
			!decode_interrupt_ && (i < (abs_start_samplenum + sample_count));
			i += DecodeChunkSamples) {

			const int64_t chunk_end = min(i + DecodeChunkSamples,
				abs_start_samplenum + sample_count);
			const int64_t chunk_samples = chunk_end - i;

			stats.total_chunks++;

			if (enable_mipmap_skip && prev_chunk_all_constant && all_channels_mask != 0) {
				auto t_skip0 = chrono::steady_clock::now();
				bool range_constant = input_segment->is_range_constant(i, chunk_samples);
				auto t_skip1 = chrono::steady_clock::now();
				stats.time_mipmap_check += chrono::duration<double, milli>(t_skip1 - t_skip0).count();

				if (range_constant) {
					uint8_t first_sample_buf[8];
					input_segment->get_samples(i, i + 1, first_sample_buf);

					bool values_match = true;
					for (size_t ci = 0; ci < ch_map.size(); ci++) {
						const DirectChMap& m = ch_map[ci];
						if (!(prev_constant_mask & (1ULL << m.dec_ch_idx))) continue;
						uint8_t bit_val = (first_sample_buf[m.src_byte_off] >> m.src_bit_off) & 1;
						uint8_t prev_val = (prev_constant_vals >> m.dec_ch_idx) & 1;
						if (bit_val != prev_val) { values_match = false; break; }
					}

					if (values_match) {
						data_chunk_send_ = decode_chunk_;
						int64_t data_size = chunk_samples * dst_unit_size;
						auto t_srd0 = chrono::steady_clock::now();
						int srd_ret = srd_session_send(srd_session_, i, chunk_end,
							data_chunk_send_, data_size, dst_unit_size,
							prev_constant_mask, prev_constant_vals);
						auto t_srd1 = chrono::steady_clock::now();
						stats.srd_session_send_calls++;
						stats.time_srd_session_send += chrono::duration<double, milli>(t_srd1 - t_srd0).count();
						stats.mipmap_skip_chunks++;

						if (srd_ret != SRD_OK && srd_ret != SRD_CHUNK_SKIPPED) {
							set_error_message(tr("Decoder reported an error"));
							decode_interrupt_ = true;
						}

						if (stats.mipmap_skip_chunks % 100 == 0 || chunk_end >= (int64_t)(abs_start_samplenum + sample_count)) {
							lock_guard<mutex> lock(output_mutex_);
							segments_.at(current_segment_id_).samples_decoded_incl = chunk_end;
							segments_.at(current_segment_id_).samples_decoded_excl = chunk_end;
						}

						{
							new_annotations();
						}

						continue;
					}
					prev_chunk_all_constant = false;
				} else {
					// Range has edges - need to re-mux
					prev_chunk_all_constant = false;
				}
			}

			auto t_get0 = chrono::steady_clock::now();
			input_segment->get_samples(i, chunk_end, src_buffer_);
			auto t_get1 = chrono::steady_clock::now();
						stats.time_get_samples += chrono::duration<double, milli>(t_get1 - t_get0).count();

			auto t_mux0 = chrono::steady_clock::now();
			uint64_t constant_mask = 0, constant_vals = 0;

			uint8_t first_bits[64];
			bool is_constant[64];
			memset(is_constant, true, sizeof(is_constant));
			memset(first_bits, 0, sizeof(first_bits));

			for (int64_t s = 0; s < chunk_samples; s++) {
				const uint8_t* src = src_buffer_ + s * src_unit_size;
				uint8_t dst_word[8] = {0};

				for (size_t ci = 0; ci < ch_map.size(); ci++) {
					const DirectChMap& m = ch_map[ci];
					uint8_t bit_val = (src[m.src_byte_off] >> m.src_bit_off) & 1;
					dst_word[m.dst_bit_pos / 8] |= (bit_val << (m.dst_bit_pos % 8));

					if (s == 0) {
						first_bits[m.dst_bit_pos] = bit_val;
					} else if (is_constant[m.dst_bit_pos] && bit_val != first_bits[m.dst_bit_pos]) {
						is_constant[m.dst_bit_pos] = false;
					}
				}

				for (int b = 0; b < dst_unit_size; b++) {
					decode_chunk_[s * dst_unit_size + b] = dst_word[b];
				}
			}

			for (size_t ci = 0; ci < ch_map.size(); ci++) {
				const DirectChMap& m = ch_map[ci];
				if (is_constant[m.dst_bit_pos]) {
					constant_mask |= (1ULL << m.dec_ch_idx);
					if (first_bits[m.dst_bit_pos])
						constant_vals |= (1ULL << m.dec_ch_idx);
				}
			}

			prev_constant_mask = constant_mask;
			prev_constant_vals = constant_vals;
			prev_chunk_all_constant = (constant_mask == all_channels_mask);

			auto t_mux1 = chrono::steady_clock::now();
			stats.time_inline_mux += chrono::duration<double, milli>(t_mux1 - t_mux0).count();

			data_chunk_send_ = decode_chunk_;
			int64_t data_size = chunk_samples * dst_unit_size;

			auto t_srd0 = chrono::steady_clock::now();
			int srd_ret = srd_session_send(srd_session_, i, chunk_end,
				data_chunk_send_, data_size, dst_unit_size,
				constant_mask, constant_vals);
			auto t_srd1 = chrono::steady_clock::now();
			stats.srd_session_send_calls++;
			stats.time_srd_session_send += chrono::duration<double, milli>(t_srd1 - t_srd0).count();

			if (srd_ret != SRD_OK && srd_ret != SRD_CHUNK_SKIPPED) {
				set_error_message(tr("Decoder reported an error"));
				decode_interrupt_ = true;
			}

			{
				auto t0 = chrono::steady_clock::now();
				lock_guard<mutex> lock(output_mutex_);
				segments_.at(current_segment_id_).samples_decoded_incl = chunk_end;
				segments_.at(current_segment_id_).samples_decoded_excl = chunk_end;
				auto t1 = chrono::steady_clock::now();
				
				stats.time_progress_lock += chrono::duration<double, milli>(t1 - t0).count();
			}

			{
				auto t0 = chrono::steady_clock::now();
				new_annotations();
				auto t1 = chrono::steady_clock::now();
				
				stats.time_new_annotations += chrono::duration<double, milli>(t1 - t0).count();
			}

			if (decode_paused_) {
				unique_lock<mutex> pause_wait_lock(decode_pause_mutex_);
				decode_pause_cond_.wait(pause_wait_lock);
			}

			if (stats.total_chunks % stats_print_interval == 0) {
				print_stats(stats, false);
			}
		}

	print_stats(stats, true);
	}
	catch (bad_alloc&)
	{
		qDebug() << "decodesignal out memory";
		LogicSegment* ptr = const_cast<LogicSegment*>(input_segment.get());
		ptr->mark_bad_memory();
		return;
	}
}

void DecodeSignal::decode_proc()
{
	current_segment_id_ = 0;

	shared_ptr<const LogicSegment> source_segment;
	{
		do {
			for (const decode::DecodeChannel& ch : channels_) {
				if (!ch.assigned_signal) continue;
				auto logic = ch.assigned_signal->logic_data();
				if (!logic || logic->logic_segments().empty()) continue;
				source_segment = logic->logic_segments()[0]->get_shared_ptr();
				break;
			}
			if (source_segment)
				break;
			// No data available yet, wait for notification
			unique_lock<mutex> input_wait_lock(input_mutex_);
			decode_input_cond_.wait(input_wait_lock);
		} while (!decode_interrupt_);
	}

	if (decode_interrupt_)
		return;

	if (decode_interrupt_)
		return;

	// Create the initial segment and set its sample rate so that we can pass it to SRD
	create_decode_segment();
	segments_.at(current_segment_id_).samplerate = source_segment->samplerate();
	segments_.at(current_segment_id_).start_time = source_segment->start_time();

	start_srd_session();

	//fprintf(stderr, "DECODE_PROC: source_segment sample_count=%lu ,is_complete=%d\n", (unsigned long)source_segment->get_sample_count(), source_segment->is_complete());
	auto decode_proc_start = std::chrono::steady_clock::now();
	uint64_t total_decode_data_time_ms = 0;
	uint64_t total_wait_time_ms = 0;

	uint64_t abs_start_samplenum = 0;
	do {
		// Keep processing new samples until we exhaust the input data
		uint64_t samples_to_process = 0;
		do {
			samples_to_process = source_segment->get_sample_count() - abs_start_samplenum;
			if (samples_to_process > 0) {
				auto dd_t0 = std::chrono::steady_clock::now();
				decode_data(abs_start_samplenum, samples_to_process, source_segment);
				auto dd_t1 = std::chrono::steady_clock::now();
				total_decode_data_time_ms += (uint64_t)std::chrono::duration<double, std::milli>(dd_t1 - dd_t0).count();
				abs_start_samplenum += samples_to_process;
			}
		} while (!decode_interrupt_ && (samples_to_process > 0));

		if (!decode_interrupt_) {
			if (source_segment->is_complete()) {
				// All data has been processed
				{
					auto decode_proc_end = std::chrono::steady_clock::now();
					double total_ms = std::chrono::duration<double, std::milli>(decode_proc_end - decode_proc_start).count();
					//fprintf(stderr, "DECODE_PROC_TOTAL: total=%.1fms decode_data=%.1fms wait_data=%.1fms\n", total_ms, (double)total_decode_data_time_ms, (double)total_wait_time_ms);
				}
				if (srd_session_) srd_session_send_eof(srd_session_);
				if (!decode_interrupt_)
					decode_finished();
				return;
			} else {
				// Wait for more input data from acquisition
				auto wt0 = std::chrono::steady_clock::now();
				unique_lock<mutex> input_wait_lock(input_mutex_);
				decode_input_cond_.wait(input_wait_lock);
				auto wt1 = std::chrono::steady_clock::now();
				total_wait_time_ms += (uint64_t)std::chrono::duration<double, std::milli>(wt1 - wt0).count();
			}
		}
	} while (!decode_interrupt_);
}

void DecodeSignal::start_srd_session()
{
	// If there were stack changes, the session has been destroyed by now, so if
	// it hasn't been destroyed, we can just reset and re-use it
	if (srd_session_) {
		// When a decoder stack was created before, re-use it
		// for the next stream of input data, after terminating
		// potentially still executing operations, and resetting
		// internal state. Skip the rather expensive (teardown
		// and) construction of another decoder stack.

		// TODO Reduce redundancy, use a common code path for
		// the meta/start sequence?
		terminate_srd_session();

		// Metadata is cleared also, so re-set it
		uint64_t samplerate = 0;
		if (segments_.size() > 0)
			samplerate = segments_.at(current_segment_id_).samplerate;
		if (samplerate)
			srd_session_metadata_set(srd_session_, SRD_CONF_SAMPLERATE,
				g_variant_new_uint64(samplerate));
		for (const shared_ptr<Decoder>& dec : stack_)
			dec->apply_all_options();
		srd_session_start(srd_session_);

		return;
	}

	// Update the samplerates for the output logic channels
	update_output_signals();

	// Create the session
	srd_session_new(&srd_session_);
	assert(srd_session_);

	// Create the decoders
	srd_decoder_inst *prev_di = nullptr;
	for (const shared_ptr<Decoder>& dec : stack_) {
		srd_decoder_inst *const di = dec->create_decoder_inst(srd_session_);

		if (!di) {
			set_error_message(tr("Failed to create decoder instance"));
			srd_session_destroy(srd_session_);
			srd_session_ = nullptr;
			return;
		}

		if (prev_di)
			srd_inst_stack(srd_session_, prev_di, di);

		prev_di = di;
	}

	// Start the session
	if (segments_.size() > 0)
		srd_session_metadata_set(srd_session_, SRD_CONF_SAMPLERATE,
			g_variant_new_uint64(segments_.at(current_segment_id_).samplerate));

	srd_pd_output_callback_add(srd_session_, SRD_OUTPUT_ANN,
		DecodeSignal::annotation_callback, this);

	srd_pd_output_callback_add(srd_session_, SRD_OUTPUT_BINARY,
		DecodeSignal::binary_callback, this);

	srd_pd_output_callback_add(srd_session_, SRD_OUTPUT_LOGIC,
		DecodeSignal::logic_output_callback, this);

	srd_session_start(srd_session_);

	// We just recreated the srd session, so all stack changes are applied now
	stack_config_changed_ = false;
}

void DecodeSignal::terminate_srd_session()
{
	// Call the "terminate and reset" routine for the decoder stack
	// (if available). This does not harm those stacks which already
	// have completed their operation, and reduces response time for
	// those stacks which still are processing data while the
	// application no longer wants them to.
	if (srd_session_) {
		srd_session_terminate_reset(srd_session_);

		// Metadata is cleared also, so re-set it
		uint64_t samplerate = 0;
		if (segments_.size() > 0)
			samplerate = segments_.at(current_segment_id_).samplerate;
		if (samplerate)
			srd_session_metadata_set(srd_session_, SRD_CONF_SAMPLERATE,
				g_variant_new_uint64(samplerate));
		for (const shared_ptr<Decoder>& dec : stack_)
			dec->apply_all_options();
	}
}

void DecodeSignal::stop_srd_session()
{
	if (srd_session_) {
		// Destroy the session
		srd_session_destroy(srd_session_);
		srd_session_ = nullptr;

		// Mark the decoder instances as non-existant since they were deleted
		for (const shared_ptr<Decoder>& dec : stack_)
			dec->invalidate_decoder_inst();
	}
}

void DecodeSignal::connect_input_notifiers()
{
	// Connect the currently used signals to our slot
	for (decode::DecodeChannel& ch : channels_) {
		if (!ch.assigned_signal)
			continue;
		const data::SignalBase *signal = ch.assigned_signal.get();

		connect(signal, SIGNAL(samples_cleared()),
			this, SLOT(on_data_cleared()), Qt::UniqueConnection);
		connect(signal, SIGNAL(samples_added(uint64_t, uint64_t, uint64_t)),
			this, SLOT(on_data_received()), Qt::UniqueConnection);

		if (signal->logic_data())
			connect(signal->logic_data().get(), SIGNAL(segment_completed()),
				this, SLOT(on_input_segment_completed()), Qt::UniqueConnection);
	}
}

void DecodeSignal::disconnect_input_notifiers()
{
	// Disconnect the notification slot from the previous set of signals
	for (decode::DecodeChannel& ch : channels_) {
		if (!ch.assigned_signal)
			continue;
		const data::SignalBase *signal = ch.assigned_signal.get();
		disconnect(signal, nullptr, this, SLOT(on_data_cleared()));
		disconnect(signal, nullptr, this, SLOT(on_data_received()));

		if (signal->logic_data())
			disconnect(signal->logic_data().get(), nullptr, this, SLOT(on_input_segment_completed()));
	}
}

void DecodeSignal::create_decode_segment()
{
	// Create annotation segment
	segments_.emplace_back();

	// Add annotation classes
	for (const shared_ptr<Decoder>& dec : stack_)
		for (Row* row : dec->get_rows())
			segments_.back().annotation_rows.emplace(row, RowData(row));

	// Prepare our binary output classes
	for (const shared_ptr<Decoder>& dec : stack_) {
		uint32_t n = dec->get_binary_class_count();

		for (uint32_t i = 0; i < n; i++)
			segments_.back().binary_classes.push_back(
				{dec.get(), dec->get_binary_class(i), deque<DecodeBinaryDataChunk>()});
	}
}

void DecodeSignal::annotation_callback(srd_proto_data *pdata, void *decode_signal)
{
	assert(pdata);
	assert(decode_signal);

	DecodeSignal *const ds = (DecodeSignal*)decode_signal;
	assert(ds);

	if (ds->decode_interrupt_)
		return;

	if (ds->segments_.empty())
		return;

	lock_guard<mutex> lock(ds->output_mutex_);

	// Get the decoder and the annotation data
	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const srd_dec = pdata->pdo->di->decoder;
	assert(srd_dec);

	const srd_proto_data_annotation *const pda = (const srd_proto_data_annotation*)pdata->data;
	assert(pda);

	// Find the row
	Decoder* dec = ds->get_decoder_by_instance(srd_dec);
	assert(dec);

	AnnotationClass* ann_class = dec->get_ann_class_by_id(pda->ann_class);
	if (!ann_class) {
		qWarning() << "Decoder" << ds->display_name() << "wanted to add annotation" <<
			"with class ID" << pda->ann_class << "but there are only" <<
			dec->ann_classes().size() << "known classes";
		return;
	}

	const Row* row = ann_class->row;

	if (!row)
		row = dec->get_row_by_id(0);

	RowData& row_data = ds->segments_[ds->current_segment_id_].annotation_rows.at(row);

	// Add the annotation to the row
	const Annotation* ann = row_data.emplace_annotation(pdata);

	// We insert the annotation into the global annotation list in a way so that
	// the annotation list is sorted by start sample and length. Otherwise, we'd
	// have to sort the model, which is expensive
	deque<const Annotation*>& all_annotations =
		ds->segments_[ds->current_segment_id_].all_annotations;

	if (all_annotations.empty()) {
		all_annotations.emplace_back(ann);
	} else {
		const uint64_t new_ann_len = (pdata->end_sample - pdata->start_sample);
		bool ann_has_earlier_start = (pdata->start_sample < all_annotations.back()->start_sample());
		bool ann_is_longer = (new_ann_len >
			(all_annotations.back()->end_sample() - all_annotations.back()->start_sample()));

		if (ann_has_earlier_start && ann_is_longer) {
			bool ann_has_same_start;
			auto it = all_annotations.end();

			do {
				it--;
				ann_has_earlier_start = (pdata->start_sample < (*it)->start_sample());
				ann_has_same_start = (pdata->start_sample == (*it)->start_sample());
				ann_is_longer = (new_ann_len > (*it)->length());
			} while ((ann_has_earlier_start || (ann_has_same_start && ann_is_longer)) && (it != all_annotations.begin()));

			// Allow inserting at the front
			if (it != all_annotations.begin())
				it++;

			all_annotations.emplace(it, ann);
		} else
			all_annotations.emplace_back(ann);
	}

	// When emplace_annotation() inserts instead of appends an annotation,
	// the pointers in all_annotations that follow the inserted annotation and
	// point to annotations for this row are off by one and must be updated
	if (&(row_data.annotations().back()) != ann) {
		// Search backwards until we find the annotation we just added
		auto row_it = row_data.annotations().end();
		auto all_it = all_annotations.end();
		do {
			all_it--;
			if ((*all_it)->row_data() == &row_data)
				row_it--;
		} while (&(*row_it) != ann);

		// Update the annotation addresses for this row's annotations until the end
		do {
			if ((*all_it)->row_data() == &row_data) {
				*all_it = &(*row_it);
				row_it++;
			}
			all_it++;
		} while (all_it != all_annotations.end());
	}
}

void DecodeSignal::binary_callback(srd_proto_data *pdata, void *decode_signal)
{
	assert(pdata);
	assert(decode_signal);

	DecodeSignal *const ds = (DecodeSignal*)decode_signal;
	assert(ds);

	if (ds->decode_interrupt_)
		return;

	// Get the decoder and the binary data
	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const srd_dec = pdata->pdo->di->decoder;
	assert(srd_dec);

	const srd_proto_data_binary *const pdb = (const srd_proto_data_binary*)pdata->data;
	assert(pdb);

	// Find the matching DecodeBinaryClass
	DecodeSegment* segment = &(ds->segments_.at(ds->current_segment_id_));

	DecodeBinaryClass* bin_class = nullptr;
	for (DecodeBinaryClass& bc : segment->binary_classes)
		if ((bc.decoder->get_srd_decoder() == srd_dec) &&
			(bc.info->bin_class_id == (uint32_t)pdb->bin_class))
			bin_class = &bc;

	if (!bin_class) {
		qWarning() << "Could not find valid DecodeBinaryClass in segment" <<
				ds->current_segment_id_ << "for binary class ID" << pdb->bin_class <<
				", segment only knows" << segment->binary_classes.size() << "classes";
		return;
	}

	// Add the data chunk
	bin_class->chunks.emplace_back();
	DecodeBinaryDataChunk* chunk = &(bin_class->chunks.back());

	chunk->sample = pdata->start_sample;
	chunk->data.resize(pdb->size);
	memcpy(chunk->data.data(), pdb->data, pdb->size);

	Decoder* dec = ds->get_decoder_by_instance(srd_dec);

	ds->new_binary_data(ds->current_segment_id_, (void*)dec, pdb->bin_class);
}

void DecodeSignal::logic_output_callback(srd_proto_data *pdata, void *decode_signal)
{
	assert(pdata);
	assert(decode_signal);

	DecodeSignal *const ds = (DecodeSignal*)decode_signal;
	assert(ds);

	if (ds->decode_interrupt_)
		return;

	lock_guard<mutex> lock(ds->output_mutex_);

	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const decc = pdata->pdo->di->decoder;
	assert(decc);

	const srd_proto_data_logic *const pdl = (const srd_proto_data_logic*)pdata->data;
	assert(pdl);

	// FIXME Only one group supported for now
	if (pdl->logic_group > 0) {
		qWarning() << "Received logic output state change for group" << pdl->logic_group << "from decoder" \
			<< QString::fromUtf8(decc->name) << "but only group 0 is currently supported";
		return;
	}

	shared_ptr<Logic> output_logic = ds->output_logic_.at(decc);

	vector< shared_ptr<Segment> > segments = output_logic->segments();

	shared_ptr<LogicSegment> last_segment;

	if (!segments.empty())
		last_segment = dynamic_pointer_cast<LogicSegment>(segments.back());
	else {
		// Happens when the data was cleared - all segments are gone then
		// segment_id is always 0 as it's the first segment
		last_segment = make_shared<data::LogicSegment>(
			*output_logic, 0, (output_logic->num_channels() + 7) / 8, output_logic->get_samplerate());
		output_logic->push_segment(last_segment);
	}

	if (pdata->start_sample < pdata->end_sample) {
		vector<uint8_t> data;
		const unsigned int unit_size = last_segment->unit_size();
		data.resize(unit_size * (1 + pdl->repeat_count));

		if (unit_size == 1)
			for (unsigned int i = 0; i <= pdl->repeat_count; i++)
				data.data()[i * unit_size] = *((uint8_t*)pdl->data);
		else if (unit_size == 2)
			for (unsigned int i = 0; i <= pdl->repeat_count; i++)
				data.data()[i * unit_size] = *((uint16_t*)pdl->data);
		else if (unit_size <= 4)
			for (unsigned int i = 0; i <= pdl->repeat_count; i++)
				data.data()[i * unit_size] = *((uint32_t*)pdl->data);
		else if (unit_size <= 8)
			for (unsigned int i = 0; i <= pdl->repeat_count; i++)
				data.data()[i * unit_size] = *((uint64_t*)pdl->data);
		else
			for (unsigned int i = 0; i <= pdl->repeat_count; i++)
				memcpy((void*)&data.data()[i * unit_size], (void*)pdl->data, unit_size);
		last_segment->append_payload(data.data(), data.size());
	} else
		qWarning() << "Ignoring malformed logic output state change for group" << pdl->logic_group << "from decoder" \
			<< QString::fromUtf8(decc->name) << "from" << pdata->start_sample << "to" << pdata->end_sample;
}

void DecodeSignal::on_capture_state_changed(int state)
{
	current_state_ = state;
	// If a new acquisition was started, we need to start decoding from scratch
	if (state == Session::Running) {
		reset_decode();
		logic_mux_data_invalid_ = true;
		begin_decode();
	}
	if (state == Session::Stopped) {
		on_data_received();
	}
}

void DecodeSignal::on_data_cleared()
{
	reset_decode();
}

void DecodeSignal::on_data_received()
{
	// If we detected a lack of input data when trying to start decoding,
	// we have set an error message. Bail out if we still don't have data
	// to work with
	if ((!error_message_.isEmpty()) && (get_input_segment_count() == 0))
		return;

	if (!error_message_.isEmpty()) {
		error_message_.clear();
		// TODO Emulate noquote()
		qDebug().nospace() << name() << ": Input data available, error cleared";
	}
	if (!logic_mux_thread_.joinable())
		begin_decode();
	else{
		// qDebug()<< "session_.decode_signal_map_.size() == " << session_.decode_signal_map_.size();
		// if (session_.decode_signal_map_.size() == 1)
		logic_mux_cond_.notify_one();
	}
}

void DecodeSignal::on_input_segment_completed()
{
	if (!logic_mux_thread_.joinable())
		logic_mux_cond_.notify_one();
}

void DecodeSignal::on_annotation_visibility_changed()
{
	annotation_visibility_changed();
}

} // namespace data
} // namespace pv
