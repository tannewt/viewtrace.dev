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

#include "src/trace_processor/importers/saleae/saleae_csv_trace_reader.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {
constexpr auto kSaleaeCsvBlueprint = tracks::SliceBlueprint(
    "saleae_csv",
    tracks::DimensionBlueprints(tracks::StringDimensionBlueprint("analyzer")),
    tracks::DynamicNameBlueprint());

std::string ToString(const TraceBlobView& view) {
  return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}

std::string JoinBytes(const std::vector<std::string>& bytes) {
  std::string out;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i > 0) {
      out += " ";
    }
    out += bytes[i];
  }
  return out;
}

std::vector<std::string> ParseCsvLine(base::StringView line) {
  std::vector<std::string> fields;
  std::string field;
  bool in_quotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line.at(i);
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < line.size() && line.at(i + 1) == '"') {
          field.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field.push_back(c);
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == ',') {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(c);
    }
  }
  fields.push_back(field);
  return fields;
}

std::string StripUtf8Bom(std::string value) {
  if (value.size() >= 3 && static_cast<uint8_t>(value[0]) == 0xEF &&
      static_cast<uint8_t>(value[1]) == 0xBB &&
      static_cast<uint8_t>(value[2]) == 0xBF) {
    value.erase(0, 3);
  }
  return value;
}

}  // namespace

SaleaeCsvTraceReader::SaleaeCsvTraceReader(TraceProcessorContext* context)
    : context_(context) {}

SaleaeCsvTraceReader::~SaleaeCsvTraceReader() = default;

base::Status SaleaeCsvTraceReader::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));
  for (;;) {
    auto it = reader_.GetIterator();
    auto line = it.MaybeFindAndRead('\n');
    if (!line) {
      return base::OkStatus();
    }
    std::string text = ToString(*line);
    reader_.PopFrontUntil(it.file_offset());
    RETURN_IF_ERROR(ParseLine(std::move(text)));
  }
}

base::Status SaleaeCsvTraceReader::NotifyEndOfFile() {
  if (reader_.avail() == 0) {
    return base::OkStatus();
  }
  auto start = reader_.start_offset();
  auto length = reader_.avail();
  auto remainder = reader_.SliceOff(start, length);
  reader_.PopFrontUntil(reader_.end_offset());
  if (!remainder || remainder->size() == 0) {
    return base::OkStatus();
  }
  return ParseLine(ToString(*remainder));
}

base::Status SaleaeCsvTraceReader::ParseLine(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (base::TrimWhitespace(line).empty()) {
    return base::OkStatus();
  }
  if (!header_parsed_) {
    return ParseHeader(line);
  }
  return ParseRow(line);
}

base::Status SaleaeCsvTraceReader::ParseHeader(const std::string& line) {
  std::vector<std::string> fields = ParseCsvLine(base::StringView(line));
  if (fields.empty()) {
    return base::ErrStatus("Saleae CSV header is empty");
  }
  columns_.clear();
  column_key_ids_.clear();
  columns_.reserve(fields.size());
  column_key_ids_.reserve(fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    std::string field = base::TrimWhitespace(fields[i]);
    if (i == 0) {
      field = StripUtf8Bom(std::move(field));
    }
    columns_.push_back(field);
    column_key_ids_.push_back(
        context_->storage->InternString(base::StringView(columns_.back())));
    std::string lower = base::ToLower(columns_.back());
    if (lower == "name") {
      name_col_ = i;
    } else if (lower == "type") {
      type_col_ = i;
    } else if (lower == "start_time" || lower == "start time") {
      start_time_col_ = i;
    } else if (lower == "duration") {
      duration_col_ = i;
    } else if (lower == "data") {
      data_col_ = i;
    } else if (lower == "address") {
      address_col_ = i;
    } else if (lower == "read") {
      read_col_ = i;
    }
  }
  if (!name_col_ || !type_col_ || !start_time_col_ || !duration_col_) {
    return base::ErrStatus(
        "Saleae CSV header missing required columns (name, type, start_time, "
        "duration)");
  }
  header_parsed_ = true;
  return base::OkStatus();
}

base::Status SaleaeCsvTraceReader::ParseRow(const std::string& line) {
  std::vector<std::string> fields = ParseCsvLine(base::StringView(line));
  if (fields.empty()) {
    return base::OkStatus();
  }
  if (fields.size() < columns_.size()) {
    fields.resize(columns_.size());
  }

  std::string analyzer = base::TrimWhitespace(fields[name_col_.value_or(0)]);
  if (analyzer.empty()) {
    analyzer = "Unknown";
  }
  std::string analyzer_lower = base::ToLower(analyzer);
  std::string type = base::TrimWhitespace(fields[type_col_.value_or(0)]);
  if (type.empty()) {
    type = "event";
  }
  std::string type_lower = base::ToLower(type);

  std::string start_str =
      base::TrimWhitespace(fields[start_time_col_.value_or(0)]);
  if (start_str.empty()) {
    return base::ErrStatus("Saleae CSV row missing start_time");
  }
  auto start_seconds = base::StringToDouble(start_str);
  if (!start_seconds) {
    return base::ErrStatus("Saleae CSV invalid start_time '%s'",
                           start_str.c_str());
  }

  std::string dur_str = base::TrimWhitespace(fields[duration_col_.value_or(0)]);
  double duration_seconds = 0.0;
  if (!dur_str.empty()) {
    auto dur_value = base::StringToDouble(dur_str);
    if (!dur_value) {
      return base::ErrStatus("Saleae CSV invalid duration '%s'",
                             dur_str.c_str());
    }
    duration_seconds = *dur_value;
  }

  int64_t ts_ns = SecondsToNs(*start_seconds);
  int64_t dur_ns = SecondsToNs(duration_seconds);

  auto track_it = track_ids_.Find(analyzer);
  TrackId track_id;
  if (track_it) {
    track_id = *track_it;
  } else {
    std::string track_name = "Saleae CSV: " + analyzer;
    StringId track_name_id =
        context_->storage->InternString(base::StringView(track_name));
    track_id = context_->track_tracker->InternTrack(
        kSaleaeCsvBlueprint, tracks::Dimensions(base::StringView(analyzer)),
        track_name_id);
    track_ids_.Insert(analyzer, track_id);
  }

  std::string event_name = type;
  if (data_col_) {
    std::string data_value =
        base::TrimWhitespace(fields[data_col_.value_or(0)]);
    if (!data_value.empty()) {
      event_name = data_value;
    }
  }
  if (event_name == type && address_col_) {
    std::string address_value =
        base::TrimWhitespace(fields[address_col_.value_or(0)]);
    if (!address_value.empty()) {
      event_name = address_value;
    }
  }

  StringId slice_name_id =
      context_->storage->InternString(base::StringView(event_name));
  StringId category_id = kNullStringId;
  if (type_lower == "data" || type_lower == "address") {
    category_id = context_->storage->InternString(base::StringView(type_lower));
  }

  struct ParsedArg {
    StringId key;
    Variadic value;
  };
  std::vector<ParsedArg> args;
  args.reserve(columns_.size());
  for (size_t i = 0; i < columns_.size() && i < fields.size(); ++i) {
    if (i == name_col_ || i == type_col_ || i == start_time_col_ ||
        i == duration_col_) {
      continue;
    }
    if (columns_[i].empty()) {
      continue;
    }
    std::string value = base::TrimWhitespace(fields[i]);
    if (value.empty()) {
      continue;
    }
    std::string lower = base::ToLower(value);
    if (lower == "true" || lower == "false") {
      args.push_back({column_key_ids_[i], Variadic::Boolean(lower == "true")});
    } else {
      StringId value_id =
          context_->storage->InternString(base::StringView(value));
      args.push_back({column_key_ids_[i], Variadic::String(value_id)});
    }
  }

  context_->slice_tracker->Scoped(
      ts_ns, track_id, category_id, slice_name_id, dur_ns,
      [args = std::move(args)](ArgsTracker::BoundInserter* inserter) {
        if (!inserter) {
          return;
        }
        for (const auto& arg : args) {
          inserter->AddArg(arg.key, arg.value);
        }
      });

  if (analyzer_lower == "i2c") {
    TransactionState* state = transaction_state_.Find(analyzer);
    if (!state) {
      auto [it, inserted] = transaction_state_.Insert(analyzer, {});
      state = it;
    }

    if (type_lower == "start") {
      if (!state->open) {
        state->open = true;
        state->start_ts_ns = ts_ns;
        state->address.clear();
        state->read = false;
        state->write_bytes.clear();
        state->read_bytes.clear();
      }
    } else if (type_lower == "address") {
      if (state->open && address_col_) {
        std::string address_value =
            base::TrimWhitespace(fields[address_col_.value_or(0)]);
        if (!address_value.empty()) {
          state->address = address_value;
        }
      }
      if (state->open && read_col_) {
        std::string read_value =
            base::TrimWhitespace(fields[read_col_.value_or(0)]);
        if (!read_value.empty()) {
          std::string read_lower = base::ToLower(read_value);
          if (read_lower == "true" || read_lower == "false") {
            state->read = read_lower == "true";
          }
        }
      }
    } else if (type_lower == "data") {
      if (state->open && data_col_) {
        std::string data_value =
            base::TrimWhitespace(fields[data_col_.value_or(0)]);
        if (!data_value.empty()) {
          if (state->read) {
            state->read_bytes.push_back(data_value);
          } else {
            state->write_bytes.push_back(data_value);
          }
        }
      }
    } else if (type_lower == "stop") {
      if (state->open) {
        StringId transaction_category_id =
            context_->storage->InternString(base::StringView("i2c"));
        std::string transaction_name = BuildTransactionName(*state);
        StringId transaction_name_id =
            context_->storage->InternString(base::StringView(transaction_name));
        StringId address_key_id =
            context_->storage->InternString(base::StringView("address"));
        StringId write_key_id =
            context_->storage->InternString(base::StringView("write_bytes"));
        StringId read_key_id =
            context_->storage->InternString(base::StringView("read_bytes"));
        StringId address_value_id = kNullStringId;
        StringId write_value_id = kNullStringId;
        StringId read_value_id = kNullStringId;
        if (!state->address.empty()) {
          address_value_id =
              context_->storage->InternString(base::StringView(state->address));
        }
        if (!state->write_bytes.empty()) {
          std::string joined = JoinBytes(state->write_bytes);
          write_value_id =
              context_->storage->InternString(base::StringView(joined));
        }
        if (!state->read_bytes.empty()) {
          std::string joined = JoinBytes(state->read_bytes);
          read_value_id =
              context_->storage->InternString(base::StringView(joined));
        }
        int64_t end_ts_ns = ts_ns + dur_ns;
        int64_t transaction_dur_ns = end_ts_ns - state->start_ts_ns;
        if (transaction_dur_ns < 0) {
          transaction_dur_ns = 0;
        }
        context_->slice_tracker->Scoped(
            state->start_ts_ns, track_id, transaction_category_id,
            transaction_name_id, transaction_dur_ns,
            [address_key_id, write_key_id, read_key_id, address_value_id,
             write_value_id,
             read_value_id](ArgsTracker::BoundInserter* inserter) {
              if (!inserter) {
                return;
              }
              if (!address_value_id.is_null()) {
                inserter->AddArg(address_key_id,
                                 Variadic::String(address_value_id));
              }
              if (!write_value_id.is_null()) {
                inserter->AddArg(write_key_id,
                                 Variadic::String(write_value_id));
              }
              if (!read_value_id.is_null()) {
                inserter->AddArg(read_key_id, Variadic::String(read_value_id));
              }
            });
        state->open = false;
      }
    }
  }

  return base::OkStatus();
}

std::string SaleaeCsvTraceReader::BuildTransactionName(
    const TransactionState& state) const {
  std::string name = state.address.empty() ? "i2c" : state.address;
  if (!state.write_bytes.empty()) {
    name += " W:";
    for (const auto& byte : state.write_bytes) {
      name += " ";
      name += byte;
    }
  }
  if (!state.read_bytes.empty()) {
    name += " R:";
    for (const auto& byte : state.read_bytes) {
      name += " ";
      name += byte;
    }
  }
  return name;
}

int64_t SaleaeCsvTraceReader::SecondsToNs(double seconds) const {
  return static_cast<int64_t>(std::llround(seconds * 1e9));
}

}  // namespace perfetto::trace_processor
