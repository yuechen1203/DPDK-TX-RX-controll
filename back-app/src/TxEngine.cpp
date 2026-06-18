#include "dptx/TxEngine.h"
#include "dptx/Utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

#ifdef DPTX_ENABLE_DPDK
#include <arpa/inet.h>
#include <cerrno>
#include <memory>
#include <netinet/in.h>
#include <set>
#include <vector>

extern "C" {
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>
}
#endif

namespace dptx {

namespace {

enum class SequenceMode {
    Fixed,
    Increment,
    Random
};

struct IpRangeConfig {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t step = 1;
    SequenceMode mode = SequenceMode::Fixed;
    bool valid = false;
};

struct PortRangeConfig {
    uint16_t start = 0;
    uint16_t end = 0;
    uint16_t step = 1;
    SequenceMode mode = SequenceMode::Increment;
};

struct ConstructMutation {
    bool enabled = false;
    bool udp = true;
    bool checksum_enabled = true;
    uint16_t ipv4_offset = 0;
    uint16_t l4_offset = 0;
    IpRangeConfig src_ip;
    IpRangeConfig dst_ip;
    PortRangeConfig src_port;
    PortRangeConfig dst_port;
};

struct PacketTemplate {
    std::vector<uint8_t> bytes;
    uint32_t l2_len = 0;
    uint64_t wire_bits = 0;
    ConstructMutation construct;
};

uint32_t read_u32(const unsigned char* bytes, bool little_endian) {
    if (little_endian) {
        return static_cast<uint32_t>(bytes[0]) |
               (static_cast<uint32_t>(bytes[1]) << 8) |
               (static_cast<uint32_t>(bytes[2]) << 16) |
               (static_cast<uint32_t>(bytes[3]) << 24);
    }
    return static_cast<uint32_t>(bytes[3]) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[0]) << 24);
}

uint64_t packet_wire_bits(size_t l2_len, const AppConfig& config) {
    const size_t overhead = config.rate_accounting == "wire" ? 20 : 0;
    return static_cast<uint64_t>(l2_len + overhead) * 8ULL;
}

PacketTemplate make_packet_template(std::vector<uint8_t> bytes, const AppConfig& config) {
    if (bytes.size() < 60) {
        bytes.resize(60, 0);
    }
    PacketTemplate packet;
    packet.l2_len = static_cast<uint32_t>(bytes.size());
    packet.wire_bits = packet_wire_bits(packet.l2_len, config);
    packet.bytes = std::move(bytes);
    return packet;
}

std::vector<int> parse_core_list_value(const std::string& value) {
    std::vector<int> cores;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) {
            continue;
        }
        auto dash = item.find('-');
        if (dash == std::string::npos) {
            cores.push_back(std::stoi(item));
            continue;
        }
        int start = std::stoi(item.substr(0, dash));
        int end = std::stoi(item.substr(dash + 1));
        if (start > end) {
            std::swap(start, end);
        }
        for (int core = start; core <= end; ++core) {
            cores.push_back(core);
        }
    }
    std::sort(cores.begin(), cores.end());
    cores.erase(std::unique(cores.begin(), cores.end()), cores.end());
    return cores;
}

std::vector<PacketTemplate> load_classic_pcap(std::ifstream& file, const std::filesystem::path& path, bool little_endian, const AppConfig& config, std::string& error) {
    std::vector<PacketTemplate> packets;
    unsigned char packet_header[16]{};

    while (file.read(reinterpret_cast<char*>(packet_header), sizeof(packet_header))) {
        uint32_t incl_len = read_u32(packet_header + 8, little_endian);
        if (incl_len == 0) {
            continue;
        }
        if (incl_len > 256 * 1024 * 1024) {
            error = "invalid pcap packet length: " + path.string();
            return {};
        }
        std::vector<uint8_t> bytes(incl_len);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            break;
        }
        packets.push_back(make_packet_template(std::move(bytes), config));
    }
    return packets;
}

std::vector<PacketTemplate> load_pcapng(std::ifstream& file, const std::filesystem::path& path, const AppConfig& config, std::string& error) {
    std::vector<PacketTemplate> packets;
    file.clear();
    file.seekg(0, std::ios::beg);

    bool little_endian = true;
    bool have_section = false;

    for (;;) {
        unsigned char header[8]{};
        if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) {
            break;
        }

        uint32_t block_type = read_u32(header, little_endian);
        if (!have_section && header[0] == 0x0a && header[1] == 0x0d && header[2] == 0x0d && header[3] == 0x0a) {
            block_type = 0x0A0D0D0A;
        }

        uint32_t block_total_len = read_u32(header + 4, little_endian);
        if (block_type == 0x0A0D0D0A) {
            unsigned char bom[4]{};
            if (!file.read(reinterpret_cast<char*>(bom), sizeof(bom))) {
                break;
            }
            if (bom[0] == 0x4d && bom[1] == 0x3c && bom[2] == 0x2b && bom[3] == 0x1a) {
                little_endian = true;
                block_total_len = read_u32(header + 4, true);
            } else if (bom[0] == 0x1a && bom[1] == 0x2b && bom[2] == 0x3c && bom[3] == 0x4d) {
                little_endian = false;
                block_total_len = read_u32(header + 4, false);
            } else {
                error = "invalid pcapng byte order: " + path.string();
                return {};
            }
            have_section = true;
            if (block_total_len < 28) {
                error = "invalid pcapng section length: " + path.string();
                return {};
            }
            file.seekg(static_cast<std::streamoff>(block_total_len) - 12, std::ios::cur);
            continue;
        }

        if (block_total_len < 12 || block_total_len > 256 * 1024 * 1024) {
            error = "invalid pcapng block length: " + path.string();
            return {};
        }

        std::vector<unsigned char> body(block_total_len - 12);
        if (!file.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(body.size()))) {
            break;
        }
        file.seekg(4, std::ios::cur);

        uint32_t captured_len = 0;
        size_t data_offset = 0;
        if (block_type == 0x00000006 && body.size() >= 20) {
            captured_len = read_u32(body.data() + 12, little_endian);
            data_offset = 20;
        } else if (block_type == 0x00000003 && body.size() >= 4) {
            captured_len = read_u32(body.data(), little_endian);
            data_offset = 4;
        } else if (block_type == 0x00000002 && body.size() >= 20) {
            captured_len = read_u32(body.data() + 12, little_endian);
            data_offset = 20;
        } else {
            continue;
        }

        if (captured_len == 0 || data_offset + captured_len > body.size()) {
            continue;
        }
        std::vector<uint8_t> bytes(captured_len);
        std::memcpy(bytes.data(), body.data() + data_offset, captured_len);
        packets.push_back(make_packet_template(std::move(bytes), config));
    }

    return packets;
}

std::vector<PacketTemplate> load_packet_templates(const std::string& path_value, const AppConfig& config, std::string& error) {
    const std::filesystem::path path(path_value);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "pcap file open failed: " + path.string();
        return {};
    }

    unsigned char global_header[24]{};
    file.read(reinterpret_cast<char*>(global_header), sizeof(global_header));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(global_header))) {
        error = "pcap file too small: " + path.string();
        return {};
    }

    const bool classic_little = global_header[0] == 0xd4 && global_header[1] == 0xc3 && global_header[2] == 0xb2 && global_header[3] == 0xa1;
    const bool classic_big = global_header[0] == 0xa1 && global_header[1] == 0xb2 && global_header[2] == 0xc3 && global_header[3] == 0xd4;
    const bool nano_little = global_header[0] == 0x4d && global_header[1] == 0x3c && global_header[2] == 0xb2 && global_header[3] == 0xa1;
    const bool nano_big = global_header[0] == 0xa1 && global_header[1] == 0xb2 && global_header[2] == 0x3c && global_header[3] == 0x4d;
    const bool pcapng = global_header[0] == 0x0a && global_header[1] == 0x0d && global_header[2] == 0x0d && global_header[3] == 0x0a;

    std::vector<PacketTemplate> packets;
    if (pcapng) {
        packets = load_pcapng(file, path, config, error);
    } else if (classic_little || nano_little) {
        packets = load_classic_pcap(file, path, true, config, error);
    } else if (classic_big || nano_big) {
        packets = load_classic_pcap(file, path, false, config, error);
    } else {
        error = "unsupported pcap format: " + path.string();
        return {};
    }

    if (packets.empty() && error.empty()) {
        error = "pcap contains no packet: " + path.string();
    }
    return packets;
}

class UnavailableTxEngine final : public TxEngine {
public:
    explicit UnavailableTxEngine(AppConfig config) : config_(std::move(config)) {}

    bool initialize(std::string& error) override {
        status_.status = "not_built";
        status_.message = "当前程序未以 ENABLE_DPDK=ON 构建，不能创建真实 TX/RX stream";
        error = status_.message;
        return false;
    }

    void shutdown() override {}

    TxEngineStatus status() const override {
        return status_;
    }

    std::vector<DeviceInfo> discover_devices() override {
        return {};
    }

    bool start_stream(const StreamInfo&, std::string& error) override {
        error = status_.message;
        return false;
    }

    bool stop_stream(int, std::string&) override {
        return true;
    }

    bool delete_stream(int, std::string&) override {
        return true;
    }

    void reset_stats(std::optional<int>) override {}

    void snapshot_stats(std::vector<PortStats>& ports, std::unordered_map<int, StreamRuntimeStats>& stream_stats) override {
        stream_stats.clear();
        for (auto& row : ports) {
            row.tx_mbps = 0;
            row.tx_mpps = 0.0;
            row.rx_mbps = 0;
            row.rx_mpps = 0.0;
        }
    }

private:
    AppConfig config_;
    TxEngineStatus status_;
};

#ifdef DPTX_ENABLE_DPDK

struct WorkerCounters {
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> drops{0};
    std::atomic<uint64_t> no_mbuf{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> dump_files{0};
    std::atomic<uint64_t> dump_errors{0};
};

struct PcapDumpWriter {
    std::ofstream file;
    std::filesystem::path path;
    uint64_t bytes_written = 0;

    bool open(const std::filesystem::path& output_path, std::string& error) {
        path = output_path;
        bytes_written = 0;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "create rx pcap dump directory failed: " + ec.message();
            return false;
        }
        file.open(path, std::ios::binary);
        if (!file) {
            error = "open rx pcap dump file failed: " + path.string();
            return false;
        }

        struct PcapGlobalHeader {
            uint32_t magic = 0xa1b2c3d4;
            uint16_t version_major = 2;
            uint16_t version_minor = 4;
            int32_t thiszone = 0;
            uint32_t sigfigs = 0;
            uint32_t snaplen = 65535;
            uint32_t network = 1;
        } header;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        bytes_written += sizeof(header);
        return static_cast<bool>(file);
    }

    bool write_packet(struct rte_mbuf* mbuf) {
        if (!file) {
            return false;
        }
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        const uint32_t pkt_len = rte_pktmbuf_pkt_len(mbuf);
        struct PcapPacketHeader {
            uint32_t ts_sec = 0;
            uint32_t ts_usec = 0;
            uint32_t incl_len = 0;
            uint32_t orig_len = 0;
        } header;
        header.ts_sec = static_cast<uint32_t>(ts.tv_sec);
        header.ts_usec = static_cast<uint32_t>(ts.tv_nsec / 1000);
        header.incl_len = pkt_len;
        header.orig_len = pkt_len;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        bytes_written += sizeof(header);

        for (struct rte_mbuf* seg = mbuf; seg != nullptr; seg = seg->next) {
            const uint16_t len = rte_pktmbuf_data_len(seg);
            const char* data = rte_pktmbuf_mtod(seg, const char*);
            file.write(data, len);
            bytes_written += len;
            if (!file) {
                return false;
            }
        }
        return static_cast<bool>(file);
    }

    void close() {
        if (file) {
            file.flush();
            file.close();
        }
    }
};

struct BurstPacer {
    uint64_t tsc_hz = 1;
    uint64_t rate_bps = 1;
    uint64_t burst_bytes = 2048;
    uint64_t burst_bits = 2048 * 8ULL;
    uint64_t cycles_per_burst = 1;
    uint64_t cycle_stamp = 0;
    int64_t remaining_bits = 0;

    void reset(uint64_t bps, uint64_t configured_burst_bytes) {
        tsc_hz = rte_get_tsc_hz();
        rate_bps = std::max<uint64_t>(1, bps);
        burst_bytes = std::clamp<uint64_t>(configured_burst_bytes, 2048, 16 * 1024);
        burst_bits = burst_bytes * 8ULL;
        const auto cycles = (static_cast<unsigned __int128>(burst_bits) * tsc_hz + rate_bps - 1) / rate_bps;
        cycles_per_burst = static_cast<uint64_t>(std::max<unsigned __int128>(1, cycles));
        cycle_stamp = rte_get_tsc_cycles();
        remaining_bits = static_cast<int64_t>(burst_bits);
    }

    void consume(uint64_t bits) {
        remaining_bits -= static_cast<int64_t>(std::min<uint64_t>(bits, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
        while (remaining_bits <= 0) {
            const uint64_t target = cycle_stamp + cycles_per_burst;
            while (static_cast<int64_t>(rte_get_tsc_cycles() - target) < 0) {
            }
            cycle_stamp = rte_get_tsc_cycles();
            remaining_bits += static_cast<int64_t>(burst_bits);
        }
    }
};

struct WorkerContext {
    int stream_id = 0;
    unsigned lcore_id = 0;
    uint16_t port_id = 0;
    uint16_t queue_id = 0;
    uint64_t rate_bps = 0;
    uint16_t max_burst = 32;
    uint32_t burst_bytes = 2048;
    struct rte_mempool* pool = nullptr;
    std::shared_ptr<std::vector<PacketTemplate>> packets;
    std::atomic_bool running{false};
    WorkerCounters counters;
};

struct RxWorkerContext {
    int stream_id = 0;
    std::string stream_name;
    unsigned lcore_id = 0;
    uint16_t port_id = 0;
    uint16_t queue_id = 0;
    uint16_t max_burst = 32;
    bool pcap_dump_enabled = false;
    bool pcap_stop_on_error = true;
    uint64_t pcap_max_file_bytes = 0;
    std::filesystem::path dump_root;
    std::filesystem::path dump_path;
    std::atomic_bool running{false};
    WorkerCounters counters;
};

struct StreamContext {
    int id = 0;
    std::string direction = "tx";
    bool running = false;
    std::shared_ptr<std::vector<PacketTemplate>> packets;
    std::vector<std::unique_ptr<WorkerContext>> workers;
    std::vector<std::unique_ptr<RxWorkerContext>> rx_workers;
    std::vector<int> worker_rates_mbps;
    std::vector<int> worker_burst_bytes;
    uint64_t last_bytes = 0;
    uint64_t last_packets = 0;
    std::chrono::steady_clock::time_point last_sample = std::chrono::steady_clock::now();
};

SequenceMode parse_sequence_mode(const std::string& text, SequenceMode fallback) {
    std::string value = text;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "random" || value == "rand" || value == "随机") {
        return SequenceMode::Random;
    }
    if (value == "increment" || value == "inc" || value == "递增") {
        return SequenceMode::Increment;
    }
    if (value == "fixed" || value == "固定") {
        return SequenceMode::Fixed;
    }
    return fallback;
}

bool parse_ipv4_host(const std::string& text, uint32_t& out_host) {
    in_addr addr{};
    if (::inet_pton(AF_INET, text.c_str(), &addr) != 1) {
        return false;
    }
    out_host = ntohl(addr.s_addr);
    return true;
}

IpRangeConfig parse_ip_range_config(const std::string& json, const std::string& prefix, const std::string& fallback_ip) {
    std::string ip_text = extract_json_string(json, prefix + "_ip_addr");
    int mask = extract_json_int(json, prefix + "_ip_mask", -1);
    if (ip_text.empty()) {
        std::string legacy = extract_json_string(json, prefix + "_ip", fallback_ip);
        auto slash = legacy.find('/');
        if (slash != std::string::npos) {
            ip_text = legacy.substr(0, slash);
            if (mask < 0) {
                try {
                    mask = std::stoi(legacy.substr(slash + 1));
                } catch (...) {
                    mask = 32;
                }
            }
        } else {
            ip_text = legacy;
        }
    }
    mask = std::clamp(mask < 0 ? 32 : mask, 0, 32);

    IpRangeConfig config;
    config.mode = parse_sequence_mode(extract_json_string(json, prefix + "_ip_mode", "fixed"), SequenceMode::Fixed);
    config.step = static_cast<uint32_t>(std::max(1, extract_json_int(json, prefix + "_ip_step", 1)));

    uint32_t ip_host = 0;
    if (!parse_ipv4_host(ip_text, ip_host)) {
        return config;
    }
    config.valid = true;

    if (config.mode == SequenceMode::Fixed) {
        config.start = ip_host;
        config.end = ip_host;
        return config;
    }

    const uint32_t mask_value = mask == 0 ? 0 : (0xffffffffu << (32 - mask));
    config.start = ip_host & mask_value;
    config.end = config.start | ~mask_value;
    return config;
}

PortRangeConfig parse_port_range_config(const std::string& json, const std::string& prefix, int fallback_start) {
    int start = extract_json_int(json, prefix + "_port_start", fallback_start);
    int end = extract_json_int(json, prefix + "_port_end", start);
    if (start > end) {
        std::swap(start, end);
    }

    PortRangeConfig config;
    config.start = static_cast<uint16_t>(std::clamp(start, 0, 65535));
    config.end = static_cast<uint16_t>(std::clamp(end, 0, 65535));
    config.step = static_cast<uint16_t>(std::clamp(extract_json_int(json, prefix + "_port_step", 1), 1, 65535));
    config.mode = parse_sequence_mode(extract_json_string(json, prefix + "_port_mode", "increment"), SequenceMode::Increment);
    return config;
}

std::vector<uint8_t> parse_hex_payload(const std::string& text) {
    std::vector<uint8_t> bytes;
    std::stringstream ss(text);
    std::string item;
    while (ss >> item) {
        try {
            bytes.push_back(static_cast<uint8_t>(std::stoul(item, nullptr, 16) & 0xff));
        } catch (...) {
            bytes.clear();
            break;
        }
    }
    if (bytes.empty() && !text.empty()) {
        bytes.assign(text.begin(), text.end());
    }
    return bytes;
}

bool parse_mac(const std::string& text, struct rte_ether_addr& out) {
    std::stringstream ss(text);
    std::string item;
    int index = 0;
    while (std::getline(ss, item, ':')) {
        if (index >= RTE_ETHER_ADDR_LEN) {
            return false;
        }
        try {
            out.addr_bytes[index++] = static_cast<uint8_t>(std::stoul(item, nullptr, 16) & 0xff);
        } catch (...) {
            return false;
        }
    }
    return index == RTE_ETHER_ADDR_LEN;
}

bool parse_ipv4(const std::string& text, uint32_t& out_be) {
    auto slash = text.find('/');
    auto ip = slash == std::string::npos ? text : text.substr(0, slash);
    uint32_t host = 0;
    if (!parse_ipv4_host(ip, host)) {
        return false;
    }
    out_be = htonl(host);
    return true;
}

std::vector<PacketTemplate> build_construct_templates(const StreamInfo& stream, const AppConfig& config, std::string& error) {
    const std::string l3 = extract_json_string(stream.config_json, "l3", "IPv4");
    const std::string l4 = extract_json_string(stream.config_json, "l4", "UDP");
    if (l3 != "IPv4") {
        error = "construct mode currently supports IPv4 only";
        return {};
    }

    struct rte_ether_addr src_mac{};
    struct rte_ether_addr dst_mac{};
    if (!parse_mac(extract_json_string(stream.config_json, "src_mac", "02:00:00:00:00:01"), src_mac) ||
        !parse_mac(extract_json_string(stream.config_json, "dst_mac", "02:00:00:00:00:02"), dst_mac)) {
        error = "invalid mac address";
        return {};
    }

    const bool udp = l4 != "TCP";
    const uint16_t l4_len = udp ? sizeof(struct rte_udp_hdr) : sizeof(struct rte_tcp_hdr);
    ConstructMutation mutation;
    mutation.enabled = true;
    mutation.udp = udp;
    mutation.checksum_enabled = extract_json_bool(stream.config_json, "checksum_enabled", true);
    mutation.ipv4_offset = static_cast<uint16_t>(sizeof(struct rte_ether_hdr));
    mutation.l4_offset = static_cast<uint16_t>(sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    mutation.src_ip = parse_ip_range_config(stream.config_json, "src", "192.168.0.1");
    mutation.dst_ip = parse_ip_range_config(stream.config_json, "dst", "192.168.0.2");
    mutation.src_port = parse_port_range_config(stream.config_json, "src", 10000);
    mutation.dst_port = parse_port_range_config(stream.config_json, "dst", 53);
    if (!mutation.src_ip.valid || !mutation.dst_ip.valid) {
        error = "invalid IPv4 address";
        return {};
    }

    int payload_len = std::max(0, extract_json_int(stream.config_json, "payload_len", 64));
    const size_t min_payload = 60 - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr) - l4_len;
    payload_len = std::max(payload_len, static_cast<int>(min_payload));

    std::vector<uint8_t> payload = parse_hex_payload(extract_json_string(stream.config_json, "payload", ""));
    if (payload.size() < static_cast<size_t>(payload_len)) {
        payload.resize(static_cast<size_t>(payload_len), 0);
    } else if (payload.size() > static_cast<size_t>(payload_len)) {
        payload.resize(static_cast<size_t>(payload_len));
    }

    const size_t frame_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + l4_len + payload.size();
    std::vector<uint8_t> frame(frame_len, 0);
    auto* eth = reinterpret_cast<struct rte_ether_hdr*>(frame.data());
    eth->src_addr = src_mac;
    eth->dst_addr = dst_mac;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    auto* ip = reinterpret_cast<struct rte_ipv4_hdr*>(frame.data() + sizeof(struct rte_ether_hdr));
    ip->version_ihl = RTE_IPV4_VHL_DEF;
    ip->type_of_service = 0;
    ip->total_length = rte_cpu_to_be_16(static_cast<uint16_t>(sizeof(struct rte_ipv4_hdr) + l4_len + payload.size()));
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = 64;
    ip->next_proto_id = udp ? IPPROTO_UDP : IPPROTO_TCP;
    ip->src_addr = rte_cpu_to_be_32(mutation.src_ip.start);
    ip->dst_addr = rte_cpu_to_be_32(mutation.dst_ip.start);

    const uint16_t src_port = mutation.src_port.start;
    const uint16_t dst_port = mutation.dst_port.start;
    auto* l4_hdr = frame.data() + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
    if (udp) {
        auto* udp_hdr = reinterpret_cast<struct rte_udp_hdr*>(l4_hdr);
        udp_hdr->src_port = rte_cpu_to_be_16(src_port);
        udp_hdr->dst_port = rte_cpu_to_be_16(dst_port);
        udp_hdr->dgram_len = rte_cpu_to_be_16(static_cast<uint16_t>(sizeof(struct rte_udp_hdr) + payload.size()));
        std::memcpy(l4_hdr + sizeof(struct rte_udp_hdr), payload.data(), payload.size());
        udp_hdr->dgram_cksum = 0;
        if (mutation.checksum_enabled) {
            udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp_hdr);
        }
    } else {
        auto* tcp_hdr = reinterpret_cast<struct rte_tcp_hdr*>(l4_hdr);
        tcp_hdr->src_port = rte_cpu_to_be_16(src_port);
        tcp_hdr->dst_port = rte_cpu_to_be_16(dst_port);
        tcp_hdr->data_off = static_cast<uint8_t>((sizeof(struct rte_tcp_hdr) / 4) << 4);
        tcp_hdr->tcp_flags = RTE_TCP_SYN_FLAG;
        tcp_hdr->rx_win = rte_cpu_to_be_16(8192);
        std::memcpy(l4_hdr + sizeof(struct rte_tcp_hdr), payload.data(), payload.size());
        tcp_hdr->cksum = 0;
        if (mutation.checksum_enabled) {
            tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip, tcp_hdr);
        }
    }
    ip->hdr_checksum = 0;
    if (mutation.checksum_enabled) {
        ip->hdr_checksum = rte_ipv4_cksum(ip);
    }

    std::vector<PacketTemplate> templates;
    auto packet = make_packet_template(std::move(frame), config);
    packet.construct = mutation;
    templates.push_back(std::move(packet));
    return templates;
}

uint64_t next_random_u64(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

uint32_t next_ip_value(const IpRangeConfig& config, uint32_t& cursor, uint64_t& rng) {
    if (config.mode == SequenceMode::Fixed || config.start >= config.end) {
        return config.start;
    }

    const uint64_t span = static_cast<uint64_t>(config.end) - config.start + 1ULL;
    if (config.mode == SequenceMode::Random) {
        return static_cast<uint32_t>(config.start + (next_random_u64(rng) % span));
    }

    const uint32_t value = cursor < config.start || cursor > config.end ? config.start : cursor;
    const uint64_t offset = (static_cast<uint64_t>(value) - config.start + std::max<uint32_t>(1, config.step)) % span;
    cursor = static_cast<uint32_t>(config.start + offset);
    return value;
}

uint16_t next_port_value(const PortRangeConfig& config, uint16_t& cursor, uint64_t& rng) {
    if (config.start >= config.end) {
        return config.start;
    }

    const uint32_t span = static_cast<uint32_t>(config.end) - config.start + 1U;
    if (config.mode == SequenceMode::Random) {
        return static_cast<uint16_t>(config.start + (next_random_u64(rng) % span));
    }

    const uint16_t value = cursor < config.start || cursor > config.end ? config.start : cursor;
    const uint32_t offset = (static_cast<uint32_t>(value) - config.start + std::max<uint16_t>(1, config.step)) % span;
    cursor = static_cast<uint16_t>(config.start + offset);
    return value;
}

struct ConstructCursor {
    uint32_t src_ip = 0;
    uint32_t dst_ip = 0;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint64_t rng = 0;
};

ConstructCursor make_construct_cursor(const ConstructMutation& construct, unsigned lcore_id, int stream_id) {
    ConstructCursor cursor;
    cursor.src_ip = construct.src_ip.start;
    cursor.dst_ip = construct.dst_ip.start;
    cursor.src_port = construct.src_port.start;
    cursor.dst_port = construct.dst_port.start;
    cursor.rng = 0x9e3779b97f4a7c15ULL ^
        (static_cast<uint64_t>(lcore_id) << 32) ^
        (static_cast<uint64_t>(stream_id) << 16) ^
        rte_get_tsc_cycles();
    if (cursor.rng == 0) {
        cursor.rng = 0x9e3779b97f4a7c15ULL;
    }
    return cursor;
}

void apply_construct_mutation(const PacketTemplate& packet, ConstructCursor& cursor, void* data) {
    const auto& construct = packet.construct;
    if (!construct.enabled || packet.l2_len < construct.l4_offset) {
        return;
    }

    auto* bytes = static_cast<uint8_t*>(data);
    auto* ip = reinterpret_cast<struct rte_ipv4_hdr*>(bytes + construct.ipv4_offset);
    ip->src_addr = rte_cpu_to_be_32(next_ip_value(construct.src_ip, cursor.src_ip, cursor.rng));
    ip->dst_addr = rte_cpu_to_be_32(next_ip_value(construct.dst_ip, cursor.dst_ip, cursor.rng));
    ip->hdr_checksum = 0;

    auto* l4_hdr = bytes + construct.l4_offset;
    const uint16_t src_port = rte_cpu_to_be_16(next_port_value(construct.src_port, cursor.src_port, cursor.rng));
    const uint16_t dst_port = rte_cpu_to_be_16(next_port_value(construct.dst_port, cursor.dst_port, cursor.rng));
    if (construct.udp) {
        auto* udp = reinterpret_cast<struct rte_udp_hdr*>(l4_hdr);
        udp->src_port = src_port;
        udp->dst_port = dst_port;
        udp->dgram_cksum = 0;
        if (construct.checksum_enabled) {
            udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
        }
    } else {
        auto* tcp = reinterpret_cast<struct rte_tcp_hdr*>(l4_hdr);
        tcp->src_port = src_port;
        tcp->dst_port = dst_port;
        tcp->cksum = 0;
        if (construct.checksum_enabled) {
            tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);
        }
    }
    if (construct.checksum_enabled) {
        ip->hdr_checksum = rte_ipv4_cksum(ip);
    }
}

int dpdk_worker_main(void* arg) {
    auto* worker = static_cast<WorkerContext*>(arg);
    BurstPacer pacer;
    pacer.reset(worker->rate_bps, static_cast<uint64_t>(worker->burst_bytes));

    std::vector<struct rte_mbuf*> bufs(worker->max_burst, nullptr);
    std::vector<uint32_t> lens(worker->max_burst, 0);
    std::vector<ConstructCursor> construct_cursors;
    construct_cursors.reserve(worker->packets->size());
    for (const auto& packet : *worker->packets) {
        construct_cursors.push_back(make_construct_cursor(packet.construct, worker->lcore_id, worker->stream_id));
    }
    size_t packet_index = 0;
    bool running = worker->running.load(std::memory_order_relaxed);

    while (running) {
        uint16_t count = 0;
        while (count < worker->max_burst) {
            const size_t template_index = packet_index++ % worker->packets->size();
            const auto& packet = (*worker->packets)[template_index];
            struct rte_mbuf* mbuf = rte_pktmbuf_alloc(worker->pool);
            if (!mbuf) {
                worker->counters.no_mbuf.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            void* data = rte_pktmbuf_append(mbuf, packet.l2_len);
            if (!data) {
                rte_pktmbuf_free(mbuf);
                worker->counters.no_mbuf.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            std::memcpy(data, packet.bytes.data(), packet.l2_len);
            apply_construct_mutation(packet, construct_cursors[template_index], data);
            mbuf->port = worker->port_id;
            bufs[count] = mbuf;
            lens[count] = packet.l2_len;
            ++count;
            pacer.consume(packet.wire_bits);
        }

        if (count == 0) {
            running = worker->running.load(std::memory_order_relaxed);
            continue;
        }

        const uint16_t sent = rte_eth_tx_burst(worker->port_id, worker->queue_id, bufs.data(), count);
        uint64_t sent_bytes = 0;
        for (uint16_t i = 0; i < sent; ++i) {
            sent_bytes += lens[i];
        }
        worker->counters.packets.fetch_add(sent, std::memory_order_relaxed);
        worker->counters.bytes.fetch_add(sent_bytes, std::memory_order_relaxed);

        if (sent < count) {
            worker->counters.drops.fetch_add(count - sent, std::memory_order_relaxed);
            rte_pktmbuf_free_bulk(&bufs[sent], count - sent);
        }

        running = worker->running.load(std::memory_order_relaxed);
    }
    return 0;
}

std::string safe_file_part(std::string value) {
    for (char& c : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
        if (!ok) {
            c = '_';
        }
    }
    return value.empty() ? "stream" : value;
}

std::string timestamp_for_filename() {
    std::time_t now = std::time(nullptr);
    std::tm tm_value{};
    localtime_r(&now, &tm_value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm_value);
    return buffer;
}

std::filesystem::path rx_dump_path(const RxWorkerContext& worker) {
    std::ostringstream name;
    name << "stream-" << worker.stream_id
         << "_" << safe_file_part(worker.stream_name)
         << "_port-" << worker.port_id
         << "_queue-" << worker.queue_id
         << "_lcore-" << worker.lcore_id
         << "_" << timestamp_for_filename()
         << ".pcap";
    return worker.dump_root / name.str();
}

int dpdk_rx_worker_main(void* arg) {
    auto* worker = static_cast<RxWorkerContext*>(arg);
    std::vector<struct rte_mbuf*> bufs(worker->max_burst, nullptr);
    PcapDumpWriter writer;
    bool dump_active = false;

    if (worker->pcap_dump_enabled) {
        worker->dump_path = rx_dump_path(*worker);
        std::string error;
        dump_active = writer.open(worker->dump_path, error);
        if (dump_active) {
            worker->counters.dump_files.fetch_add(1, std::memory_order_relaxed);
        } else {
            worker->counters.dump_errors.fetch_add(1, std::memory_order_relaxed);
            if (worker->pcap_stop_on_error) {
                worker->running.store(false, std::memory_order_release);
            }
        }
    }

    bool running = worker->running.load(std::memory_order_relaxed);
    while (running) {
        const uint16_t count = rte_eth_rx_burst(worker->port_id, worker->queue_id, bufs.data(), worker->max_burst);
        if (count == 0) {
            running = worker->running.load(std::memory_order_relaxed);
            continue;
        }

        uint64_t bytes = 0;
        for (uint16_t i = 0; i < count; ++i) {
            struct rte_mbuf* mbuf = bufs[i];
            bytes += rte_pktmbuf_pkt_len(mbuf);
            if (dump_active) {
                if (!writer.write_packet(mbuf)) {
                    worker->counters.dump_errors.fetch_add(1, std::memory_order_relaxed);
                    dump_active = false;
                    writer.close();
                    if (worker->pcap_stop_on_error) {
                        worker->running.store(false, std::memory_order_release);
                    }
                } else if (worker->pcap_max_file_bytes > 0 && writer.bytes_written >= worker->pcap_max_file_bytes) {
                    writer.close();
                    worker->dump_path = rx_dump_path(*worker);
                    std::string error;
                    dump_active = writer.open(worker->dump_path, error);
                    if (dump_active) {
                        worker->counters.dump_files.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        worker->counters.dump_errors.fetch_add(1, std::memory_order_relaxed);
                        if (worker->pcap_stop_on_error) {
                            worker->running.store(false, std::memory_order_release);
                        }
                    }
                }
            }
            rte_pktmbuf_free(mbuf);
        }

        worker->counters.packets.fetch_add(count, std::memory_order_relaxed);
        worker->counters.bytes.fetch_add(bytes, std::memory_order_relaxed);
        running = worker->running.load(std::memory_order_relaxed);
    }
    writer.close();
    return 0;
}

class DpdkTxEngine final : public TxEngine {
public:
    explicit DpdkTxEngine(AppConfig config) : config_(std::move(config)) {
        status_.dpdk_linked = true;
        status_.mode = config_.tx_engine_mode;
    }

    ~DpdkTxEngine() override {
        shutdown();
    }

    bool initialize(std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) {
            return status_.ready;
        }

        std::vector<std::string> args;
        args.push_back("dpdk-tx-eal");
        args.push_back("-l");
        args.push_back(config_.core_list);
        args.push_back("-n");
        args.push_back(std::to_string(config_.mem_channels));
        args.push_back("--file-prefix");
        args.push_back(config_.file_prefix);
        args.push_back("--proc-type");
        args.push_back(config_.proc_type);
        for (const auto& pci : config_.device_list) {
            args.push_back("-a");
            args.push_back(pci);
        }

        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }

        int rc = rte_eal_init(static_cast<int>(argv.size()), argv.data());
        if (rc < 0) {
            status_.ready = false;
            status_.status = "eal_failed";
            status_.message = "rte_eal_init failed";
            error = status_.message;
            initialized_ = true;
            return false;
        }

        pool_ = rte_pktmbuf_pool_create(
            "dptx_mbuf_pool",
            static_cast<unsigned int>(config_.mbuf_pool_size),
            static_cast<unsigned int>(config_.mbuf_cache_size),
            0,
            RTE_MBUF_DEFAULT_BUF_SIZE,
            rte_socket_id());
        if (!pool_) {
            status_.ready = false;
            status_.status = "mempool_failed";
            status_.message = "rte_pktmbuf_pool_create failed";
            error = status_.message;
            initialized_ = true;
            return false;
        }

        if (!setup_ports_locked(error)) {
            initialized_ = true;
            return false;
        }

        status_.ready = true;
        status_.status = "running";
        status_.message = "DPDK TX/RX 引擎已就绪，统计来自软件 worker 计数";
        initialized_ = true;
        return true;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : streams_) {
            stop_stream_locked(item.first);
        }
        streams_.clear();
        for (const auto& item : ports_) {
            rte_eth_dev_stop(static_cast<uint16_t>(item.first));
        }
        status_.ready = false;
        if (initialized_ && status_.status == "running") {
            status_.status = "stopped";
            status_.message = "DPDK TX 引擎已停止";
        }
    }

    TxEngineStatus status() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    std::vector<DeviceInfo> discover_devices() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DeviceInfo> devices;
        for (const auto& item : ports_) {
            devices.push_back(item.second.device);
        }
        return devices;
    }

    bool start_stream(const StreamInfo& stream, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.ready) {
            error = status_.message;
            return false;
        }
        auto existing = streams_.find(stream.id);
        if (existing != streams_.end()) {
            if (existing->second->running) {
                return true;
            }
            if (existing->second->direction == "rx") {
                return launch_rx_stream_locked(*existing->second, error);
            }
            return launch_stream_locked(*existing->second, stream.worker_rates_mbps, stream.worker_burst_bytes, stream.target_mbps, error);
        }

        const std::string direction = stream.direction == "rx" ? "rx" : "tx";
        const std::string port_name = direction == "rx" ? (stream.rx_port.empty() ? stream.tx_port : stream.rx_port) : stream.tx_port;
        auto port_id = find_port_id_locked(port_name);
        if (!port_id) {
            error = (direction == "rx" ? "rx" : "tx") + std::string(" port is not initialized by DPDK");
            return false;
        }
        if (stream.cores.empty() || stream.queues.size() != stream.cores.size()) {
            error = "stream core/queue allocation is invalid";
            return false;
        }

        if (direction == "rx") {
            auto context = std::make_unique<StreamContext>();
            context->id = stream.id;
            context->direction = "rx";
            context->last_sample = std::chrono::steady_clock::now();
            for (size_t i = 0; i < stream.cores.size(); ++i) {
                auto worker = std::make_unique<RxWorkerContext>();
                worker->stream_id = stream.id;
                worker->stream_name = stream.name;
                worker->lcore_id = static_cast<unsigned>(stream.cores[i]);
                worker->port_id = *port_id;
                worker->queue_id = static_cast<uint16_t>(stream.queues[i]);
                worker->max_burst = static_cast<uint16_t>(std::clamp(config_.max_burst, 1, 1024));
                worker->pcap_dump_enabled = stream.pcap_dump_enabled;
                worker->pcap_stop_on_error = config_.rx_pcap_stop_on_error;
                worker->pcap_max_file_bytes = config_.rx_pcap_max_file_mb <= 0
                    ? 0
                    : static_cast<uint64_t>(config_.rx_pcap_max_file_mb) * 1024ULL * 1024ULL;
                worker->dump_root = stream.pcap_dump_dir.empty() ? config_.rx_pcap_dump_root : stream.pcap_dump_dir;
                context->rx_workers.push_back(std::move(worker));
            }
            if (!launch_rx_stream_locked(*context, error)) {
                return false;
            }
            streams_[stream.id] = std::move(context);
            return true;
        }

        std::shared_ptr<std::vector<PacketTemplate>> packets;
        if (stream.mode == "pcap") {
            auto loaded = load_packet_templates(extract_json_string(stream.config_json, "pcap_path"), config_, error);
            if (loaded.empty()) {
                return false;
            }
            packets = std::make_shared<std::vector<PacketTemplate>>(std::move(loaded));
        } else {
            auto generated = build_construct_templates(stream, config_, error);
            if (generated.empty()) {
                return false;
            }
            packets = std::make_shared<std::vector<PacketTemplate>>(std::move(generated));
        }

        auto context = std::make_unique<StreamContext>();
        context->id = stream.id;
        context->direction = "tx";
        context->packets = packets;
        context->worker_rates_mbps = stream.worker_rates_mbps;
        context->worker_burst_bytes = stream.worker_burst_bytes;
        context->last_sample = std::chrono::steady_clock::now();

        for (size_t i = 0; i < stream.cores.size(); ++i) {
            auto worker = std::make_unique<WorkerContext>();
            worker->stream_id = stream.id;
            worker->lcore_id = static_cast<unsigned>(stream.cores[i]);
            worker->port_id = *port_id;
            worker->queue_id = static_cast<uint16_t>(stream.queues[i]);
            worker->max_burst = static_cast<uint16_t>(std::clamp(config_.max_burst, 1, 1024));
            worker->burst_bytes = i < stream.worker_burst_bytes.size()
                ? static_cast<uint32_t>(std::clamp(stream.worker_burst_bytes[i], 2048, 16 * 1024))
                : 2048U;
            worker->pool = pool_;
            worker->packets = packets;
            context->workers.push_back(std::move(worker));
        }

        if (!launch_stream_locked(*context, stream.worker_rates_mbps, stream.worker_burst_bytes, stream.target_mbps, error)) {
            return false;
        }
        streams_[stream.id] = std::move(context);
        return true;
    }

    bool stop_stream(int stream_id, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_stream_locked(stream_id)) {
            error = "stream not found";
            return false;
        }
        return true;
    }

    bool delete_stream(int stream_id, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iter = streams_.find(stream_id);
        if (iter != streams_.end()) {
            stop_stream_locked(stream_id);
            archive_stream_totals_locked(*iter->second);
            streams_.erase(iter);
        }
        error.clear();
        return true;
    }

    void reset_stats(std::optional<int> port_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto totals = collect_port_totals_locked();
        const auto now = std::chrono::steady_clock::now();
        for (auto& item : ports_) {
            if (port_id && item.second.device.port_id != *port_id) {
                continue;
            }
            rte_eth_stats_reset(static_cast<uint16_t>(item.first));
            const auto current = totals.find(item.first);
            const auto& total = current == totals.end() ? zero_port_totals_ : current->second;
            item.second.reset_tx_bytes = total.tx_bytes;
            item.second.reset_tx_packets = total.tx_packets;
            item.second.reset_tx_drops = total.tx_drops;
            item.second.reset_rx_bytes = total.rx_bytes;
            item.second.reset_rx_packets = total.rx_packets;
            item.second.reset_rx_drops = total.rx_drops;
            item.second.reset_rx_errors = total.rx_errors;
            item.second.reset_rx_nombuf = total.rx_nombuf;
            item.second.last_tx_bytes = total.tx_bytes;
            item.second.last_tx_packets = total.tx_packets;
            item.second.last_rx_bytes = total.rx_bytes;
            item.second.last_rx_packets = total.rx_packets;
            item.second.last_sample = now;
        }
    }

    void snapshot_stats(std::vector<PortStats>& ports, std::unordered_map<int, StreamRuntimeStats>& stream_stats) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_stats.clear();
        if (!status_.ready) {
            for (auto& row : ports) {
                row.tx_mbps = 0;
                row.tx_mpps = 0.0;
                row.rx_mbps = 0;
                row.rx_mpps = 0.0;
            }
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto port_totals = collect_port_totals_locked();
        for (auto& row : ports) {
            auto port = find_port_runtime_locked(row.pci);
            if (!port) {
                row.tx_mbps = 0;
                row.tx_mpps = 0;
                row.rx_mbps = 0;
                row.rx_mpps = 0;
                continue;
            }
            const auto current = port_totals.find(port->device.port_id);
            const auto& total = current == port_totals.end() ? zero_port_totals_ : current->second;
            const double elapsed = std::max(0.001, std::chrono::duration<double>(now - port->last_sample).count());
            const uint64_t tx_byte_delta = total.tx_bytes >= port->last_tx_bytes ? total.tx_bytes - port->last_tx_bytes : 0;
            const uint64_t tx_packet_delta = total.tx_packets >= port->last_tx_packets ? total.tx_packets - port->last_tx_packets : 0;
            const uint64_t rx_byte_delta = total.rx_bytes >= port->last_rx_bytes ? total.rx_bytes - port->last_rx_bytes : 0;
            const uint64_t rx_packet_delta = total.rx_packets >= port->last_rx_packets ? total.rx_packets - port->last_rx_packets : 0;
            row.tx_mbps = static_cast<int>(std::llround((static_cast<double>(tx_byte_delta) * 8.0) / elapsed / 1000000.0));
            row.tx_mpps = (static_cast<double>(tx_packet_delta) / elapsed) / 1000000.0;
            row.total_tb = static_cast<double>(total.tx_bytes >= port->reset_tx_bytes ? total.tx_bytes - port->reset_tx_bytes : 0) / 1024.0 / 1024.0 / 1024.0 / 1024.0;
            row.tx_packets_m = static_cast<double>(total.tx_packets >= port->reset_tx_packets ? total.tx_packets - port->reset_tx_packets : 0) / 1000000.0;
            row.tx_drops = static_cast<long>(total.tx_drops >= port->reset_tx_drops ? total.tx_drops - port->reset_tx_drops : 0);
            row.rx_mbps = static_cast<int>(std::llround((static_cast<double>(rx_byte_delta) * 8.0) / elapsed / 1000000.0));
            row.rx_mpps = (static_cast<double>(rx_packet_delta) / elapsed) / 1000000.0;
            row.rx_total_gb = static_cast<double>(total.rx_bytes >= port->reset_rx_bytes ? total.rx_bytes - port->reset_rx_bytes : 0) / 1024.0 / 1024.0 / 1024.0;
            row.rx_packets_m = static_cast<double>(total.rx_packets >= port->reset_rx_packets ? total.rx_packets - port->reset_rx_packets : 0) / 1000000.0;
            row.rx_drops = static_cast<long>(total.rx_drops >= port->reset_rx_drops ? total.rx_drops - port->reset_rx_drops : 0);
            row.rx_errors = static_cast<long>(total.rx_errors >= port->reset_rx_errors ? total.rx_errors - port->reset_rx_errors : 0);
            row.rx_nombuf = static_cast<long>(total.rx_nombuf >= port->reset_rx_nombuf ? total.rx_nombuf - port->reset_rx_nombuf : 0);
            port->last_tx_bytes = total.tx_bytes;
            port->last_tx_packets = total.tx_packets;
            port->last_rx_bytes = total.rx_bytes;
            port->last_rx_packets = total.rx_packets;
            port->last_sample = now;
        }

        for (auto& item : streams_) {
            auto& context = *item.second;
            uint64_t total_bytes = 0;
            uint64_t total_packets = 0;
            uint64_t total_drops = 0;
            uint64_t total_errors = 0;
            uint64_t total_dump_files = 0;
            uint64_t total_dump_errors = 0;
            if (context.direction == "rx") {
                for (const auto& worker : context.rx_workers) {
                    total_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
                    total_packets += worker->counters.packets.load(std::memory_order_relaxed);
                    total_drops += worker->counters.drops.load(std::memory_order_relaxed);
                    total_errors += worker->counters.errors.load(std::memory_order_relaxed);
                    total_dump_files += worker->counters.dump_files.load(std::memory_order_relaxed);
                    total_dump_errors += worker->counters.dump_errors.load(std::memory_order_relaxed);
                }
            } else {
                for (const auto& worker : context.workers) {
                    total_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
                    total_packets += worker->counters.packets.load(std::memory_order_relaxed);
                    total_drops += worker->counters.drops.load(std::memory_order_relaxed);
                    total_errors += worker->counters.errors.load(std::memory_order_relaxed);
                }
            }
            const double elapsed = std::max(0.001, std::chrono::duration<double>(now - context.last_sample).count());
            const uint64_t byte_delta = total_bytes >= context.last_bytes ? total_bytes - context.last_bytes : 0;
            const uint64_t packet_delta = total_packets >= context.last_packets ? total_packets - context.last_packets : 0;
            StreamRuntimeStats snapshot;
            snapshot.actual_mbps = context.running ? static_cast<int>(std::llround((static_cast<double>(byte_delta) * 8.0) / elapsed / 1000000.0)) : 0;
            snapshot.actual_pps = context.running ? static_cast<double>(packet_delta) / elapsed : 0.0;
            snapshot.total_gb = static_cast<double>(total_bytes) / 1024.0 / 1024.0 / 1024.0;
            snapshot.packets = total_packets;
            snapshot.drops = total_drops;
            snapshot.errors = total_errors;
            snapshot.dump_files = total_dump_files;
            snapshot.dump_errors = total_dump_errors;
            stream_stats[context.id] = snapshot;
            context.last_bytes = total_bytes;
            context.last_packets = total_packets;
            context.last_sample = now;
        }
    }

private:
    struct PortTotals {
        uint64_t tx_bytes = 0;
        uint64_t tx_packets = 0;
        uint64_t tx_drops = 0;
        uint64_t rx_bytes = 0;
        uint64_t rx_packets = 0;
        uint64_t rx_drops = 0;
        uint64_t rx_errors = 0;
        uint64_t rx_nombuf = 0;
    };

    struct PortRuntime {
        DeviceInfo device;
        uint64_t last_tx_bytes = 0;
        uint64_t last_tx_packets = 0;
        uint64_t last_rx_bytes = 0;
        uint64_t last_rx_packets = 0;
        uint64_t reset_tx_bytes = 0;
        uint64_t reset_tx_packets = 0;
        uint64_t reset_tx_drops = 0;
        uint64_t reset_rx_bytes = 0;
        uint64_t reset_rx_packets = 0;
        uint64_t reset_rx_drops = 0;
        uint64_t reset_rx_errors = 0;
        uint64_t reset_rx_nombuf = 0;
        uint64_t archived_tx_bytes = 0;
        uint64_t archived_tx_packets = 0;
        uint64_t archived_tx_drops = 0;
        uint64_t archived_rx_bytes = 0;
        uint64_t archived_rx_packets = 0;
        uint64_t archived_rx_drops = 0;
        uint64_t archived_rx_errors = 0;
        uint64_t archived_rx_nombuf = 0;
        std::chrono::steady_clock::time_point last_sample = std::chrono::steady_clock::now();
    };

    AppConfig config_;
    mutable std::mutex mutex_;
    bool initialized_ = false;
    TxEngineStatus status_;
    struct rte_mempool* pool_ = nullptr;
    PortTotals zero_port_totals_;
    std::unordered_map<int, PortRuntime> ports_;
    std::unordered_map<int, std::unique_ptr<StreamContext>> streams_;

    std::optional<uint16_t> find_port_id_locked(const std::string& pci) const {
        for (const auto& item : ports_) {
            if (item.second.device.pci == pci) {
                return static_cast<uint16_t>(item.first);
            }
        }
        return std::nullopt;
    }

    PortRuntime* find_port_runtime_locked(const std::string& pci) {
        for (auto& item : ports_) {
            if (item.second.device.pci == pci) {
                return &item.second;
            }
        }
        return nullptr;
    }

    bool setup_ports_locked(std::string& error) {
        std::set<std::string> allow(config_.device_list.begin(), config_.device_list.end());
        const auto cores = parse_core_list_value(config_.core_list);
        const int worker_count = std::max(1, static_cast<int>(cores.size()) - 1);
        const uint16_t tx_queues = static_cast<uint16_t>(config_.tx_queue_per_port > 0 ? config_.tx_queue_per_port : worker_count);
        const uint16_t rx_queues = static_cast<uint16_t>(config_.rx_queue_per_port > 0 ? config_.rx_queue_per_port : worker_count);

        uint16_t port_id = 0;
        RTE_ETH_FOREACH_DEV(port_id) {
            char name[RTE_ETH_NAME_MAX_LEN]{};
            rte_eth_dev_get_name_by_port(port_id, name);
            const std::string pci = name[0] ? name : ("port-" + std::to_string(port_id));
            if (!allow.empty() && allow.find(pci) == allow.end()) {
                continue;
            }

            struct rte_eth_conf port_conf{};
            port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
            uint16_t rx_desc = static_cast<uint16_t>(std::max(64, config_.rx_ring_size));
            uint16_t tx_desc = static_cast<uint16_t>(std::max(64, config_.tx_ring_size));
            int rc = rte_eth_dev_configure(port_id, rx_queues, tx_queues, &port_conf);
            if (rc < 0) {
                error = "rte_eth_dev_configure failed for " + pci;
                status_.status = "port_config_failed";
                status_.message = error;
                return false;
            }
            rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_desc, &tx_desc);
            const int socket_id = rte_eth_dev_socket_id(port_id);
            for (uint16_t queue = 0; queue < rx_queues; ++queue) {
                rc = rte_eth_rx_queue_setup(port_id, queue, rx_desc, socket_id, nullptr, pool_);
                if (rc < 0) {
                    error = "rte_eth_rx_queue_setup failed for " + pci + " queue " + std::to_string(queue);
                    status_.status = "queue_setup_failed";
                    status_.message = error;
                    return false;
                }
            }
            for (uint16_t queue = 0; queue < tx_queues; ++queue) {
                rc = rte_eth_tx_queue_setup(port_id, queue, tx_desc, socket_id, nullptr);
                if (rc < 0) {
                    error = "rte_eth_tx_queue_setup failed for " + pci + " queue " + std::to_string(queue);
                    status_.status = "queue_setup_failed";
                    status_.message = error;
                    return false;
                }
            }
            rc = rte_eth_dev_start(port_id);
            if (rc < 0) {
                error = "rte_eth_dev_start failed for " + pci;
                status_.status = "port_start_failed";
                status_.message = error;
                return false;
            }
            rte_eth_promiscuous_enable(port_id);

            struct rte_eth_dev_info info{};
            struct rte_eth_link link{};
            const int link_rc = rte_eth_link_get_nowait(port_id, &link);
            const int info_rc = rte_eth_dev_info_get(port_id, &info);

            DeviceInfo device;
            device.pci = pci;
            device.port_id = port_id;
            device.link_up = link_rc == 0 && link.link_status == RTE_ETH_LINK_UP;
            device.link_speed = link_rc == 0 && link.link_speed != RTE_ETH_SPEED_NUM_UNKNOWN ? std::to_string(link.link_speed) + "M" : "unknown";
            device.socket_id = socket_id < 0 ? 0 : socket_id;
            device.driver = info_rc == 0 && info.driver_name ? info.driver_name : "dpdk";
            device.total_tx_queues = tx_queues;
            device.used_tx_queues = 0;
            device.total_rx_queues = rx_queues;
            device.used_rx_queues = 0;
            device.available = true;
            PortRuntime runtime;
            runtime.device = device;
            runtime.last_sample = std::chrono::steady_clock::now();
            ports_[port_id] = std::move(runtime);
        }

        if (ports_.empty()) {
            error = "no DPDK Ethernet port initialized";
            status_.status = "no_ports";
            status_.message = error;
            return false;
        }
        return true;
    }

    bool launch_stream_locked(StreamContext& context, const std::vector<int>& worker_rates_mbps, const std::vector<int>& worker_burst_bytes, int fallback_total_mbps, std::string& error) {
        if (context.workers.empty()) {
            error = "stream has no worker";
            return false;
        }

        auto rates = worker_rates_mbps.empty() ? context.worker_rates_mbps : worker_rates_mbps;
        auto bursts = worker_burst_bytes.empty() ? context.worker_burst_bytes : worker_burst_bytes;
        if (rates.empty()) {
            const int worker_count = static_cast<int>(context.workers.size());
            const int total = std::max(0, fallback_total_mbps);
            const int base = worker_count > 0 ? total / worker_count : 0;
            int remainder = worker_count > 0 ? total % worker_count : 0;
            for (int i = 0; i < worker_count; ++i) {
                const int extra = remainder > 0 ? 1 : 0;
                rates.push_back(std::clamp(base + extra, 100, 10000));
                if (remainder > 0) {
                    --remainder;
                }
            }
        }
        if (rates.size() < context.workers.size()) {
            rates.resize(context.workers.size(), 100);
        } else if (rates.size() > context.workers.size()) {
            rates.resize(context.workers.size());
        }
        for (auto& rate : rates) {
            rate = std::clamp(rate, 100, 10000);
        }
        if (bursts.size() < context.workers.size()) {
            bursts.resize(context.workers.size(), 2048);
        } else if (bursts.size() > context.workers.size()) {
            bursts.resize(context.workers.size());
        }
        for (auto& burst : bursts) {
            burst = std::clamp(burst, 2048, 16 * 1024);
        }
        context.worker_rates_mbps = rates;
        context.worker_burst_bytes = bursts;

        for (size_t i = 0; i < context.workers.size(); ++i) {
            auto& worker = context.workers[i];
            worker->rate_bps = static_cast<uint64_t>(rates[i]) * 1000000ULL;
            worker->burst_bytes = static_cast<uint32_t>(bursts[i]);
            worker->running.store(true, std::memory_order_release);
            int rc = rte_eal_remote_launch(dpdk_worker_main, worker.get(), worker->lcore_id);
            if (rc != 0) {
                worker->running.store(false, std::memory_order_release);
                for (auto& launched : context.workers) {
                    launched->running.store(false, std::memory_order_release);
                    rte_eal_wait_lcore(launched->lcore_id);
                }
                error = "rte_eal_remote_launch failed on lcore " + std::to_string(worker->lcore_id) + ": " + std::strerror(-rc);
                return false;
            }
        }
        context.running = true;
        context.last_bytes = 0;
        context.last_packets = 0;
        for (const auto& worker : context.workers) {
            context.last_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
            context.last_packets += worker->counters.packets.load(std::memory_order_relaxed);
        }
        context.last_sample = std::chrono::steady_clock::now();
        return true;
    }

    bool launch_rx_stream_locked(StreamContext& context, std::string& error) {
        if (context.rx_workers.empty()) {
            error = "rx stream has no worker";
            return false;
        }

        for (auto& worker : context.rx_workers) {
            worker->running.store(true, std::memory_order_release);
            int rc = rte_eal_remote_launch(dpdk_rx_worker_main, worker.get(), worker->lcore_id);
            if (rc != 0) {
                worker->running.store(false, std::memory_order_release);
                for (auto& launched : context.rx_workers) {
                    launched->running.store(false, std::memory_order_release);
                    rte_eal_wait_lcore(launched->lcore_id);
                }
                error = "rte_eal_remote_launch failed on rx lcore " + std::to_string(worker->lcore_id) + ": " + std::strerror(-rc);
                return false;
            }
        }
        context.running = true;
        context.last_bytes = 0;
        context.last_packets = 0;
        for (const auto& worker : context.rx_workers) {
            context.last_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
            context.last_packets += worker->counters.packets.load(std::memory_order_relaxed);
        }
        context.last_sample = std::chrono::steady_clock::now();
        return true;
    }

    bool stop_stream_locked(int stream_id) {
        auto iter = streams_.find(stream_id);
        if (iter == streams_.end()) {
            return false;
        }
        auto& context = *iter->second;
        if (!context.running) {
            return true;
        }
        for (auto& worker : context.workers) {
            worker->running.store(false, std::memory_order_release);
        }
        for (auto& worker : context.rx_workers) {
            worker->running.store(false, std::memory_order_release);
        }
        for (auto& worker : context.workers) {
            rte_eal_wait_lcore(worker->lcore_id);
        }
        for (auto& worker : context.rx_workers) {
            rte_eal_wait_lcore(worker->lcore_id);
        }
        context.running = false;
        return true;
    }

    std::unordered_map<int, PortTotals> collect_port_totals_locked() const {
        std::unordered_map<int, PortTotals> totals;
        for (const auto& port : ports_) {
            auto& total = totals[port.first];
            total.tx_bytes += port.second.archived_tx_bytes;
            total.tx_packets += port.second.archived_tx_packets;
            total.tx_drops += port.second.archived_tx_drops;
            total.rx_bytes += port.second.archived_rx_bytes;
            total.rx_packets += port.second.archived_rx_packets;
            total.rx_drops += port.second.archived_rx_drops;
            total.rx_errors += port.second.archived_rx_errors;
            total.rx_nombuf += port.second.archived_rx_nombuf;
        }
        for (const auto& item : streams_) {
            const auto& context = *item.second;
            if (context.direction == "rx") {
                for (const auto& worker : context.rx_workers) {
                    auto& total = totals[worker->port_id];
                    total.rx_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
                    total.rx_packets += worker->counters.packets.load(std::memory_order_relaxed);
                    total.rx_drops += worker->counters.drops.load(std::memory_order_relaxed);
                    total.rx_errors += worker->counters.errors.load(std::memory_order_relaxed);
                    total.rx_nombuf += worker->counters.no_mbuf.load(std::memory_order_relaxed);
                }
                continue;
            }

            for (const auto& worker : context.workers) {
                auto& total = totals[worker->port_id];
                total.tx_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
                total.tx_packets += worker->counters.packets.load(std::memory_order_relaxed);
                total.tx_drops += worker->counters.drops.load(std::memory_order_relaxed);
                total.tx_drops += worker->counters.no_mbuf.load(std::memory_order_relaxed);
            }
        }
        return totals;
    }

    void archive_stream_totals_locked(const StreamContext& context) {
        if (context.direction == "rx") {
            for (const auto& worker : context.rx_workers) {
                auto port = ports_.find(worker->port_id);
                if (port == ports_.end()) {
                    continue;
                }
                port->second.archived_rx_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
                port->second.archived_rx_packets += worker->counters.packets.load(std::memory_order_relaxed);
                port->second.archived_rx_drops += worker->counters.drops.load(std::memory_order_relaxed);
                port->second.archived_rx_errors += worker->counters.errors.load(std::memory_order_relaxed);
                port->second.archived_rx_nombuf += worker->counters.no_mbuf.load(std::memory_order_relaxed);
            }
            return;
        }

        for (const auto& worker : context.workers) {
            auto port = ports_.find(worker->port_id);
            if (port == ports_.end()) {
                continue;
            }
            port->second.archived_tx_bytes += worker->counters.bytes.load(std::memory_order_relaxed);
            port->second.archived_tx_packets += worker->counters.packets.load(std::memory_order_relaxed);
            port->second.archived_tx_drops += worker->counters.drops.load(std::memory_order_relaxed);
            port->second.archived_tx_drops += worker->counters.no_mbuf.load(std::memory_order_relaxed);
        }
    }
};

#endif

} // namespace

std::unique_ptr<TxEngine> create_tx_engine(const AppConfig& config) {
#ifdef DPTX_ENABLE_DPDK
    return std::make_unique<DpdkTxEngine>(config);
#else
    return std::make_unique<UnavailableTxEngine>(config);
#endif
}

} // namespace dptx
