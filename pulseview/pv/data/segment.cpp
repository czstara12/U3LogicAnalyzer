/*
 * This file is part of the LogicAnalyzer project.
 * LogicAnaylzer is based on Pulseview.
 *
 * Copyright (C) 2017 Soeren Apel <soeren@apelpie.net>
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

#include "segment.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <QDebug>

using std::bad_alloc;
using std::lock_guard;
using std::min;
using std::recursive_mutex;

namespace pv {
namespace data {

const uint64_t Segment::MaxChunkSize = 2 * 1024 * 1024;  /* 2MiB */

Segment::Segment(uint32_t segment_id, uint64_t samplerate, unsigned int unit_size) :
	segment_id_(segment_id),
	sample_count_(0),
	start_time_(0),
	samplerate_(samplerate),
	unit_size_(unit_size),
	iterator_count_(0),
	mem_optimization_requested_(false),
	is_complete_(false),
	static_zero_chunk_(nullptr),
	static_one_chunk_(nullptr)
{
	assert(unit_size_ > 0);
	// Determine the number of samples we can fit in one chunk
	// without exceeding MaxChunkSize
	chunk_size_ = min(MaxChunkSize, (MaxChunkSize / unit_size_) * unit_size_);

	try {
		static_zero_chunk_ = new uint8_t[chunk_size_ + 7];
		memset(static_zero_chunk_, 0, chunk_size_ + 7);
		static_one_chunk_ = new uint8_t[chunk_size_ + 7];
		memset(static_one_chunk_, 0xFF, chunk_size_ + 7);

		current_chunk_ = new uint8_t[chunk_size_ + 7];  /* FIXME +7 is workaround for #1284 */
	} catch (bad_alloc&) {
		qDebug() << "Segment constructor: memory allocation failed";
		delete[] static_zero_chunk_;
		static_zero_chunk_ = nullptr;
		delete[] static_one_chunk_;
		static_one_chunk_ = nullptr;
		// current_chunk_ not yet assigned if new threw before reaching it
		bad_memory_ = true;
		throw;  // Re-throw: object construction failed, caller must handle
	}
	data_chunks_.push_back(current_chunk_);
	chunk_types_.push_back(ChunkType::NORMAL);
	used_samples_ = 0;
	unused_samples_ = chunk_size_ / unit_size_;
}

Segment::~Segment()
{
	lock_guard<recursive_mutex> lock(mutex_);

	for (size_t i = 0; i < data_chunks_.size(); i++) {
		if (data_chunks_[i] != nullptr) {
			delete[] data_chunks_[i];
		}
	}
	if (static_zero_chunk_) {
		delete[] static_zero_chunk_;
		static_zero_chunk_ = nullptr;
	}
	if (static_one_chunk_) {
		delete[] static_one_chunk_;
		static_one_chunk_ = nullptr;
	}
	if (data_zoom != NULL) {
		delete[] data_zoom;
		data_zoom = NULL;
	}
}

uint64_t Segment::get_sample_count() const
{
	return sample_count_;
}

const pv::util::Timestamp& Segment::start_time() const
{
	return start_time_;
}

double Segment::samplerate() const
{
	return samplerate_;
}

void Segment::set_samplerate(double samplerate)
{
	samplerate_ = samplerate;
}

unsigned int Segment::unit_size() const
{
	return unit_size_;
}

uint32_t Segment::segment_id() const
{
	return segment_id_;
}

void Segment::set_complete()
{
	is_complete_ = true;

	completed();
}

bool Segment::is_complete() const
{
	return is_complete_;
}

ChunkType Segment::check_chunk_type(const uint8_t* chunk, uint64_t size) const
{
	const uint64_t* ptr = (const uint64_t*)chunk;
	uint64_t num_elements = size / sizeof(uint64_t);

	if (num_elements == 0) {
		if (size == 0) return ChunkType::ALL_ZERO;

		bool all_zero = true;
		bool all_one = true;
		for (uint64_t i = 0; i < size; i++) {
			if (chunk[i] != 0) all_zero = false;
			if (chunk[i] != 0xFF) all_one = false;
		}
		if (all_zero) return ChunkType::ALL_ZERO;
		if (all_one) return ChunkType::ALL_ONE;
		return ChunkType::NORMAL;
	}

	uint64_t first = ptr[0];
	bool maybe_zero = (first == 0);
	bool maybe_one = (first == UINT64_MAX);

	if (!maybe_zero && !maybe_one) return ChunkType::NORMAL;

	for (uint64_t i = 1; i < num_elements; i++) {
		if (maybe_zero && ptr[i] != 0) return ChunkType::NORMAL;
		if (maybe_one && ptr[i] != UINT64_MAX) return ChunkType::NORMAL;
	}

	const uint8_t* tail = (const uint8_t*)(ptr + num_elements);
	for (uint64_t i = 0; i < (size % sizeof(uint64_t)); i++) {
		if (maybe_zero && tail[i] != 0) return ChunkType::NORMAL;
		if (maybe_one && tail[i] != 0xFF) return ChunkType::NORMAL;
	}

	return maybe_zero ? ChunkType::ALL_ZERO : ChunkType::ALL_ONE;
}

bool Segment::compare_chunks(const uint8_t* chunk1, const uint8_t* chunk2, uint64_t size) const
{
	const uint64_t* ptr1 = (const uint64_t*)chunk1;
	const uint64_t* ptr2 = (const uint64_t*)chunk2;
	uint64_t num_elements = size / sizeof(uint64_t);

	if (num_elements == 0) {
		return memcmp(chunk1, chunk2, size) == 0;
	}

	if (ptr1[0] != ptr2[0]) return false;
	if (num_elements > 1 && ptr1[num_elements - 1] != ptr2[num_elements - 1]) return false;

	for (uint64_t i = 1; i < num_elements - 1; i++) {
		if (ptr1[i] != ptr2[i]) return false;
	}

	const uint8_t* tail1 = (const uint8_t*)(ptr1 + num_elements);
	const uint8_t* tail2 = (const uint8_t*)(ptr2 + num_elements);
	for (uint64_t i = 0; i < (size % sizeof(uint64_t)); i++) {
		if (tail1[i] != tail2[i]) return false;
	}

	return true;
}

uint8_t* Segment::get_chunk_ptr_internal(uint64_t chunk_num) const
{
	if (chunk_num >= chunk_types_.size()) {
		return static_zero_chunk_;
	}

	ChunkType type = chunk_types_[chunk_num];

	while (type == ChunkType::SAME_AS_PREVIOUS && chunk_num > 0) {
		chunk_num--;
		type = chunk_types_[chunk_num];
	}

	switch (type) {
		case ChunkType::ALL_ZERO:
			return static_zero_chunk_;
		case ChunkType::ALL_ONE:
			return static_one_chunk_;
		case ChunkType::NORMAL:
		case ChunkType::SAME_AS_PREVIOUS:
		default:
			return data_chunks_[chunk_num];
	}
}

void Segment::free_unused_memory()
{
	lock_guard<recursive_mutex> lock(mutex_);

	if (iterator_count_ > 0) {
		mem_optimization_requested_ = true;
		return;
	}

	if (current_chunk_) {
		uint64_t used_size = used_samples_ * unit_size_;

		ChunkType type = check_chunk_type(current_chunk_, used_size);

		if (type == ChunkType::NORMAL && data_chunks_.size() > 1) {
			uint64_t prev_chunk_num = data_chunks_.size() - 2;
			uint8_t* prev_chunk = get_chunk_ptr_internal(prev_chunk_num);

			if (compare_chunks(current_chunk_, prev_chunk, used_size)) {
				type = ChunkType::SAME_AS_PREVIOUS;
			}
		}

		chunk_types_.back() = type;

		if (type == ChunkType::NORMAL) {
			uint8_t* resized_chunk;
			try {
				resized_chunk = new uint8_t[used_size + 7];  /* FIXME +7 is workaround for #1284 */
			} catch (bad_alloc&) {
				qDebug() << "free_unused_memory: resize allocation failed, keeping original chunk";
				return;  // Keep original chunk, just skip shrinking
			}
			memcpy(resized_chunk, current_chunk_, used_size);

			delete[] current_chunk_;
			current_chunk_ = resized_chunk;

			data_chunks_.pop_back();
			data_chunks_.push_back(resized_chunk);
		} else {
			delete[] current_chunk_;
			data_chunks_.pop_back();
			data_chunks_.push_back(nullptr);
			current_chunk_ = nullptr;
		}
	}
}

void Segment::append_single_sample(void *data)
{
	lock_guard<recursive_mutex> lock(mutex_);

	// There will always be space for at least one sample in
	// the current chunk, so we do not need to test for space

	memcpy(current_chunk_ + (used_samples_ * unit_size_), data, unit_size_);
	used_samples_++;
	unused_samples_--;

	if (unused_samples_ == 0) {
		ChunkType type = check_chunk_type(current_chunk_, chunk_size_);

		if (type == ChunkType::NORMAL && data_chunks_.size() > 1) {
			uint64_t prev_chunk_num = data_chunks_.size() - 2;
			uint8_t* prev_chunk = get_chunk_ptr_internal(prev_chunk_num);
			if (compare_chunks(current_chunk_, prev_chunk, chunk_size_)) {
				type = ChunkType::SAME_AS_PREVIOUS;
			}
		}

		chunk_types_.back() = type;

		if (type != ChunkType::NORMAL) {
			delete[] current_chunk_;
			data_chunks_.back() = nullptr;
		}

		try {
			current_chunk_ = new uint8_t[chunk_size_ + 7];  /* FIXME +7 is workaround for #1284 */
		} catch (bad_alloc&) {
			qDebug() << "append_single_sample: chunk allocation failed";
			current_chunk_ = nullptr;
			bad_memory_ = true;
			Q_EMIT memoryError();
			return;
		}
		data_chunks_.push_back(current_chunk_);
		chunk_types_.push_back(ChunkType::NORMAL);
		used_samples_ = 0;
		unused_samples_ = chunk_size_ / unit_size_;
	}

	sample_count_++;
}

void Segment::append_samples(void* data, uint64_t samples)
{
	lock_guard<recursive_mutex> lock(mutex_);

	const uint8_t* data_byte_ptr = (uint8_t*)data;
	uint64_t remaining_samples = samples;
	uint64_t data_offset = 0;
	do {
		uint64_t copy_count = 0;

		if (remaining_samples <= unused_samples_) {
			// All samples fit into the current chunk
			copy_count = remaining_samples;
		} else {
			// Only a part of the samples fit, fill up current chunk
			copy_count = unused_samples_;
		}

		const uint8_t* dest = &(current_chunk_[used_samples_ * unit_size_]);
		const uint8_t* src = &(data_byte_ptr[data_offset]);
		memcpy((void*)dest, (void*)src, (copy_count * unit_size_));

		used_samples_ += copy_count;
		unused_samples_ -= copy_count;
		remaining_samples -= copy_count;
		data_offset += (copy_count * unit_size_);

		if (unused_samples_ == 0) {
			ChunkType type = check_chunk_type(current_chunk_, chunk_size_);

			if (type == ChunkType::NORMAL && data_chunks_.size() > 1) {
				uint64_t prev_chunk_num = data_chunks_.size() - 2;
				uint8_t* prev_chunk = get_chunk_ptr_internal(prev_chunk_num);
				if (compare_chunks(current_chunk_, prev_chunk, chunk_size_)) {
					type = ChunkType::SAME_AS_PREVIOUS;
				}
			}

			chunk_types_.back() = type;

			if (type != ChunkType::NORMAL) {
				delete[] current_chunk_;
				data_chunks_.back() = nullptr;
			}

			try {
				current_chunk_ = new uint8_t[chunk_size_ + 7];  /* FIXME +7 is workaround for #1284 */
			} catch (bad_alloc&) {
				qDebug() << "append_samples: chunk allocation failed";
				current_chunk_ = nullptr;
				bad_memory_ = true;
				Q_EMIT memoryError();
				return;
			}

			data_chunks_.push_back(current_chunk_);
			chunk_types_.push_back(ChunkType::NORMAL);
			used_samples_ = 0;
			unused_samples_ = chunk_size_ / unit_size_;
		}
	} while (remaining_samples > 0);
	if (channel_num_ >= 8)
		sample_count_ += samples;
	else
		sample_count_ += samples * unit_size_temp;
}

const uint8_t* Segment::get_raw_sample(uint64_t sample_num) const
{
	assert(sample_num <= sample_count_);

	uint64_t chunk_num = (sample_num * unit_size_) / chunk_size_;
	uint64_t chunk_offs = (sample_num * unit_size_) % chunk_size_;

	if (channel_num_ < 8){
		chunk_num /= unit_size_temp;
		chunk_offs /= unit_size_temp;
	}

	lock_guard<recursive_mutex> lock(mutex_);  // Because of free_unused_memory()

	const uint8_t* chunk = get_chunk_ptr_internal(chunk_num);

	return chunk + chunk_offs;
}

void Segment::get_raw_samples(uint64_t start, uint64_t count, uint8_t* dest) const
{
	assert(start < sample_count_);
	assert(start + count <= sample_count_);
	assert(count > 0);
	assert(dest != nullptr);
	register uint8_t* dest_ptr = dest;
	if (channel_num_ >= 8) {
		uint64_t chunk_num = (start * unit_size_) / chunk_size_;
		uint64_t chunk_offs = (start * unit_size_) % chunk_size_;

		lock_guard<recursive_mutex> lock(mutex_);  // Because of free_unused_memory()

		while (count > 0) {
			uint8_t* chunk = get_chunk_ptr_internal(chunk_num);

			uint64_t copy_size = min(count * unit_size_,
				chunk_size_ - chunk_offs);
			memcpy(dest_ptr, chunk + chunk_offs, copy_size);
			dest_ptr += copy_size;
			count -= (copy_size / unit_size_);
			chunk_num++;
			chunk_offs = 0;
		}
	}
	else {
		uint64_t chunkNum = 0;
		uint64_t chunkOffs = 0;
		uint8_t bitNum = 0;
		register uint8_t bitIndex = 0;
		register uint64_t data_count = count;
		double startByte = 0;
		uint8_t* chunk = NULL;
		startByte = start / (double)unit_size_temp;
		bitNum = 8.0 * (startByte - (uint64_t)startByte);
		chunkNum = (uint64_t)floor(startByte) / chunk_size_;       // 0
		chunkOffs = (uint64_t)floor(startByte) % chunk_size_;       // 1
		chunk = get_chunk_ptr_internal(chunkNum);
		bitIndex = bitNum;
		while (data_count--) {
			(*dest_ptr) = (*(chunk + chunkOffs) >> bitIndex) & CHANNEL_MASK(channel_num_);
			dest_ptr++;
			// count--;
			bitIndex += channel_num_;
			if (bitIndex >= 8) {
				chunkOffs++;
				bitIndex = 0;
				if (chunkOffs >= chunk_size_) {
					chunkOffs = 0;
					chunkNum++;
					chunk = get_chunk_ptr_internal(chunkNum);
				}
			}
		}
	}
}

SegmentDataIterator* Segment::begin_sample_iteration(uint64_t start)
{
	SegmentDataIterator* it = new SegmentDataIterator;

	assert(start < sample_count_);

	iterator_count_++;
	it->sample_index = start;
	it->chunk_num = (start * unit_size_) / chunk_size_;
	it->chunk_offs = (start * unit_size_) % chunk_size_;
	it->chunk = get_chunk_ptr_internal(it->chunk_num);

	return it;
}

SegmentDataIterator* Segment::begin_sample_iteration_Ex(uint64_t start, uint64_t& sample_len, uint8_t*& sample_data)
{
	SegmentDataIterator* it = new SegmentDataIterator;

	assert(start < sample_count_);

	iterator_count_++;
	
	double start_zoom = (double)start / unit_size_temp;
	uint64_t len = (8.0 * (start_zoom - (uint64_t)start_zoom)) / channel_num_;
	get_raw_samples(start, len, sample_data);

	it->sample_index = start;
	it->chunk_num = (uint64_t)ceil(start_zoom) / chunk_size_;
	it->chunk_offs = (uint64_t)ceil(start_zoom) % chunk_size_;
	it->chunk = get_chunk_ptr_internal(it->chunk_num);
	sample_len -= len;
	sample_data += len;
	return it;
}

void Segment::continue_sample_iteration(SegmentDataIterator* it, uint64_t increase)
{
	it->sample_index += increase;
	it->chunk_offs += (increase * unit_size_);

	if (it->chunk_offs > (chunk_size_ - 1)) {
		it->chunk_num++;
		it->chunk_offs -= chunk_size_;
		it->chunk = get_chunk_ptr_internal(it->chunk_num);
	}
}

void Segment::end_sample_iteration(SegmentDataIterator* it)
{
	delete it;

	iterator_count_--;

	if ((iterator_count_ == 0) && mem_optimization_requested_) {
		mem_optimization_requested_ = false;
		free_unused_memory();
	}
}

uint8_t* Segment::get_iterator_value(SegmentDataIterator* it)
{
	assert(it->sample_index <= (sample_count_ - 1));

	return (it->chunk + it->chunk_offs);
}

uint64_t Segment::get_iterator_valid_length(SegmentDataIterator* it)
{
	assert(it->sample_index <= (sample_count_ - 1));
	return ((chunk_size_ - it->chunk_offs) / unit_size_);

}

void Segment::set_channel_numner(uint16_t channel_num)
{
	channel_num_ = channel_num;
	unit_size_temp = (8.0 / channel_num_);
	if (data_zoom == NULL) {
		data_zoom = new(std::nothrow) uint8_t[chunk_size_ * unit_size_temp];
		if (data_zoom == NULL) {
			qDebug() << __FILE__ << " :" << __LINE__ << ":MALLOC MEMORY FAILED";
			// data_zoom is auxiliary buffer, not critical
		}
	}
}

void Segment::set_unit_size(uint16_t unit_size)
{
	unit_size_ = unit_size;
}

void Segment::mark_bad_memory()
{
	bad_memory_ = true;
	Q_EMIT memoryError();
}

uint64_t Segment::get_chunk_size() const
{
	return chunk_size_;
}

} // namespace data
} // namespace pv
