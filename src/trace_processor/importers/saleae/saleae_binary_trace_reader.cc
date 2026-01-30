/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/saleae/saleae_binary_trace_reader.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {
constexpr char kSaleaeMagic[] = "<SALEAE>";
constexpr uint32_t kSaleaeV0FileId = 0x00002f00;
constexpr int32_t kSaleaeVersion0 = 0;
constexpr int32_t kSaleaeVersion1 = 1;
constexpr int32_t kSaleaeDigitalType = 0;
constexpr int32_t kSaleaeAnalogType = 1;

constexpr auto kSaleaeDigitalBlueprint =
    tracks::CounterBlueprint("saleae_digital",
                             tracks::UnknownUnitBlueprint(),
                             tracks::DimensionBlueprints(),
                             tracks::StaticNameBlueprint("Saleae Digital"));

constexpr auto kSaleaeAnalogBlueprint =
    tracks::CounterBlueprint("saleae_analog",
                             tracks::UnknownUnitBlueprint(),
                             tracks::DimensionBlueprints(),
                             tracks::StaticNameBlueprint("Saleae Analog"));

template <typename T>
bool ReadField(const std::vector<uint8_t>& buffer, size_t* pos, T* out) {
  if (*pos + sizeof(T) > buffer.size()) {
    return false;
  }
  memcpy(out, buffer.data() + *pos, sizeof(T));
  *pos += sizeof(T);
  return true;
}

bool MatchesMagic(const std::vector<uint8_t>& buffer) {
  if (buffer.size() < sizeof(kSaleaeMagic) - 1) {
    return false;
  }
  return memcmp(buffer.data(), kSaleaeMagic, sizeof(kSaleaeMagic) - 1) == 0;
}

}  // namespace

SaleaeBinaryTraceReader::SaleaeBinaryTraceReader(TraceProcessorContext* context)
    : context_(context) {}

SaleaeBinaryTraceReader::~SaleaeBinaryTraceReader() = default;

base::Status SaleaeBinaryTraceReader::Parse(TraceBlobView blob) {
  buffer_.insert(buffer_.end(), blob.data(), blob.data() + blob.size());
  return base::OkStatus();
}

base::Status SaleaeBinaryTraceReader::NotifyEndOfFile() {
  if (buffer_.empty()) {
    return base::ErrStatus("Empty Saleae binary data");
  }
  return ParseBuffer();
}

SaleaeBinaryTraceReader::DataType SaleaeBinaryTraceReader::ParseDataType(
    int32_t raw_type) const {
  return raw_type == kSaleaeAnalogType ? DataType::kAnalog : DataType::kDigital;
}

int64_t SaleaeBinaryTraceReader::SecondsToNs(double seconds) const {
  return static_cast<int64_t>(std::llround(seconds * 1e9));
}

base::Status SaleaeBinaryTraceReader::ParseBuffer() {
  size_t pos = 0;
  if (MatchesMagic(buffer_)) {
    pos += sizeof(kSaleaeMagic) - 1;
    int32_t version = 0;
    int32_t raw_type = 0;
    if (!ReadField(buffer_, &pos, &version) ||
        !ReadField(buffer_, &pos, &raw_type)) {
      return base::ErrStatus("Saleae v1 header truncated");
    }
    if (raw_type != kSaleaeDigitalType && raw_type != kSaleaeAnalogType) {
      return base::ErrStatus("Unsupported Saleae data type %d", raw_type);
    }
    data_type_ = ParseDataType(raw_type);
    if (version == kSaleaeVersion1) {
      int64_t chunk_count = 0;
      if (!ReadField(buffer_, &pos, &chunk_count)) {
        return base::ErrStatus("Saleae v1 header truncated");
      }
      if (chunk_count < 0) {
        return base::ErrStatus("Invalid Saleae chunk count");
      }
      for (int64_t i = 0; i < chunk_count; ++i) {
        RETURN_IF_ERROR(ParseVersion1(data_type_, &pos));
      }
      return base::OkStatus();
    }
    if (version == kSaleaeVersion0) {
      RETURN_IF_ERROR(ParseVersion0(data_type_, &pos));
      return base::OkStatus();
    }
    return base::ErrStatus("Unsupported Saleae version %d", version);
  }

  int32_t file_id = 0;
  int32_t version = 0;
  int32_t raw_type = 0;
  if (!ReadField(buffer_, &pos, &file_id) ||
      !ReadField(buffer_, &pos, &version) ||
      !ReadField(buffer_, &pos, &raw_type)) {
    return base::ErrStatus("Saleae v0 header truncated");
  }
  if (static_cast<uint32_t>(file_id) != kSaleaeV0FileId ||
      version != kSaleaeVersion0) {
    return base::ErrStatus("Unsupported Saleae header");
  }
  if (raw_type != kSaleaeDigitalType && raw_type != kSaleaeAnalogType) {
    return base::ErrStatus("Unsupported Saleae data type %d", raw_type);
  }
  data_type_ = ParseDataType(raw_type);
  return ParseVersion0(data_type_, &pos);
}

base::Status SaleaeBinaryTraceReader::ParseVersion0(DataType data_type,
                                                    size_t* pos) {
  if (data_type == DataType::kDigital) {
    return ParseDigitalChunkV0(pos);
  }
  return ParseAnalogWaveformV0(pos);
}

base::Status SaleaeBinaryTraceReader::ParseVersion1(DataType data_type,
                                                    size_t* pos) {
  if (data_type == DataType::kDigital) {
    return ParseDigitalChunk(pos);
  }
  return ParseAnalogWaveformV1(pos);
}

base::Status SaleaeBinaryTraceReader::ParseDigitalChunk(size_t* pos) {
  int32_t initial_state_raw = 0;
  double sample_rate = 0.0;
  double begin_time = 0.0;
  double end_time = 0.0;
  int64_t num_transitions = 0;
  if (!ReadField(buffer_, pos, &initial_state_raw) ||
      !ReadField(buffer_, pos, &sample_rate) ||
      !ReadField(buffer_, pos, &begin_time) ||
      !ReadField(buffer_, pos, &end_time) ||
      !ReadField(buffer_, pos, &num_transitions)) {
    return base::ErrStatus("Saleae digital chunk truncated");
  }
  if (num_transitions < 0) {
    return base::ErrStatus("Saleae digital transition count invalid");
  }
  if (*pos + static_cast<size_t>(num_transitions) * sizeof(double) >
      buffer_.size()) {
    return base::ErrStatus("Saleae digital transitions truncated");
  }
  (void)sample_rate;
  (void)end_time;

  TrackId track_id = track_id_.value_or(context_->track_tracker->CreateTrack(
      kSaleaeDigitalBlueprint, tracks::Dimensions()));
  track_id_ = track_id;

  int state = initial_state_raw ? 1 : 0;
  context_->event_tracker->PushCounter(SecondsToNs(begin_time), state,
                                       track_id);

  for (int64_t i = 0; i < num_transitions; ++i) {
    double transition_time = 0.0;
    memcpy(&transition_time, buffer_.data() + *pos, sizeof(double));
    *pos += sizeof(double);
    state = 1 - state;
    context_->event_tracker->PushCounter(SecondsToNs(transition_time), state,
                                         track_id);
  }
  return base::OkStatus();
}

base::Status SaleaeBinaryTraceReader::ParseDigitalChunkV0(size_t* pos) {
  uint32_t initial_state_raw = 0;
  double begin_time = 0.0;
  double end_time = 0.0;
  uint64_t num_transitions = 0;
  if (!ReadField(buffer_, pos, &initial_state_raw) ||
      !ReadField(buffer_, pos, &begin_time) ||
      !ReadField(buffer_, pos, &end_time) ||
      !ReadField(buffer_, pos, &num_transitions)) {
    return base::ErrStatus("Saleae digital v0 header truncated");
  }
  if (*pos + static_cast<size_t>(num_transitions) * sizeof(double) >
      buffer_.size()) {
    return base::ErrStatus("Saleae digital v0 transitions truncated");
  }
  (void)end_time;

  TrackId track_id = track_id_.value_or(context_->track_tracker->CreateTrack(
      kSaleaeDigitalBlueprint, tracks::Dimensions()));
  track_id_ = track_id;

  int state = initial_state_raw ? 1 : 0;
  context_->event_tracker->PushCounter(SecondsToNs(begin_time), state,
                                       track_id);

  for (uint64_t i = 0; i < num_transitions; ++i) {
    double transition_time = 0.0;
    memcpy(&transition_time, buffer_.data() + *pos, sizeof(double));
    *pos += sizeof(double);
    state = 1 - state;
    context_->event_tracker->PushCounter(SecondsToNs(transition_time), state,
                                         track_id);
  }
  return base::OkStatus();
}

base::Status SaleaeBinaryTraceReader::ParseAnalogWaveformV1(size_t* pos) {
  uint64_t waveform_count = 0;
  if (!ReadField(buffer_, pos, &waveform_count)) {
    return base::ErrStatus("Saleae analog v1 header truncated");
  }

  TrackId track_id = track_id_.value_or(context_->track_tracker->CreateTrack(
      kSaleaeAnalogBlueprint, tracks::Dimensions()));
  track_id_ = track_id;

  for (uint64_t waveform = 0; waveform < waveform_count; ++waveform) {
    double begin_time = 0.0;
    double trigger_time = 0.0;
    double sample_rate = 0.0;
    int64_t downsample = 0;
    uint64_t num_samples = 0;
    if (!ReadField(buffer_, pos, &begin_time) ||
        !ReadField(buffer_, pos, &trigger_time) ||
        !ReadField(buffer_, pos, &sample_rate) ||
        !ReadField(buffer_, pos, &downsample) ||
        !ReadField(buffer_, pos, &num_samples)) {
      return base::ErrStatus("Saleae analog v1 waveform truncated");
    }
    if (sample_rate <= 0.0) {
      return base::ErrStatus("Saleae analog v1 sample rate invalid");
    }
    if (downsample <= 0) {
      return base::ErrStatus("Saleae analog v1 downsample invalid");
    }
    if (*pos + num_samples * sizeof(float) > buffer_.size()) {
      return base::ErrStatus("Saleae analog v1 samples truncated");
    }
    (void)trigger_time;

    const double step = static_cast<double>(downsample) / sample_rate;
    double current_time = begin_time;
    for (uint64_t i = 0; i < num_samples; ++i) {
      float sample = 0.0f;
      memcpy(&sample, buffer_.data() + *pos, sizeof(float));
      *pos += sizeof(float);
      context_->event_tracker->PushCounter(
          SecondsToNs(current_time), static_cast<double>(sample), track_id);
      current_time += step;
    }
  }
  return base::OkStatus();
}

base::Status SaleaeBinaryTraceReader::ParseAnalogWaveformV0(size_t* pos) {
  double begin_time = 0.0;
  uint64_t sample_rate = 0;
  uint64_t downsample = 0;
  uint64_t num_samples = 0;
  if (!ReadField(buffer_, pos, &begin_time) ||
      !ReadField(buffer_, pos, &sample_rate) ||
      !ReadField(buffer_, pos, &downsample) ||
      !ReadField(buffer_, pos, &num_samples)) {
    return base::ErrStatus("Saleae analog v0 header truncated");
  }
  if (sample_rate == 0) {
    return base::ErrStatus("Saleae analog v0 sample rate invalid");
  }
  if (downsample == 0) {
    return base::ErrStatus("Saleae analog v0 downsample invalid");
  }
  if (*pos + num_samples * sizeof(float) > buffer_.size()) {
    return base::ErrStatus("Saleae analog v0 samples truncated");
  }

  TrackId track_id = track_id_.value_or(context_->track_tracker->CreateTrack(
      kSaleaeAnalogBlueprint, tracks::Dimensions()));
  track_id_ = track_id;

  const double step =
      static_cast<double>(downsample) / static_cast<double>(sample_rate);
  double current_time = begin_time;
  for (uint64_t i = 0; i < num_samples; ++i) {
    float sample = 0.0f;
    memcpy(&sample, buffer_.data() + *pos, sizeof(float));
    *pos += sizeof(float);
    context_->event_tracker->PushCounter(SecondsToNs(current_time),
                                         static_cast<double>(sample), track_id);
    current_time += step;
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
