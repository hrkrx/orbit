// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DATA_VIEWS_LIVE_FUNCTIONS_DATA_VIEW_H_
#define DATA_VIEWS_LIVE_FUNCTIONS_DATA_VIEW_H_

#include <absl/container/flat_hash_map.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "DataViews/AppInterface.h"
#include "DataViews/DataView.h"
#include "DataViews/LiveFunctionsInterface.h"
#include "MetricsUploader/MetricsUploader.h"
#include "capture.pb.h"
#include "capture_data.pb.h"

namespace orbit_data_views {

class LiveFunctionsDataView : public DataView {
 public:
  explicit LiveFunctionsDataView(LiveFunctionsInterface* live_functions, AppInterface* app,
                                 orbit_metrics_uploader::MetricsUploader* metrics_uploader);

  const std::vector<Column>& GetColumns() override;
  int GetDefaultSortingColumn() override { return kColumnCount; }
  std::vector<std::vector<std::string>> GetContextMenuWithGrouping(
      int clicked_index, const std::vector<int>& selected_indices) override;
  std::string GetValue(int row, int column) override;
  // As we allow single selection on Live tab, this method returns either an empty vector or a
  // single-value vector.
  std::vector<int> GetVisibleSelectedIndices() override;
  void UpdateHighlightedFunctionId(const std::vector<int>& rows);
  void UpdateSelectedFunctionId();

  void OnSelect(const std::vector<int>& rows) override;
  void OnContextMenu(const std::string& action, int menu_index,
                     const std::vector<int>& item_indices) override;
  void OnDataChanged() override;
  void OnTimer() override;
  void OnRefresh(const std::vector<int>& visible_selected_indices,
                 const RefreshMode& mode) override;
  [[nodiscard]] bool ResetOnRefresh() const override { return false; }
  std::optional<int> GetRowFromFunctionId(uint64_t function_id);
  void AddFunction(uint64_t function_id, orbit_client_protos::FunctionInfo function_info);

  // Export all events (including the function name, thread name and id, start timestamp, end
  // timestamp, and duration) associated with the selected rows in to a CSV file.
  ErrorMessageOr<void> ExportAllEventsToCsv(const std::filesystem::path& file_path,
                                            const std::vector<int>& item_indices);

 protected:
  void DoFilter() override;
  void DoSort() override;
  [[nodiscard]] uint64_t GetInstrumentedFunctionId(uint32_t row) const;
  [[nodiscard]] const orbit_client_protos::FunctionInfo& GetInstrumentedFunction(
      uint32_t row) const;
  [[nodiscard]] std::optional<orbit_client_protos::FunctionInfo>
  CreateFunctionInfoFromInstrumentedFunction(
      const orbit_grpc_protos::InstrumentedFunction& instrumented_function);

  absl::flat_hash_map<uint64_t, orbit_client_protos::FunctionInfo> functions_{};

  LiveFunctionsInterface* live_functions_;
  uint64_t selected_function_id_;

  enum ColumnIndex {
    kColumnSelected,
    kColumnName,
    kColumnCount,
    kColumnTimeTotal,
    kColumnTimeAvg,
    kColumnTimeMin,
    kColumnTimeMax,
    kColumnStdDev,
    kColumnModule,
    kColumnAddress,
    kNumColumns
  };

  static const std::string kMenuActionSelect;
  static const std::string kMenuActionUnselect;
  static const std::string kMenuActionDisassembly;
  static const std::string kMenuActionSourceCode;
  static const std::string kMenuActionEnableFrameTrack;
  static const std::string kMenuActionDisableFrameTrack;
  static const std::string kMenuActionIterate;
  static const std::string kMenuActionJumpToFirst;
  static const std::string kMenuActionJumpToLast;
  static const std::string kMenuActionJumpToMin;
  static const std::string kMenuActionJumpToMax;
  static const std::string kMenuActionExportEventsToCsv;

 private:
  orbit_metrics_uploader::MetricsUploader* metrics_uploader_;
};

}  // namespace orbit_data_views

#endif  // DATA_VIEWS_LIVE_FUNCTIONS_DATA_VIEW_H_
