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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_SALEAE_SALEAE_CSV_TRACE_READER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_SALEAE_SALEAE_CSV_TRACE_READER_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

class SaleaeCsvTraceReader : public ChunkedTraceReader {
 public:
  explicit SaleaeCsvTraceReader(TraceProcessorContext* context);
  ~SaleaeCsvTraceReader() override;

  base::Status Parse(TraceBlobView blob) override;
 base::Status NotifyEndOfFile() override;

 private:
  struct TransactionState {
    bool open = false;
    int64_t start_ts_ns = 0;
    std::string address;
    bool read = false;
    std::vector<std::string> write_bytes;
    std::vector<std::string> read_bytes;
  };

  base::Status ParseLine(std::string line);
  base::Status ParseHeader(const std::string& line);
  base::Status ParseRow(const std::string& line);
  std::string BuildTransactionName(const TransactionState& state) const;
  int64_t SecondsToNs(double seconds) const;

  TraceProcessorContext* const context_;
  util::TraceBlobViewReader reader_;
  bool header_parsed_ = false;
  std::vector<std::string> columns_;
  std::vector<StringId> column_key_ids_;
  std::optional<size_t> name_col_;
  std::optional<size_t> type_col_;
  std::optional<size_t> start_time_col_;
  std::optional<size_t> duration_col_;
  std::optional<size_t> data_col_;
  std::optional<size_t> address_col_;
  std::optional<size_t> read_col_;
  base::FlatHashMap<std::string, TrackId, base::MurmurHash<std::string>>
      track_ids_;
  base::FlatHashMap<std::string, TransactionState,
                    base::MurmurHash<std::string>>
      transaction_state_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_SALEAE_SALEAE_CSV_TRACE_READER_H_
