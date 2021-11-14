// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "DataViews/DataView.h"

#include <absl/strings/str_replace.h>

#include <memory>

#include "OrbitBase/File.h"
#include "OrbitBase/Logging.h"

namespace orbit_data_views {

std::string FormatValueForCsv(std::string_view value) {
  std::string result;
  result.append("\"");
  result.append(absl::StrReplaceAll(value, {{"\"", "\"\""}}));
  result.append("\"");
  return result;
}

void DataView::InitSortingOrders() {
  sorting_orders_.clear();
  for (const auto& column : GetColumns()) {
    sorting_orders_.push_back(column.initial_order);
  }

  sorting_column_ = GetDefaultSortingColumn();
}

void DataView::OnSort(int column, std::optional<SortingOrder> new_order) {
  if (column < 0) {
    return;
  }

  if (!IsSortingAllowed()) {
    return;
  }

  if (sorting_orders_.empty()) {
    InitSortingOrders();
  }

  sorting_column_ = column;
  if (new_order.has_value()) {
    sorting_orders_[column] = new_order.value();
  }

  DoSort();
}

void DataView::OnFilter(const std::string& filter) {
  filter_ = filter;
  DoFilter();
  OnSort(sorting_column_, {});
}

void DataView::SetUiFilterString(const std::string& filter) {
  if (filter_callback_) {
    filter_callback_(filter);
  }
}

void DataView::OnDataChanged() {
  DoFilter();
  OnSort(sorting_column_, std::optional<SortingOrder>{});
}

const std::string DataView::kMenuActionCopySelection = "Copy Selection";
const std::string DataView::kMenuActionExportToCsv = "Export to CSV";

std::vector<std::vector<std::string>> DataView::GetContextMenuWithGrouping(
    int /*clicked_index*/, const std::vector<int>& selected_indices) {
  // GetContextmenuWithGrouping is called when OrbitTreeView::indexAt returns a valid index and
  // hence the selected_indices retrieved from OrbitTreeView::selectionModel()->selectedIndexes()
  // should not be empty.
  CHECK(!selected_indices.empty());

  static std::vector<std::string> default_group = {kMenuActionCopySelection,
                                                   kMenuActionExportToCsv};
  return {default_group};
}

void DataView::OnContextMenu(const std::string& action, int /*menu_index*/,
                             const std::vector<int>& item_indices) {
  if (action == kMenuActionExportToCsv) {
    std::string save_file = app_->GetSaveFile(".csv");
    if (!save_file.empty()) {
      auto result = ExportCsv(save_file);
      if (result.has_error()) {
        app_->SendErrorToUi("Export to CSV", result.error().message());
      }
    }
  } else if (action == kMenuActionCopySelection) {
    CopySelection(item_indices);
  }
}

std::vector<int> DataView::GetVisibleSelectedIndices() {
  std::vector<int> visible_selected_indices;
  for (size_t row = 0; row < indices_.size(); ++row) {
    if (selected_indices_.contains(indices_[row])) {
      visible_selected_indices.push_back(static_cast<int>(row));
    }
  }
  return visible_selected_indices;
}

ErrorMessageOr<void> DataView::ExportCsv(const std::filesystem::path& file_path) {
  ErrorMessageOr<orbit_base::unique_fd> result = orbit_base::OpenFileForWriting(file_path);
  if (result.has_error()) {
    return ErrorMessage{absl::StrFormat("Failed to open \"%s\" file: %s", file_path.string(),
                                        result.error().message())};
  }

  const orbit_base::unique_fd& fd = result.value();

  constexpr const char* kFieldSeparator = ",";
  // CSV RFC requires lines to end with CRLF
  constexpr const char* kLineSeparator = "\r\n";

  size_t num_columns = GetColumns().size();

  {
    std::string header_line;
    for (size_t i = 0; i < num_columns; ++i) {
      header_line.append(FormatValueForCsv(GetColumns()[i].header));
      if (i < num_columns - 1) header_line.append(kFieldSeparator);
    }

    header_line.append(kLineSeparator);
    auto write_result = orbit_base::WriteFully(fd, header_line);
    if (write_result.has_error()) {
      return ErrorMessage{absl::StrFormat("Error writing to \"%s\": %s", file_path.string(),
                                          write_result.error().message())};
    }
  }

  size_t num_elements = GetNumElements();
  for (size_t i = 0; i < num_elements; ++i) {
    std::string line;
    for (size_t j = 0; j < num_columns; ++j) {
      line.append(FormatValueForCsv(GetValueForCopy(i, j)));
      if (j < num_columns - 1) line.append(kFieldSeparator);
    }
    line.append(kLineSeparator);
    auto write_result = orbit_base::WriteFully(fd, line);
    if (write_result.has_error()) {
      return ErrorMessage{absl::StrFormat("Error writing to \"%s\": %s", file_path.string(),
                                          write_result.error().message())};
    }
  }

  return outcome::success();
}

void DataView::CopySelection(const std::vector<int>& selection) {
  constexpr const char* kFieldSeparator = "\t";
  constexpr const char* kLineSeparator = "\n";

  std::string clipboard;
  size_t num_columns = GetColumns().size();
  for (size_t i = 0; i < num_columns; ++i) {
    clipboard += GetColumns()[i].header;
    if (i < num_columns - 1) clipboard.append(kFieldSeparator);
  }
  clipboard.append(kLineSeparator);

  size_t num_elements = GetNumElements();
  for (size_t i : selection) {
    if (i >= num_elements) continue;
    for (size_t j = 0; j < num_columns; ++j) {
      clipboard += GetValueForCopy(i, j);
      if (j < num_columns - 1) clipboard.append(kFieldSeparator);
    }
    clipboard.append(kLineSeparator);
  }

  app_->SetClipboard(clipboard);
}

}  // namespace orbit_data_views