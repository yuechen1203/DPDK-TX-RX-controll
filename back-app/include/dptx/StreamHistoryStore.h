#pragma once

#include "dptx/RuntimeState.h"

#include <string>
#include <optional>
#include <vector>

namespace dptx {

class StreamHistoryStore {
public:
    explicit StreamHistoryStore(const AppConfig& config, std::string direction = "tx");

    bool enabled() const;
    std::string last_error() const;
    bool save_streams(const std::vector<StreamInfo>& streams);
    std::vector<HistoryStreamInfo> load_streams(int limit = 200);
    std::optional<HistoryStreamInfo> load_stream(long id);
    bool delete_stream(long id);

private:
    AppConfig config_;
    std::string direction_;
    std::string last_error_;

    bool ensure_table(void* mysql);
    void* connect();
    std::string table_name() const;
};

} // namespace dptx
