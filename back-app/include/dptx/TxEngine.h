#pragma once

#include "dptx/RuntimeState.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dptx {

struct TxEngineStatus {
    bool ready = false;
    bool dpdk_linked = false;
    std::string mode = "dpdk";
    std::string status = "disabled";
    std::string message;
};

class TxEngine {
public:
    virtual ~TxEngine() = default;

    virtual bool initialize(std::string& error) = 0;
    virtual void shutdown() = 0;
    virtual TxEngineStatus status() const = 0;

    virtual std::vector<DeviceInfo> discover_devices() = 0;
    virtual bool start_stream(const StreamInfo& stream, std::string& error) = 0;
    virtual bool stop_stream(int stream_id, std::string& error) = 0;
    virtual bool delete_stream(int stream_id, std::string& error) = 0;
    virtual void reset_stats(std::optional<int> port_id) = 0;
    virtual void snapshot_stats(std::vector<PortStats>& ports, std::unordered_map<int, StreamRuntimeStats>& stream_stats) = 0;
};

std::unique_ptr<TxEngine> create_tx_engine(const AppConfig& config);

} // namespace dptx
