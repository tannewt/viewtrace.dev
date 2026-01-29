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

#include <cstdint>
#include <vector>

#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

void AppendBytes(std::vector<uint8_t>* buffer, const void* data, size_t size) {
  const uint8_t* ptr = static_cast<const uint8_t*>(data);
  buffer->insert(buffer->end(), ptr, ptr + size);
}

template <typename T>
void Append(std::vector<uint8_t>* buffer, const T& value) {
  AppendBytes(buffer, &value, sizeof(T));
}

TEST(SaleaeBinaryTraceReaderTest, ParsesDigitalV1) {
  std::vector<uint8_t> data;
  const char magic[] = "<SALEAE>";
  AppendBytes(&data, magic, sizeof(magic) - 1);
  int32_t version = 1;
  int32_t type = 0;
  int64_t chunk_count = 1;
  Append(&data, version);
  Append(&data, type);
  Append(&data, chunk_count);

  int32_t initial_state = 0;
  double sample_rate = 1e6;
  double begin_time = 0.0;
  double end_time = 2.0;
  int64_t transitions = 2;
  Append(&data, initial_state);
  Append(&data, sample_rate);
  Append(&data, begin_time);
  Append(&data, end_time);
  Append(&data, transitions);
  double t0 = 0.5;
  double t1 = 1.0;
  Append(&data, t0);
  Append(&data, t1);

  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  context.global_args_tracker =
      std::make_unique<GlobalArgsTracker>(context.storage.get());
  context.track_tracker = std::make_unique<TrackTracker>(&context);
  context.event_tracker = std::make_unique<EventTracker>(&context);

  SaleaeBinaryTraceReader reader(&context);
  TraceBlobView view(TraceBlob::CopyFrom(data.data(), data.size()));
  ASSERT_TRUE(reader.Parse(std::move(view)).ok());
  ASSERT_TRUE(reader.NotifyEndOfFile().ok());

  const auto& counters = context.storage->counter_table();
  ASSERT_EQ(counters.row_count(), 3u);
  EXPECT_EQ(counters[0].ts(), 0);
  EXPECT_EQ(counters[0].value(), 0);
  EXPECT_EQ(counters[1].ts(), 500000000);
  EXPECT_EQ(counters[1].value(), 1);
  EXPECT_EQ(counters[2].ts(), 1000000000);
  EXPECT_EQ(counters[2].value(), 0);
}

TEST(SaleaeBinaryTraceReaderTest, ParsesDigitalV0Sampled) {
  std::vector<uint8_t> data;
  const char magic[] = "<SALEAE>";
  AppendBytes(&data, magic, sizeof(magic) - 1);
  int32_t version = 0;
  int32_t type = 0;
  Append(&data, version);
  Append(&data, type);

  uint32_t initial_state = 0;
  double begin_time = 0.0;
  double end_time = 2.0;
  uint64_t num_transitions = 2;
  Append(&data, initial_state);
  Append(&data, begin_time);
  Append(&data, end_time);
  Append(&data, num_transitions);
  double t0 = 0.5;
  double t1 = 1.0;
  Append(&data, t0);
  Append(&data, t1);

  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  context.global_args_tracker =
      std::make_unique<GlobalArgsTracker>(context.storage.get());
  context.track_tracker = std::make_unique<TrackTracker>(&context);
  context.event_tracker = std::make_unique<EventTracker>(&context);

  SaleaeBinaryTraceReader reader(&context);
  TraceBlobView view(TraceBlob::CopyFrom(data.data(), data.size()));
  ASSERT_TRUE(reader.Parse(std::move(view)).ok());
  ASSERT_TRUE(reader.NotifyEndOfFile().ok());

  const auto& counters = context.storage->counter_table();
  ASSERT_EQ(counters.row_count(), 3u);
  EXPECT_EQ(counters[0].ts(), 0);
  EXPECT_EQ(counters[0].value(), 0);
  EXPECT_EQ(counters[1].ts(), 500000000);
  EXPECT_EQ(counters[1].value(), 1);
  EXPECT_EQ(counters[2].ts(), 1000000000);
  EXPECT_EQ(counters[2].value(), 0);
}

}  // namespace
}  // namespace perfetto::trace_processor
