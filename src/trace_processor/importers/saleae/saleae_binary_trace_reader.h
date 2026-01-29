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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_SALEAE_SALEAE_BINARY_TRACE_READER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_SALEAE_SALEAE_BINARY_TRACE_READER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

class SaleaeBinaryTraceReader : public ChunkedTraceReader {
 public:
  explicit SaleaeBinaryTraceReader(TraceProcessorContext* context);
  ~SaleaeBinaryTraceReader() override;

  base::Status Parse(TraceBlobView blob) override;
  base::Status NotifyEndOfFile() override;

 private:
  enum class DataType {
    kDigital,
    kAnalog,
  };

  base::Status ParseBuffer();
  base::Status ParseVersion0(DataType data_type, size_t* pos);
  base::Status ParseVersion1(DataType data_type, size_t* pos);
  base::Status ParseDigitalChunk(size_t* pos);
  base::Status ParseDigitalChunkV0(size_t* pos);
  base::Status ParseAnalogWaveformV0(size_t* pos);
  base::Status ParseAnalogWaveformV1(size_t* pos);

  int64_t SecondsToNs(double seconds) const;
  DataType ParseDataType(int32_t raw_type) const;

  TraceProcessorContext* const context_;
  std::vector<uint8_t> buffer_;
  std::optional<TrackId> track_id_;
  DataType data_type_ = DataType::kDigital;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_SALEAE_SALEAE_BINARY_TRACE_READER_H_
