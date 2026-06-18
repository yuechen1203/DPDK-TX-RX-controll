#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dptx {

class TxEngine;

struct AppConfig {
    std::string system_name = "DPDK TX/RX 控制台";
    std::string auth_token = "dpdk-tx-secret";
    std::string listen_host = "0.0.0.0";
    int listen_port = 8080;
    bool enable_tls = false;
    std::string tls_cert_file;
    std::string tls_key_file;
    std::string pcap_root = "/data/pcap";
    bool pcap_recursive = false;
    int pcap_max_scan_files = 1024;
    std::vector<std::string> pcap_extensions{".pcap"};
    std::string core_list = "0-7";
    int main_lcore = 0;
    int mem_channels = 2;
    std::string file_prefix = "dpdk-tx";
    std::string proc_type = "primary";
    std::vector<std::string> device_list;
    std::vector<std::string> allow_ports;
    int tx_ring_size = 4096;
    int rx_ring_size = 1024;
    int tx_queue_per_port = 0;
    int rx_queue_per_port = 0;
    int mbuf_pool_size = 262143;
    int mbuf_cache_size = 512;
    int max_burst = 32;
    std::string rate_accounting = "wire";
    int stats_interval_ms = 1000;
    bool mock_initial_streams = false;
    std::string tx_engine_mode = "dpdk";
    std::string rx_pcap_dump_root = "/home/huang/rx_pcap";
    int rx_pcap_max_file_mb = 0;
    bool rx_pcap_stop_on_error = true;
    bool db_enabled = false;
    std::string db_endpoint = "127.0.0.1:3306";
    std::string db_user = "root";
    std::string db_password;
    std::string db_name = "dpdk_tx";
    std::string db_stream_table = "stream_history";
    std::string db_tx_stream_table = "tx_stream_history";
    std::string db_rx_stream_table = "rx_stream_history";
};

struct DeviceInfo {
    std::string pci;
    int port_id = 0;
    bool link_up = true;
    std::string link_speed;
    int socket_id = 0;
    std::string driver;
    int total_tx_queues = 7;
    int used_tx_queues = 0;
    int total_rx_queues = 7;
    int used_rx_queues = 0;
    bool available = true;
    std::string unavailable_reason;
    std::vector<std::string> streams;
    std::vector<std::string> tx_streams;
    std::vector<std::string> rx_streams;
};

struct CoreInfo {
    int id = 0;
    std::string status = "idle";
    std::string role = "空闲";
    std::string stream;
    int usage_percent = 0;
    bool available = true;
    std::string unavailable_reason;
};

struct StreamInfo {
    int id = 0;
    std::string name;
    std::string status;
    std::string direction = "tx";
    std::string mode;
    std::string tx_port;
    std::string rx_port;
    std::vector<int> cores;
    int target_mbps = 0;
    int actual_mbps = 0;
    double actual_pps = 0.0;
    double total_gb = 0.0;
    uint64_t packets = 0;
    uint64_t drops = 0;
    uint64_t errors = 0;
    uint64_t dump_files = 0;
    uint64_t dump_errors = 0;
    bool pcap_dump_enabled = false;
    std::string pcap_dump_dir;
    std::string config_json;
    std::vector<int> queues;
    std::vector<int> worker_rates_mbps;
    std::vector<int> worker_burst_bytes;
};

struct HistoryStreamInfo {
    long id = 0;
    std::string name;
    std::string direction = "tx";
    std::string mode;
    int rate_mbps = 0;
    std::string config_json;
    std::string saved_at;
};

struct PcapFileInfo {
    std::string name;
    std::string path;
    long packet_count = 0;
    int max_len = 0;
    int min_len = 0;
    int avg_len = 0;
    double size_mb = 0;
};

struct PortStats {
    std::string pci;
    int port_id = 0;
    int tx_mbps = 0;
    double tx_mpps = 0;
    double total_tb = 0;
    uint64_t tx_packets = 0;
    double tx_packets_m = 0;
    long tx_drops = 0;
    long tx_nombuf = 0;
    int rx_mbps = 0;
    double rx_mpps = 0.0;
    double rx_total_gb = 0.0;
    uint64_t rx_packets = 0;
    double rx_packets_m = 0.0;
    long rx_drops = 0;
    long rx_errors = 0;
    long rx_nombuf = 0;
};

struct StreamRuntimeStats {
    int actual_mbps = 0;
    double actual_pps = 0.0;
    double total_gb = 0.0;
    uint64_t packets = 0;
    uint64_t drops = 0;
    uint64_t errors = 0;
    uint64_t dump_files = 0;
    uint64_t dump_errors = 0;
};

struct CpuUsageSample {
    uint64_t idle = 0;
    uint64_t total = 0;
};

class RuntimeState {
public:
    explicit RuntimeState(AppConfig config);
    ~RuntimeState();

    std::string challenge() const;
    std::string expected_auth_md5() const;
    std::string create_session_token(const std::string& auth_md5);
    bool valid_session(const std::string& authorization) const;

    std::string runtime_json();
    std::string devices_json();
    std::string cores_json();
    std::string streams_json();
    std::string pcap_files_json();
    std::string stats_json();

    std::string create_stream(const std::string& body, int& status);
    std::string start_stream(int id, int& status);
    std::string stop_stream(int id, int& status);
    std::string delete_stream(int id, int& status);
    std::string reset_stats(std::optional<int> port_id);
    std::string refresh_resources(const std::string& target);
    std::string history_streams_json(const std::string& direction, int& status);
    std::string restore_history_stream(long history_id, const std::string& body, int& status);
    std::string restore_history_stream(const std::string& direction, long history_id, const std::string& body, int& status);
    std::string delete_history_stream(long history_id, int& status);
    std::string delete_history_stream(const std::string& direction, long history_id, int& status);
    void save_streams_to_database();

    const AppConfig& config() const { return config_; }

private:
    AppConfig config_;
    std::string challenge_ = "dpdk-tx-static-challenge";
    std::string session_token_;
    std::chrono::steady_clock::time_point started_at_;
    int next_stream_id_ = 4;
    std::unique_ptr<TxEngine> tx_engine_;

    std::vector<DeviceInfo> devices_;
    std::vector<CoreInfo> cores_;
    std::vector<StreamInfo> streams_;
    std::vector<PcapFileInfo> pcaps_;
    std::vector<PortStats> stats_;
    std::unordered_map<int, CpuUsageSample> cpu_usage_samples_;
    mutable std::mutex mutex_;

    void seed();
    void discover_devices_locked();
    void scan_pcap_files_locked();
    void initialize_cores_locked();
    void initialize_stats_locked();
    void update_cpu_usage_locked();
    void refresh_stats_locked();
    void refresh_engine_stats_locked();
    bool tx_engine_ready_locked() const;
    std::string tx_engine_status_locked() const;
    std::string tx_engine_message_locked() const;
    std::string uptime_locked() const;
    DeviceInfo* find_device_locked(const std::string& pci);
    StreamInfo* find_stream_locked(int id);
    PortStats* find_stats_locked(int port_id);
    std::vector<int> allocate_cores_locked(int count, const std::string& stream_name, bool running);
    std::vector<int> allocate_queues_locked(const std::string& pci, int count, const std::string& direction);
    void release_stream_resources_locked(const StreamInfo& stream);
    void sync_devices_from_streams_locked();
    void sync_core_state_locked();
    bool core_online_locked(int core_id) const;
};

AppConfig load_config(const std::string& path);

} // namespace dptx
