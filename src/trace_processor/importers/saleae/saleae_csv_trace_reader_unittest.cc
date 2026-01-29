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

#include <cstring>

#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class SaleaeCsvTraceReaderTest : public ::testing::Test {
 protected:
  SaleaeCsvTraceReaderTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(context_.storage.get());
    context_.args_translation_table =
        std::make_unique<ArgsTranslationTable>(context_.storage.get());
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, 0);
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
  }

  TraceProcessorContext context_;
};

TEST_F(SaleaeCsvTraceReaderTest, ParsesCsvRows) {
  const char csv[] =
      "name,type,start_time,duration,data,ack,address,read\n"
      "\"I2C\",\"start\",0,0.000000002,,,,\n"
      "\"I2C\",\"address\",0.1,0.0000001,,true,0x20,false\n"
      "\"I2C\",\"data\",0.2,0.0000001,0x01,true,,\n"
      "\"I2C\",\"data\",0.25,0.0000001,0x02,true,,\n"
      "\"I2C\",\"stop\",0.3,0.000000002,,,,\n"
      "\"Async Serial\",\"data\",0.4,0.1,\"A\",,,\n";

  SaleaeCsvTraceReader reader(&context_);
  TraceBlobView view(TraceBlob::CopyFrom(csv, strlen(csv)));
  ASSERT_TRUE(reader.Parse(std::move(view)).ok());
  ASSERT_TRUE(reader.NotifyEndOfFile().ok());

  const auto& slices = context_.storage->slice_table();
  ASSERT_EQ(slices.row_count(), 7u);
  EXPECT_EQ(slices[0].ts(), 0);
  EXPECT_EQ(slices[0].dur(), 2);
  bool has_start = false;
  bool has_stop = false;
  bool has_address = false;
  bool has_data1 = false;
  bool has_data2 = false;
  bool has_async_data = false;
  bool has_transaction = false;
  for (uint32_t i = 0; i < slices.row_count(); ++i) {
    const auto& row = slices[i];
    if (!row.name().has_value()) {
      continue;
    }
    auto name = context_.storage->GetString(*row.name()).ToStdString();
    std::string category;
    if (row.category().has_value()) {
      category = context_.storage->GetString(*row.category()).ToStdString();
    }
    if (category == "i2c" && name == "0x20 W: 0x01 0x02") {
      has_transaction = true;
    } else if (category.empty() && name == "start") {
      has_start = true;
    } else if (category.empty() && name == "stop") {
      has_stop = true;
    } else if (category == "address" && name == "0x20") {
      has_address = true;
    } else if (category == "data" && name == "0x01") {
      has_data1 = true;
    } else if (category == "data" && name == "0x02") {
      has_data2 = true;
    } else if (category == "data" && name == "A") {
      has_async_data = true;
    }
  }
  EXPECT_TRUE(has_start);
  EXPECT_TRUE(has_stop);
  EXPECT_TRUE(has_address);
  EXPECT_TRUE(has_data1);
  EXPECT_TRUE(has_data2);
  EXPECT_TRUE(has_async_data);
  EXPECT_TRUE(has_transaction);

  const auto& tracks = context_.storage->track_table();
  ASSERT_EQ(tracks.row_count(), 2u);
  bool has_i2c = false;
  bool has_async = false;
  for (uint32_t i = 0; i < tracks.row_count(); ++i) {
    auto name = tracks[i].name();
    if (name.is_null()) {
      continue;
    }
    std::string value = context_.storage->GetString(name).ToStdString();
    if (value == "Saleae CSV: I2C") {
      has_i2c = true;
    }
    if (value == "Saleae CSV: Async Serial") {
      has_async = true;
    }
  }
  EXPECT_TRUE(has_i2c);
  EXPECT_TRUE(has_async);
}

}  // namespace
}  // namespace perfetto::trace_processor
