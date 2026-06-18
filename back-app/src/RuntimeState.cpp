#include "dptx/RuntimeState.h"
#include "dptx/StreamHistoryStore.h"
#include "dptx/TxEngine.h"
#include "dptx/Utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace dptx {

namespace {

std::string quote(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

std::string string_array(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << quote(values[i]);
    }
    out << ']';
    return out.str();
}

std::string int_array(const std::vector<int>& values) {
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
    return out.str();
}

constexpr int kMinWorkerRateMbps = 100;
constexpr int kMaxWorkerRateMbps = 10000;
constexpr int kMinWorkerBurstBytes = 2048;
constexpr int kMaxWorkerBurstBytes = 16 * 1024;

int clamp_rate_mbps(int value) {
    return std::clamp(value, kMinWorkerRateMbps, kMaxWorkerRateMbps);
}

int clamp_burst_bytes(int value) {
    return std::clamp(value, kMinWorkerBurstBytes, kMaxWorkerBurstBytes);
}

std::vector<int> split_total_rate_mbps(int total_rate_mbps, int count) {
    std::vector<int> rates;
    if (count <= 0) {
        return rates;
    }

    total_rate_mbps = std::max(0, total_rate_mbps);
    const int base = total_rate_mbps / count;
    
    int remainder = total_rate_mbps % count;
    rates.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int extra = remainder > 0 ? 1 : 0;
        rates.push_back(clamp_rate_mbps(base + extra));
        if (remainder > 0) {
            --remainder;
        }
    }
    return rates;
}

std::vector<int> normalize_worker_rates_mbps(const std::vector<int>& input, int count, int fallback_total_rate_mbps) {
    if (count <= 0) {
        return {};
    }
    if (input.empty()) {
        return split_total_rate_mbps(fallback_total_rate_mbps, count);
    }

    std::vector<int> rates;
    rates.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int value = i < static_cast<int>(input.size()) ? input[static_cast<size_t>(i)] : 0;
        rates.push_back(clamp_rate_mbps(value));
    }
    return rates;
}

int sum_rates_mbps(const std::vector<int>& rates) {
    return std::accumulate(rates.begin(), rates.end(), 0);
}

std::vector<int> normalize_worker_burst_bytes(const std::vector<int>& input, int count, int fallback_burst_bytes = kMinWorkerBurstBytes) {
    if (count <= 0) {
        return {};
    }

    std::vector<int> bursts;
    bursts.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int value = i < static_cast<int>(input.size()) ? input[static_cast<size_t>(i)] : fallback_burst_bytes;
        bursts.push_back(clamp_burst_bytes(value));
    }
    return bursts;
}

std::string error_json(const std::string& error) {
    return "{\"error\":\"" + json_escape(error) + "\"}";
}

void append_json_string_field(std::ostringstream& out, const std::string& key, const std::string& value) {
    out << "\"" << key << "\":" << quote(value);
}

void append_json_int_field(std::ostringstream& out, const std::string& key, int value) {
    out << "\"" << key << "\":" << value;
}

void append_json_bool_field(std::ostringstream& out, const std::string& key, bool value) {
    out << "\"" << key << "\":" << (value ? "true" : "false");
}

std::string history_ip_addr(const std::string& config_json, const std::string& prefix, const std::string& fallback) {
    auto value = extract_json_string(config_json, prefix + "_ip_addr");
    if (!value.empty()) {
        return value;
    }
    value = extract_json_string(config_json, prefix + "_ip", fallback);
    auto slash = value.find('/');
    return slash == std::string::npos ? value : value.substr(0, slash);
}

int history_ip_mask(const std::string& config_json, const std::string& prefix, int fallback) {
    int mask = extract_json_int(config_json, prefix + "_ip_mask", -1);
    if (mask >= 0) {
        return std::clamp(mask, 0, 32);
    }
    auto legacy = extract_json_string(config_json, prefix + "_ip");
    auto slash = legacy.find('/');
    if (slash != std::string::npos) {
        try {
            return std::clamp(std::stoi(legacy.substr(slash + 1)), 0, 32);
        } catch (...) {
        }
    }
    return fallback;
}

std::string history_config_summary_json(const std::string& config_json) {
    std::ostringstream out;
    out << "{";
    append_json_string_field(out, "direction", extract_json_string(config_json, "direction", "tx"));
    out << ",";
    append_json_string_field(out, "rx_port", extract_json_string(config_json, "rx_port"));
    out << ",";
    append_json_bool_field(out, "pcap_dump_enabled", extract_json_bool(config_json, "pcap_dump_enabled", false));
    out << ",";
    append_json_bool_field(out, "loop", extract_json_bool(config_json, "loop", true));
    out << ",";
    append_json_string_field(out, "pcap_path", extract_json_string(config_json, "pcap_path"));
    out << ",";
    append_json_string_field(out, "l3", extract_json_string(config_json, "l3", "IPv4"));
    out << ",";
    append_json_string_field(out, "l4", extract_json_string(config_json, "l4", "UDP"));
    out << ",";
    append_json_string_field(out, "src_mac", extract_json_string(config_json, "src_mac", "02:00:00:00:00:01"));
    out << ",";
    append_json_string_field(out, "dst_mac", extract_json_string(config_json, "dst_mac", "02:00:00:00:00:02"));
    out << ",";
    append_json_string_field(out, "src_ip_addr", history_ip_addr(config_json, "src", "192.168.0.1"));
    out << ",";
    append_json_int_field(out, "src_ip_mask", history_ip_mask(config_json, "src", 32));
    out << ",";
    append_json_string_field(out, "src_ip_mode", extract_json_string(config_json, "src_ip_mode", "fixed"));
    out << ",";
    append_json_int_field(out, "src_ip_step", extract_json_int(config_json, "src_ip_step", 1));
    out << ",";
    append_json_string_field(out, "dst_ip_addr", history_ip_addr(config_json, "dst", "192.168.0.2"));
    out << ",";
    append_json_int_field(out, "dst_ip_mask", history_ip_mask(config_json, "dst", 32));
    out << ",";
    append_json_string_field(out, "dst_ip_mode", extract_json_string(config_json, "dst_ip_mode", "fixed"));
    out << ",";
    append_json_int_field(out, "dst_ip_step", extract_json_int(config_json, "dst_ip_step", 1));
    out << ",";
    append_json_int_field(out, "src_port_start", extract_json_int(config_json, "src_port_start", 10000));
    out << ",";
    append_json_int_field(out, "src_port_end", extract_json_int(config_json, "src_port_end", 10000));
    out << ",";
    append_json_string_field(out, "src_port_mode", extract_json_string(config_json, "src_port_mode", "increment"));
    out << ",";
    append_json_int_field(out, "src_port_step", extract_json_int(config_json, "src_port_step", 1));
    out << ",";
    append_json_int_field(out, "dst_port_start", extract_json_int(config_json, "dst_port_start", 53));
    out << ",";
    append_json_int_field(out, "dst_port_end", extract_json_int(config_json, "dst_port_end", 53));
    out << ",";
    append_json_string_field(out, "dst_port_mode", extract_json_string(config_json, "dst_port_mode", "increment"));
    out << ",";
    append_json_int_field(out, "dst_port_step", extract_json_int(config_json, "dst_port_step", 1));
    out << ",";
    append_json_int_field(out, "payload_len", extract_json_int(config_json, "payload_len", 64));
    out << ",";
    append_json_bool_field(out, "checksum_enabled", extract_json_bool(config_json, "checksum_enabled", true));
    out << "}";
    return out.str();
}

long read_long_file(const std::filesystem::path& path, long fallback = 0) {
    std::ifstream file(path);
    if (!file) {
        return fallback;
    }
    long value = fallback;
    file >> value;
    return file ? value : fallback;
}

bool parse_bool(const std::string& value) {
    auto lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

std::vector<int> parse_core_list(const std::string& value) {
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

bool contains_core(const std::string& list, int core_id) {
    auto cores = parse_core_list(list);
    return std::find(cores.begin(), cores.end(), core_id) != cores.end();
}

std::string read_first_line(const std::filesystem::path& path, const std::string& fallback = "") {
    std::ifstream file(path);
    if (!file) {
        return fallback;
    }
    std::string value;
    std::getline(file, value);
    return trim(value);
}

long meminfo_kb(const std::string& key) {
    std::ifstream file("/proc/meminfo");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind(key + ":", 0) != 0) {
            continue;
        }
        std::stringstream ss(line.substr(key.size() + 1));
        long value = 0;
        ss >> value;
        return ss ? value : 0;
    }
    return 0;
}

long meminfo_count(const std::string& key) {
    return meminfo_kb(key);
}

double kb_to_mb(long kb) {
    return std::round((static_cast<double>(kb) / 1024.0) * 10.0) / 10.0;
}

std::string hugepages_json() {
    struct Pool {
        long page_size_kb = 0;
        long total = 0;
        long free = 0;
        long reserved = 0;
        long surplus = 0;
    };

    std::vector<Pool> pools;
    std::error_code ec;
    const std::filesystem::path huge_dir("/sys/kernel/mm/hugepages");
    if (std::filesystem::exists(huge_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(huge_dir, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto name = entry.path().filename().string();
            const std::string prefix = "hugepages-";
            const std::string suffix = "kB";
            if (name.rfind(prefix, 0) != 0 || name.size() <= prefix.size() + suffix.size()) {
                continue;
            }
            auto size_text = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            Pool pool;
            try {
                pool.page_size_kb = std::stol(size_text);
            } catch (...) {
                continue;
            }
            pool.total = read_long_file(entry.path() / "nr_hugepages");
            pool.free = read_long_file(entry.path() / "free_hugepages");
            pool.reserved = read_long_file(entry.path() / "resv_hugepages");
            pool.surplus = read_long_file(entry.path() / "surplus_hugepages");
            pools.push_back(pool);
        }
    }

    std::sort(pools.begin(), pools.end(), [](const Pool& lhs, const Pool& rhs) {
        return lhs.page_size_kb < rhs.page_size_kb;
    });

    const long default_page_size_kb = meminfo_kb("Hugepagesize");
    const long hugetlb_kb = meminfo_kb("Hugetlb");
    long total = 0;
    long free = 0;
    long reserved = 0;
    long surplus = 0;
    long total_kb = 0;
    long free_kb = 0;

    for (const auto& pool : pools) {
        total += pool.total;
        free += pool.free;
        reserved += pool.reserved;
        surplus += pool.surplus;
        total_kb += pool.total * pool.page_size_kb;
        free_kb += pool.free * pool.page_size_kb;
    }

    if (pools.empty() && default_page_size_kb > 0) {
        total = meminfo_count("HugePages_Total");
        free = meminfo_count("HugePages_Free");
        reserved = meminfo_count("HugePages_Rsvd");
        surplus = meminfo_count("HugePages_Surp");
        total_kb = total * default_page_size_kb;
        free_kb = free * default_page_size_kb;
    }

    std::ostringstream out;
    out << "{";
    out << "\"available\":" << (total > 0 ? "true" : "false") << ",";
    out << "\"default_page_size_kb\":" << default_page_size_kb << ",";
    out << "\"hugetlb_mb\":" << kb_to_mb(hugetlb_kb) << ",";
    out << "\"total_pages\":" << total << ",";
    out << "\"free_pages\":" << free << ",";
    out << "\"reserved_pages\":" << reserved << ",";
    out << "\"surplus_pages\":" << surplus << ",";
    out << "\"total_mb\":" << kb_to_mb(total_kb) << ",";
    out << "\"free_mb\":" << kb_to_mb(free_kb) << ",";
    out << "\"used_mb\":" << kb_to_mb(std::max<long>(0, total_kb - free_kb)) << ",";
    out << "\"pools\":[";
    for (size_t i = 0; i < pools.size(); ++i) {
        const auto& pool = pools[i];
        if (i) {
            out << ",";
        }
        out << "{";
        out << "\"page_size_kb\":" << pool.page_size_kb << ",";
        out << "\"total_pages\":" << pool.total << ",";
        out << "\"free_pages\":" << pool.free << ",";
        out << "\"reserved_pages\":" << pool.reserved << ",";
        out << "\"surplus_pages\":" << pool.surplus << ",";
        out << "\"total_mb\":" << kb_to_mb(pool.total * pool.page_size_kb) << ",";
        out << "\"free_mb\":" << kb_to_mb(pool.free * pool.page_size_kb);
        out << "}";
    }
    out << "]";
    out << "}";
    return out.str();
}

std::string read_driver_name(const std::string& pci) {
    std::error_code ec;
    auto driver_link = std::filesystem::read_symlink(std::filesystem::path("/sys/bus/pci/devices") / pci / "driver", ec);
    if (ec || driver_link.empty()) {
        return "unknown";
    }
    return driver_link.filename().string();
}

int read_numa_node(const std::string& pci) {
    auto value = read_first_line(std::filesystem::path("/sys/bus/pci/devices") / pci / "numa_node", "0");
    try {
        return std::max(0, std::stoi(value));
    } catch (...) {
        return 0;
    }
}

std::pair<bool, std::string> read_net_link(const std::string& pci) {
    std::error_code ec;
    auto net_dir = std::filesystem::path("/sys/bus/pci/devices") / pci / "net";
    if (!std::filesystem::exists(net_dir, ec)) {
        return {true, "unknown"};
    }
    for (const auto& entry : std::filesystem::directory_iterator(net_dir, ec)) {
        auto iface = entry.path().filename();
        auto operstate = read_first_line(entry.path() / "operstate", "unknown");
        auto speed = read_first_line(entry.path() / "speed", "unknown");
        bool up = operstate == "up" || operstate == "unknown";
        if (speed != "unknown" && speed != "-1") {
            speed += "M";
        }
        return {up, speed};
    }
    return {true, "unknown"};
}

std::vector<std::string> discover_vfio_pci_devices() {
    std::vector<std::string> devices;
    std::error_code ec;
    const std::filesystem::path vfio_dir("/sys/bus/pci/drivers/vfio-pci");
    if (!std::filesystem::exists(vfio_dir, ec)) {
        return devices;
    }
    for (const auto& entry : std::filesystem::directory_iterator(vfio_dir, ec)) {
        auto name = entry.path().filename().string();
        if (name.find(':') != std::string::npos && name.find('.') != std::string::npos) {
            devices.push_back(name);
        }
    }
    std::sort(devices.begin(), devices.end());
    return devices;
}

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

std::optional<PcapFileInfo> make_pcap_summary(
    const std::filesystem::path& path,
    long packet_count,
    uint64_t total_len,
    int min_len,
    int max_len) {
    if (packet_count == 0) {
        min_len = 0;
    }

    std::error_code ec;
    auto bytes = std::filesystem::file_size(path, ec);
    PcapFileInfo info;
    info.name = path.filename().string();
    info.path = path.string();
    info.packet_count = packet_count;
    info.max_len = max_len;
    info.min_len = min_len;
    info.avg_len = packet_count == 0 ? 0 : static_cast<int>(total_len / static_cast<uint64_t>(packet_count));
    info.size_mb = ec ? 0.0 : std::round((static_cast<double>(bytes) / 1024.0 / 1024.0) * 10.0) / 10.0;
    return info;
}

std::optional<PcapFileInfo> read_pcapng_summary(std::ifstream& file, const std::filesystem::path& path) {
    file.clear();
    file.seekg(0, std::ios::beg);

    long packet_count = 0;
    uint64_t total_len = 0;
    int min_len = std::numeric_limits<int>::max();
    int max_len = 0;
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
                return std::nullopt;
            }
            have_section = true;
            if (block_total_len < 28) {
                return std::nullopt;
            }
            file.seekg(static_cast<std::streamoff>(block_total_len) - 12, std::ios::cur);
            continue;
        }

        if (block_total_len < 12 || block_total_len > 256 * 1024 * 1024) {
            break;
        }

        std::vector<unsigned char> body(block_total_len - 12);
        if (!file.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(body.size()))) {
            break;
        }
        file.seekg(4, std::ios::cur);

        uint32_t logical_len = 0;
        if (block_type == 0x00000006 && body.size() >= 20) {
            // Enhanced Packet Block: interface_id, ts_high, ts_low, captured_len, packet_len.
            logical_len = read_u32(body.data() + 16, little_endian);
        } else if (block_type == 0x00000003 && body.size() >= 4) {
            // Simple Packet Block: original packet length.
            logical_len = read_u32(body.data(), little_endian);
        } else if (block_type == 0x00000002 && body.size() >= 20) {
            // Obsolete Packet Block: interface_id, drops, ts_high, ts_low, captured_len, packet_len.
            logical_len = read_u32(body.data() + 16, little_endian);
        } else {
            continue;
        }

        ++packet_count;
        total_len += logical_len;
        min_len = std::min(min_len, static_cast<int>(logical_len));
        max_len = std::max(max_len, static_cast<int>(logical_len));
    }

    return make_pcap_summary(path, packet_count, total_len, min_len, max_len);
}

std::optional<PcapFileInfo> read_pcap_summary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    unsigned char global_header[24]{};
    file.read(reinterpret_cast<char*>(global_header), sizeof(global_header));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(global_header))) {
        return std::nullopt;
    }

    bool little_endian = false;
    const bool classic_little = global_header[0] == 0xd4 && global_header[1] == 0xc3 && global_header[2] == 0xb2 && global_header[3] == 0xa1;
    const bool classic_big = global_header[0] == 0xa1 && global_header[1] == 0xb2 && global_header[2] == 0xc3 && global_header[3] == 0xd4;
    const bool nano_little = global_header[0] == 0x4d && global_header[1] == 0x3c && global_header[2] == 0xb2 && global_header[3] == 0xa1;
    const bool nano_big = global_header[0] == 0xa1 && global_header[1] == 0xb2 && global_header[2] == 0x3c && global_header[3] == 0x4d;
    const bool pcapng = global_header[0] == 0x0a && global_header[1] == 0x0d && global_header[2] == 0x0d && global_header[3] == 0x0a;
    if (pcapng) {
        return read_pcapng_summary(file, path);
    }
    if (classic_little || nano_little) {
        little_endian = true;
    } else if (classic_big || nano_big) {
        little_endian = false;
    } else {
        return std::nullopt;
    }

    long packet_count = 0;
    uint64_t total_len = 0;
    int min_len = std::numeric_limits<int>::max();
    int max_len = 0;
    unsigned char packet_header[16]{};

    while (file.read(reinterpret_cast<char*>(packet_header), sizeof(packet_header))) {
        uint32_t incl_len = read_u32(packet_header + 8, little_endian);
        uint32_t orig_len = read_u32(packet_header + 12, little_endian);
        uint32_t logical_len = orig_len == 0 ? incl_len : orig_len;
        if (incl_len > 256 * 1024 * 1024) {
            break;
        }
        ++packet_count;
        total_len += logical_len;
        min_len = std::min(min_len, static_cast<int>(logical_len));
        max_len = std::max(max_len, static_cast<int>(logical_len));
        file.seekg(incl_len, std::ios::cur);
        if (!file) {
            break;
        }
    }

    return make_pcap_summary(path, packet_count, total_len, min_len, max_len);
}

bool has_allowed_extension(const std::filesystem::path& path, const std::vector<std::string>& extensions) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto allowed : extensions) {
        std::transform(allowed.begin(), allowed.end(), allowed.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == allowed) {
            return true;
        }
    }
    return false;
}

std::unordered_map<int, CpuUsageSample> read_cpu_usage_samples() {
    std::unordered_map<int, CpuUsageSample> samples;
    std::ifstream file("/proc/stat");
    if (!file) {
        return samples;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("cpu", 0) != 0 || line.size() < 4 || !std::isdigit(static_cast<unsigned char>(line[3]))) {
            continue;
        }

        std::stringstream ss(line);
        std::string label;
        uint64_t user = 0;
        uint64_t nice = 0;
        uint64_t system = 0;
        uint64_t idle = 0;
        uint64_t iowait = 0;
        uint64_t irq = 0;
        uint64_t softirq = 0;
        uint64_t steal = 0;
        ss >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        if (!ss) {
            continue;
        }

        int core_id = std::stoi(label.substr(3));
        CpuUsageSample sample;
        sample.idle = idle + iowait;
        sample.total = user + nice + system + idle + iowait + irq + softirq + steal;
        samples[core_id] = sample;
    }
    return samples;
}

} // namespace

RuntimeState::RuntimeState(AppConfig config) : config_(std::move(config)), started_at_(std::chrono::steady_clock::now()) {
    tx_engine_ = create_tx_engine(config_);
    std::string error;
    if (tx_engine_) {
        tx_engine_->initialize(error);
    }
    seed();
}

RuntimeState::~RuntimeState() {
    if (tx_engine_) {
        tx_engine_->shutdown();
    }
}

std::string RuntimeState::challenge() const {
    return challenge_;
}

std::string RuntimeState::expected_auth_md5() const {
    return md5_hex(config_.auth_token + ":" + challenge_);
}

std::string RuntimeState::create_session_token(const std::string& auth_md5) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_token_ = md5_hex(auth_md5 + ":session");
    return session_token_;
}

bool RuntimeState::valid_session(const std::string& authorization) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_token_.empty()) {
        return false;
    }
    const std::string prefix = "Bearer ";
    return authorization.rfind(prefix, 0) == 0 && authorization.substr(prefix.size()) == session_token_;
}

void RuntimeState::seed() {
    discover_devices_locked();
    initialize_cores_locked();
    scan_pcap_files_locked();
    initialize_stats_locked();
    if (config_.mock_initial_streams && !devices_.empty()) {
        const bool can_run = tx_engine_ready_locked();
        auto allocated = allocate_cores_locked(std::min(2, std::max(1, static_cast<int>(cores_.size()) - 1)), "demo-stream", can_run);
        auto queues = allocate_queues_locked(devices_.front().pci, static_cast<int>(allocated.size()), "tx");
        if (!allocated.empty() && queues.size() == allocated.size()) {
            const auto worker_rates = normalize_worker_rates_mbps({}, static_cast<int>(allocated.size()), 1000);
            const auto worker_bursts = normalize_worker_burst_bytes({}, static_cast<int>(allocated.size()));
            StreamInfo stream;
            stream.id = next_stream_id_++;
            stream.name = "demo-stream";
            stream.status = can_run ? "running" : "stopped";
            stream.direction = "tx";
            stream.mode = pcaps_.empty() ? "construct" : "pcap";
            stream.tx_port = devices_.front().pci;
            stream.cores = std::move(allocated);
            stream.target_mbps = sum_rates_mbps(worker_rates);
            stream.actual_mbps = 0;
            stream.config_json = "{}";
            stream.queues = std::move(queues);
            stream.worker_rates_mbps = worker_rates;
            stream.worker_burst_bytes = worker_bursts;
            streams_.push_back(std::move(stream));
            sync_devices_from_streams_locked();
        }
    }
}

void RuntimeState::discover_devices_locked() {
    devices_.clear();
    if (tx_engine_ready_locked() && tx_engine_) {
        devices_ = tx_engine_->discover_devices();
        if (!devices_.empty()) {
            return;
        }
    }

    auto pci_list = config_.device_list;
    const bool from_device_list = !pci_list.empty();
    if (pci_list.empty()) {
        pci_list = discover_vfio_pci_devices();
    }

    int port_id = 0;
    int tx_queue_num = config_.tx_queue_per_port > 0
        ? config_.tx_queue_per_port
        : std::max(1, static_cast<int>(parse_core_list(config_.core_list).size()) - 1);
    int rx_queue_num = config_.rx_queue_per_port > 0
        ? config_.rx_queue_per_port
        : std::max(1, static_cast<int>(parse_core_list(config_.core_list).size()) - 1);

    for (const auto& pci : pci_list) {
        auto [link_up, link_speed] = read_net_link(pci);
        const auto sysfs_path = std::filesystem::path("/sys/bus/pci/devices") / pci;
        std::error_code ec;
        const bool exists = std::filesystem::exists(sysfs_path, ec);
        auto driver = read_driver_name(pci);
        DeviceInfo device;
        device.pci = pci;
        device.port_id = port_id++;
        device.available = exists && (!from_device_list || driver == "vfio-pci");
        device.unavailable_reason = device.available ? "" : (!exists ? "设备不存在，需重启 dpdk_tx 系统" : "设备未绑定 vfio-pci，需重启 dpdk_tx 系统");
        device.link_up = device.available && link_up;
        device.link_speed = link_speed;
        device.socket_id = read_numa_node(pci);
        device.driver = driver;
        device.total_tx_queues = tx_queue_num;
        device.used_tx_queues = 0;
        device.total_rx_queues = rx_queue_num;
        device.used_rx_queues = 0;
        devices_.push_back(device);
    }
}

void RuntimeState::scan_pcap_files_locked() {
    pcaps_.clear();
    std::error_code ec;
    const std::filesystem::path root(config_.pcap_root);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }

    int scanned = 0;
    auto add_file = [&](const std::filesystem::directory_entry& entry) {
        if (scanned >= config_.pcap_max_scan_files) {
            return;
        }
        if (!entry.is_regular_file()) {
            return;
        }
        if (!has_allowed_extension(entry.path(), config_.pcap_extensions)) {
            return;
        }
        if (auto summary = read_pcap_summary(entry.path())) {
            pcaps_.push_back(*summary);
            ++scanned;
        }
    };

    if (config_.pcap_recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            add_file(entry);
            if (scanned >= config_.pcap_max_scan_files) {
                break;
            }
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            add_file(entry);
            if (scanned >= config_.pcap_max_scan_files) {
                break;
            }
        }
    }

    std::sort(pcaps_.begin(), pcaps_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name < rhs.name;
    });
}

void RuntimeState::initialize_cores_locked() {
    cores_.clear();
    auto ids = parse_core_list(config_.core_list);
    if (ids.empty()) {
        ids.push_back(config_.main_lcore);
    }
    for (int id : ids) {
        CoreInfo core;
        core.id = id;
        core.available = core_online_locked(id);
        if (!core.available) {
            core.status = "unavailable";
            core.role = "不可用";
            core.unavailable_reason = "CPU 核不在线，需重启 dpdk_tx 系统";
            core.usage_percent = 0;
        } else if (id == config_.main_lcore) {
            core.status = "main";
            core.role = "main";
            core.usage_percent = 10;
        } else {
            core.status = "idle";
            core.role = "空闲";
            core.usage_percent = 0;
        }
        cores_.push_back(core);
    }
}

void RuntimeState::initialize_stats_locked() {
    stats_.clear();
    for (const auto& device : devices_) {
        PortStats row;
        row.pci = device.pci;
        row.port_id = device.port_id;
        stats_.push_back(row);
    }
}

bool RuntimeState::core_online_locked(int core_id) const {
    auto online = read_first_line("/sys/devices/system/cpu/online", "");
    if (online.empty()) {
        auto cpu_path = std::filesystem::path("/sys/devices/system/cpu") / ("cpu" + std::to_string(core_id));
        std::error_code ec;
        return std::filesystem::exists(cpu_path, ec);
    }
    try {
        return contains_core(online, core_id);
    } catch (...) {
        return false;
    }
}

void RuntimeState::refresh_stats_locked() {
    if (tx_engine_ready_locked()) {
        refresh_engine_stats_locked();
        return;
    }

    for (auto& stream : streams_) {
        stream.actual_mbps = 0;
    }
    for (auto& row : stats_) {
        row.tx_mbps = 0;
        row.tx_mpps = 0.0;
        row.rx_mbps = 0;
        row.rx_mpps = 0.0;
    }
}

void RuntimeState::update_cpu_usage_locked() {
    auto current = read_cpu_usage_samples();
    for (auto& core : cores_) {
        if (!core.available || core.status == "unavailable") {
            core.usage_percent = 0;
            continue;
        }

        auto now = current.find(core.id);
        if (now == current.end()) {
            core.usage_percent = 0;
            continue;
        }

        auto previous = cpu_usage_samples_.find(core.id);
        if (previous != cpu_usage_samples_.end() && now->second.total > previous->second.total) {
            const auto total_delta = now->second.total - previous->second.total;
            const auto idle_delta = now->second.idle > previous->second.idle ? now->second.idle - previous->second.idle : 0;
            const auto busy_delta = total_delta > idle_delta ? total_delta - idle_delta : 0;
            core.usage_percent = static_cast<int>(std::clamp(std::llround((static_cast<double>(busy_delta) * 100.0) / static_cast<double>(total_delta)), 0LL, 100LL));
        } else {
            core.usage_percent = 0;
        }
    }
    cpu_usage_samples_ = std::move(current);
}

bool RuntimeState::tx_engine_ready_locked() const {
    return tx_engine_ && tx_engine_->status().ready;
}

std::string RuntimeState::tx_engine_status_locked() const {
    return tx_engine_ ? tx_engine_->status().status : "disabled";
}

std::string RuntimeState::tx_engine_message_locked() const {
    return tx_engine_ ? tx_engine_->status().message : "真实 DPDK TX/RX 引擎未就绪，当前仅保留控制面配置";
}

void RuntimeState::refresh_engine_stats_locked() {
    if (!tx_engine_) {
        return;
    }
    std::unordered_map<int, StreamRuntimeStats> stream_stats;
    tx_engine_->snapshot_stats(stats_, stream_stats);
    for (auto& stream : streams_) {
        auto iter = stream_stats.find(stream.id);
        if (iter == stream_stats.end()) {
            stream.actual_mbps = 0;
            stream.actual_pps = 0.0;
            continue;
        }
        stream.actual_mbps = iter->second.actual_mbps;
        stream.actual_pps = iter->second.actual_pps;
        stream.total_gb = iter->second.total_gb;
        stream.packets = iter->second.packets;
        stream.drops = iter->second.drops;
        stream.errors = iter->second.errors;
        stream.dump_files = iter->second.dump_files;
        stream.dump_errors = iter->second.dump_errors;
    }
}

std::string RuntimeState::uptime_locked() const {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_).count();
    auto h = seconds / 3600;
    auto m = (seconds % 3600) / 60;
    auto s = seconds % 60;
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", static_cast<long long>(h), static_cast<long long>(m), static_cast<long long>(s));
    return buffer;
}

std::string RuntimeState::runtime_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> enabled_lcores;
    std::vector<int> worker_lcores;
    std::vector<std::string> device_list;
    for (const auto& core : cores_) {
        enabled_lcores.push_back(core.id);
        if (core.id != config_.main_lcore) {
            worker_lcores.push_back(core.id);
        }
    }
    for (const auto& device : devices_) {
        device_list.push_back(device.pci);
    }
    std::ostringstream out;
    out << "{";
    out << "\"started\":true,";
    out << "\"version\":\"0.1.0\",";
    out << "\"system_name\":" << quote(config_.system_name) << ",";
    out << "\"listen\":" << quote(config_.listen_host + ":" + std::to_string(config_.listen_port)) << ",";
    out << "\"pcap_root\":" << quote(config_.pcap_root) << ",";
    out << "\"rx_pcap_dump_root\":" << quote(config_.rx_pcap_dump_root) << ",";
    out << "\"uptime\":" << quote(uptime_locked()) << ",";
    out << "\"hugepages\":" << hugepages_json() << ",";
    auto engine_status = tx_engine_ ? tx_engine_->status() : TxEngineStatus{};
    out << "\"tx_engine\":{";
    out << "\"dpdk_linked\":" << (engine_status.dpdk_linked ? "true" : "false") << ",";
    out << "\"ready\":" << (engine_status.ready ? "true" : "false") << ",";
    out << "\"mode\":" << quote(engine_status.mode) << ",";
    out << "\"status\":" << quote(engine_status.status) << ",";
    out << "\"message\":" << quote(engine_status.message);
    out << "},";
    out << "\"eal\":{";
    out << "\"main_lcore\":" << config_.main_lcore << ",";
    out << "\"core_list\":" << quote(config_.core_list) << ",";
    out << "\"file_prefix\":" << quote(config_.file_prefix) << ",";
    out << "\"proc_type\":" << quote(config_.proc_type) << ",";
    out << "\"mem_channels\":" << config_.mem_channels << ",";
    out << "\"enabled_lcores\":" << int_array(enabled_lcores) << ",";
    out << "\"worker_lcores\":" << int_array(worker_lcores) << ",";
    out << "\"device_list\":" << string_array(device_list) << ",";
    out << "\"allow_ports\":" << string_array(device_list);
    out << "},";
    out << "\"settings\":{";
    out << "\"tx_ring_size\":" << config_.tx_ring_size << ",";
    out << "\"rx_ring_size\":" << config_.rx_ring_size << ",";
    out << "\"tx_queue_per_port\":" << config_.tx_queue_per_port << ",";
    out << "\"rx_queue_per_port\":" << config_.rx_queue_per_port << ",";
    out << "\"mbuf_pool_size\":" << config_.mbuf_pool_size << ",";
    out << "\"mbuf_cache_size\":" << config_.mbuf_cache_size << ",";
    out << "\"max_burst\":" << config_.max_burst << ",";
    out << "\"stats_interval_ms\":" << config_.stats_interval_ms << ",";
    out << "\"rx_pcap_dump_root\":" << quote(config_.rx_pcap_dump_root) << ",";
    out << "\"rx_pcap_max_file_mb\":" << config_.rx_pcap_max_file_mb << ",";
    out << "\"rx_pcap_stop_on_error\":" << (config_.rx_pcap_stop_on_error ? "true" : "false") << ",";
    out << "\"db_enabled\":" << (config_.db_enabled ? "true" : "false") << ",";
    out << "\"db_endpoint\":" << quote(config_.db_endpoint) << ",";
    out << "\"db_user\":" << quote(config_.db_user) << ",";
    out << "\"db_password_set\":" << (!config_.db_password.empty() ? "true" : "false") << ",";
    out << "\"db_name\":" << quote(config_.db_name) << ",";
    out << "\"db_tx_stream_table\":" << quote(config_.db_tx_stream_table) << ",";
    out << "\"db_rx_stream_table\":" << quote(config_.db_rx_stream_table);
    out << "}}";
    return out.str();
}

std::string RuntimeState::devices_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    sync_devices_from_streams_locked();
    std::ostringstream out;
    out << "{\"devices\":[";
    for (size_t i = 0; i < devices_.size(); ++i) {
        const auto& d = devices_[i];
        if (i) out << ',';
        out << "{";
        out << "\"pci\":" << quote(d.pci) << ",";
        out << "\"port_id\":" << d.port_id << ",";
        out << "\"link_up\":" << (d.link_up ? "true" : "false") << ",";
        out << "\"link_speed\":" << quote(d.link_speed) << ",";
        out << "\"socket_id\":" << d.socket_id << ",";
        out << "\"driver\":" << quote(d.driver) << ",";
        out << "\"total_tx_queues\":" << d.total_tx_queues << ",";
        out << "\"used_tx_queues\":" << d.used_tx_queues << ",";
        out << "\"total_rx_queues\":" << d.total_rx_queues << ",";
        out << "\"used_rx_queues\":" << d.used_rx_queues << ",";
        out << "\"available\":" << (d.available ? "true" : "false") << ",";
        out << "\"unavailable_reason\":" << quote(d.unavailable_reason) << ",";
        out << "\"streams\":" << string_array(d.streams) << ",";
        out << "\"tx_streams\":" << string_array(d.tx_streams) << ",";
        out << "\"rx_streams\":" << string_array(d.rx_streams);
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string RuntimeState::cores_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    update_cpu_usage_locked();
    sync_core_state_locked();
    std::ostringstream out;
    out << "{\"cores\":[";
    for (size_t i = 0; i < cores_.size(); ++i) {
        const auto& c = cores_[i];
        if (i) out << ',';
        out << "{";
        out << "\"id\":" << c.id << ",";
        out << "\"status\":" << quote(c.status) << ",";
        out << "\"role\":" << quote(c.role) << ",";
        out << "\"stream\":" << quote(c.stream) << ",";
        out << "\"usage_percent\":" << c.usage_percent << ",";
        out << "\"available\":" << (c.available ? "true" : "false") << ",";
        out << "\"unavailable_reason\":" << quote(c.unavailable_reason);
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string RuntimeState::streams_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "{\"streams\":[";
    for (size_t i = 0; i < streams_.size(); ++i) {
        const auto& s = streams_[i];
        if (i) out << ',';
        out << "{";
        out << "\"id\":" << s.id << ",";
        out << "\"name\":" << quote(s.name) << ",";
        out << "\"status\":" << quote(s.status) << ",";
        out << "\"direction\":" << quote(s.direction.empty() ? "tx" : s.direction) << ",";
        out << "\"mode\":" << quote(s.mode) << ",";
        out << "\"tx_port\":" << quote(s.tx_port) << ",";
        out << "\"rx_port\":" << quote(s.rx_port) << ",";
        out << "\"port\":" << quote(s.direction == "rx" ? s.rx_port : s.tx_port) << ",";
        out << "\"cores\":" << int_array(s.cores) << ",";
        out << "\"queues\":" << int_array(s.queues) << ",";
        out << "\"worker_rates_mbps\":" << int_array(s.worker_rates_mbps) << ",";
        out << "\"worker_burst_bytes\":" << int_array(s.worker_burst_bytes) << ",";
        out << "\"target_mbps\":" << s.target_mbps << ",";
        out << "\"actual_mbps\":" << s.actual_mbps << ",";
        out << "\"actual_pps\":" << s.actual_pps << ",";
        out << "\"total_gb\":" << s.total_gb << ",";
        out << "\"packets\":" << s.packets << ",";
        out << "\"drops\":" << s.drops << ",";
        out << "\"errors\":" << s.errors << ",";
        out << "\"dump_files\":" << s.dump_files << ",";
        out << "\"dump_errors\":" << s.dump_errors << ",";
        out << "\"pcap_dump_enabled\":" << (s.pcap_dump_enabled ? "true" : "false") << ",";
        out << "\"pcap_dump_dir\":" << quote(s.pcap_dump_dir) << ",";
        out << "\"config_summary\":" << history_config_summary_json(s.config_json);
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string RuntimeState::pcap_files_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    scan_pcap_files_locked();
    std::ostringstream out;
    out << "{\"files\":[";
    for (size_t i = 0; i < pcaps_.size(); ++i) {
        const auto& p = pcaps_[i];
        if (i) out << ',';
        out << "{";
        out << "\"name\":" << quote(p.name) << ",";
        out << "\"path\":" << quote(p.path) << ",";
        out << "\"packet_count\":" << p.packet_count << ",";
        out << "\"max_len\":" << p.max_len << ",";
        out << "\"min_len\":" << p.min_len << ",";
        out << "\"avg_len\":" << p.avg_len << ",";
        out << "\"size_mb\":" << p.size_mb;
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string RuntimeState::stats_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    refresh_stats_locked();
    std::ostringstream out;
    out << "{\"ports\":[";
    for (size_t i = 0; i < stats_.size(); ++i) {
        const auto& s = stats_[i];
        if (i) out << ',';
        out << "{";
        out << "\"pci\":" << quote(s.pci) << ",";
        out << "\"port_id\":" << s.port_id << ",";
        out << "\"tx_mbps\":" << s.tx_mbps << ",";
        out << "\"tx_mpps\":" << s.tx_mpps << ",";
        out << "\"total_tb\":" << s.total_tb << ",";
        out << "\"tx_packets\":" << s.tx_packets << ",";
        out << "\"tx_packets_m\":" << s.tx_packets_m << ",";
        out << "\"tx_drops\":" << s.tx_drops << ",";
        out << "\"tx_nombuf\":" << s.tx_nombuf << ",";
        out << "\"rx_mbps\":" << s.rx_mbps << ",";
        out << "\"rx_mpps\":" << s.rx_mpps << ",";
        out << "\"rx_total_gb\":" << s.rx_total_gb << ",";
        out << "\"rx_packets\":" << s.rx_packets << ",";
        out << "\"rx_packets_m\":" << s.rx_packets_m << ",";
        out << "\"rx_drops\":" << s.rx_drops << ",";
        out << "\"rx_errors\":" << s.rx_errors << ",";
        out << "\"rx_nombuf\":" << s.rx_nombuf;
        out << "}";
    }
    out << "],\"streams\":[";
    for (size_t i = 0; i < streams_.size(); ++i) {
        const auto& stream = streams_[i];
        if (i) out << ',';
        out << "{";
        out << "\"id\":" << stream.id << ",";
        out << "\"direction\":" << quote(stream.direction.empty() ? "tx" : stream.direction) << ",";
        out << "\"actual_mbps\":" << stream.actual_mbps << ",";
        out << "\"actual_pps\":" << stream.actual_pps << ",";
        out << "\"total_gb\":" << stream.total_gb << ",";
        out << "\"packets\":" << stream.packets << ",";
        out << "\"drops\":" << stream.drops << ",";
        out << "\"errors\":" << stream.errors << ",";
        out << "\"dump_files\":" << stream.dump_files << ",";
        out << "\"dump_errors\":" << stream.dump_errors;
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string RuntimeState::create_stream(const std::string& body, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string name = extract_json_string(body, "name", "stream-" + std::to_string(next_stream_id_));
    std::string direction = extract_json_string(body, "direction", "tx");
    std::transform(direction.begin(), direction.end(), direction.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (direction != "rx") {
        direction = "tx";
    }
    std::string tx_port = extract_json_string(body, "tx_port", devices_.empty() ? "" : devices_.front().pci);
    std::string rx_port = extract_json_string(body, "rx_port", tx_port.empty() ? (devices_.empty() ? "" : devices_.front().pci) : tx_port);
    std::string selected_port = direction == "rx" ? rx_port : tx_port;
    std::string mode = extract_json_string(body, "mode", "pcap");
    if (direction == "rx") {
        mode = "receive";
    }
    int core_count = std::max(1, extract_json_int(body, "core_count", 1));
    auto worker_rates = direction == "rx" ? std::vector<int>{} : normalize_worker_rates_mbps(
        extract_json_int_array(body, "worker_rates_mbps"),
        core_count,
        extract_json_int(body, "rate_mbps", 1000));
    auto worker_bursts = direction == "rx" ? std::vector<int>{} : normalize_worker_burst_bytes(
        extract_json_int_array(body, "worker_burst_bytes"),
        core_count);
    int rate = sum_rates_mbps(worker_rates);

    if (!tx_engine_ready_locked()) {
        status = 503;
        return error_json(tx_engine_message_locked());
    }
    if (devices_.empty()) {
        status = 409;
        return error_json("no DPDK device discovered");
    }
    auto* device = find_device_locked(selected_port);
    if (!device || !device->available || !device->link_up) {
        status = 400;
        return error_json(direction == "rx" ? "rx port unavailable" : "tx port unavailable");
    }
    if (direction == "tx" && mode == "pcap") {
        scan_pcap_files_locked();
        auto pcap_path = extract_json_string(body, "pcap_path");
        bool exists = std::any_of(pcaps_.begin(), pcaps_.end(), [&](const PcapFileInfo& file) {
            return file.path == pcap_path;
        });
        if (!exists) {
            status = 400;
            return error_json("pcap file not found under PCAP_ROOT");
        }
    }
    auto queues = allocate_queues_locked(selected_port, core_count, direction);
    const int used_queues = direction == "rx" ? device->used_rx_queues : device->used_tx_queues;
    const int total_queues = direction == "rx" ? device->total_rx_queues : device->total_tx_queues;
    if (static_cast<int>(queues.size()) < core_count || used_queues + core_count > total_queues) {
        status = 409;
        return error_json(direction == "rx" ? "rx queue not enough" : "tx queue not enough");
    }

    auto allocated = allocate_cores_locked(core_count, name, true);
    if (static_cast<int>(allocated.size()) < core_count) {
        for (int core_id : allocated) {
            auto iter = std::find_if(cores_.begin(), cores_.end(), [&](const CoreInfo& c) { return c.id == core_id; });
            if (iter != cores_.end()) {
                iter->status = "idle";
                iter->role = "空闲";
                iter->stream.clear();
            }
        }
        status = 409;
        return error_json("lcore not enough");
    }

    StreamInfo stream;
    stream.id = next_stream_id_++;
    stream.name = name;
    stream.status = "running";
    stream.direction = direction;
    stream.mode = mode;
    stream.tx_port = tx_port;
    stream.rx_port = rx_port;
    stream.cores = std::move(allocated);
    stream.target_mbps = rate;
    stream.actual_mbps = 0;
    stream.actual_pps = 0.0;
    stream.total_gb = 0.0;
    stream.pcap_dump_enabled = direction == "rx" && extract_json_bool(body, "pcap_dump_enabled", false);
    stream.pcap_dump_dir = stream.pcap_dump_enabled ? config_.rx_pcap_dump_root : "";
    stream.config_json = body;
    stream.queues = std::move(queues);
    stream.worker_rates_mbps = std::move(worker_rates);
    stream.worker_burst_bytes = std::move(worker_bursts);
    std::string error;
    if (!tx_engine_ || !tx_engine_->start_stream(stream, error)) {
        release_stream_resources_locked(stream);
        sync_devices_from_streams_locked();
        sync_core_state_locked();
        status = 500;
        return error_json(error.empty() ? "start stream failed" : error);
    }
    streams_.push_back(stream);
    sync_devices_from_streams_locked();
    status = 201;
    std::ostringstream out;
    out << "{\"stream_id\":" << stream.id << ",\"status\":" << quote(stream.status);
    out << "}";
    return out.str();
}

std::string RuntimeState::start_stream(int id, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* stream = find_stream_locked(id);
    if (!stream) {
        status = 404;
        return error_json("stream not found");
    }
    if (!tx_engine_ready_locked()) {
        status = 503;
        return error_json(tx_engine_message_locked());
    }
    if (tx_engine_) {
        std::string error;
        if (!tx_engine_->start_stream(*stream, error)) {
            status = 500;
            return error_json(error.empty() ? "start stream failed" : error);
        }
    }
    stream->status = "running";
    sync_core_state_locked();
    status = 200;
    return "{\"ok\":true}";
}

std::string RuntimeState::stop_stream(int id, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* stream = find_stream_locked(id);
    if (!stream) {
        status = 404;
        return error_json("stream not found");
    }
    if (tx_engine_ && stream->status == "running") {
        std::string error;
        if (!tx_engine_->stop_stream(id, error)) {
            status = 500;
            return error_json(error.empty() ? "stop stream failed" : error);
        }
    }
    stream->status = "stopped";
    stream->actual_mbps = 0;
    stream->actual_pps = 0.0;
    sync_core_state_locked();
    status = 200;
    return "{\"ok\":true}";
}

std::string RuntimeState::delete_stream(int id, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = std::find_if(streams_.begin(), streams_.end(), [&](const StreamInfo& s) { return s.id == id; });
    if (iter == streams_.end()) {
        status = 404;
        return error_json("stream not found");
    }
    if (tx_engine_) {
        std::string error;
        if (!tx_engine_->delete_stream(id, error)) {
            status = 500;
            return error_json(error.empty() ? "delete stream failed" : error);
        }
    }
    release_stream_resources_locked(*iter);
    streams_.erase(iter);
    sync_devices_from_streams_locked();
    sync_core_state_locked();
    status = 200;
    return "{\"ok\":true}";
}

std::string RuntimeState::reset_stats(std::optional<int> port_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tx_engine_) {
        tx_engine_->reset_stats(port_id);
    }
    for (auto& row : stats_) {
        if (!port_id || row.port_id == *port_id) {
            row.total_tb = 0;
            row.tx_packets = 0;
            row.tx_packets_m = 0;
            row.tx_drops = 0;
            row.tx_nombuf = 0;
            row.rx_total_gb = 0;
            row.rx_packets = 0;
            row.rx_packets_m = 0;
            row.rx_drops = 0;
            row.rx_errors = 0;
            row.rx_nombuf = 0;
        }
    }
    return "{\"ok\":true}";
}

std::string RuntimeState::refresh_resources(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (target == "devices" || target == "all") {
        discover_devices_locked();
        sync_devices_from_streams_locked();
        initialize_stats_locked();
    }
    if (target == "cores" || target == "all") {
        initialize_cores_locked();
        sync_core_state_locked();
    }
    if (target == "pcap" || target == "all") {
        scan_pcap_files_locked();
    }
    return "{\"ok\":true}";
}

std::string RuntimeState::history_streams_json(const std::string& direction, int& status) {
    StreamHistoryStore tx_store(config_, "tx");
    StreamHistoryStore rx_store(config_, "rx");
    if (!tx_store.enabled()) {
        status = 200;
        return "{\"enabled\":false,\"streams\":[],\"error\":\"database disabled\"}";
    }

    std::vector<HistoryStreamInfo> history;
    const bool want_rx = direction == "all" || direction == "rx" || direction.empty();
    const bool want_tx = direction == "all" || direction == "tx" || direction.empty();
    if (want_tx) {
        auto tx_history = tx_store.load_streams();
        history.insert(history.end(), tx_history.begin(), tx_history.end());
    }
    if (want_rx) {
        auto rx_history = rx_store.load_streams();
        history.insert(history.end(), rx_history.begin(), rx_history.end());
    }
    std::sort(history.begin(), history.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.saved_at == rhs.saved_at) {
            return lhs.id > rhs.id;
        }
        return lhs.saved_at > rhs.saved_at;
    });

    std::ostringstream out;
    out << "{\"enabled\":true,\"streams\":[";
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& item = history[i];
        if (i) out << ',';
        out << "{";
        out << "\"id\":" << item.id << ",";
        out << "\"name\":" << quote(item.name) << ",";
        out << "\"direction\":" << quote(item.direction.empty() ? "tx" : item.direction) << ",";
        out << "\"mode\":" << quote(item.mode) << ",";
        out << "\"rate_mbps\":" << item.rate_mbps << ",";
        out << "\"worker_rates_mbps\":" << int_array(extract_json_int_array(item.config_json, "worker_rates_mbps")) << ",";
        out << "\"worker_burst_bytes\":" << int_array(extract_json_int_array(item.config_json, "worker_burst_bytes")) << ",";
        out << "\"config_summary\":" << history_config_summary_json(item.config_json) << ",";
        out << "\"config_json\":" << quote(item.config_json) << ",";
        out << "\"saved_at\":" << quote(item.saved_at);
        out << "}";
    }
    out << "]";
    const std::string error = !tx_store.last_error().empty() ? tx_store.last_error() : rx_store.last_error();
    if (!error.empty()) {
        out << ",\"error\":" << quote(error);
    }
    out << "}";
    status = error.empty() ? 200 : 500;
    return out.str();
}

std::string RuntimeState::restore_history_stream(long history_id, const std::string& body, int& status) {
    return restore_history_stream(extract_json_string(body, "direction", "tx"), history_id, body, status);
}

std::string RuntimeState::restore_history_stream(const std::string& direction_value, long history_id, const std::string& body, int& status) {
    std::string direction = direction_value;
    std::transform(direction.begin(), direction.end(), direction.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (direction != "rx") {
        direction = "tx";
    }
    StreamHistoryStore store(config_, direction);
    if (!store.enabled()) {
        status = 400;
        return error_json("database disabled");
    }

    auto item = store.load_stream(history_id);
    if (!item) {
        status = 404;
        return error_json(store.last_error().empty() ? "history stream not found" : store.last_error());
    }

    if (direction == "rx") {
        auto rx_port = extract_json_string(body, "rx_port", extract_json_string(body, "tx_port"));
        int core_count = std::max(1, extract_json_int(body, "core_count", 1));
        if (rx_port.empty()) {
            status = 400;
            return error_json("rx_port required");
        }
        std::ostringstream create_body;
        create_body << "{";
        create_body << "\"direction\":\"rx\",";
        create_body << "\"name\":" << quote(item->name + "-restore") << ",";
        create_body << "\"rx_port\":" << quote(rx_port) << ",";
        create_body << "\"core_count\":" << core_count << ",";
        create_body << "\"pcap_dump_enabled\":" << (extract_json_bool(body, "pcap_dump_enabled", extract_json_bool(item->config_json, "pcap_dump_enabled", false)) ? "true" : "false");
        create_body << "}";
        return create_stream(create_body.str(), status);
    }

    auto tx_port = extract_json_string(body, "tx_port");
    int core_count = extract_json_int(body, "core_count", 1);
    auto worker_rates = normalize_worker_rates_mbps(
        extract_json_int_array(body, "worker_rates_mbps").empty()
            ? extract_json_int_array(item->config_json, "worker_rates_mbps")
            : extract_json_int_array(body, "worker_rates_mbps"),
        core_count,
        item->rate_mbps);
    auto worker_bursts = normalize_worker_burst_bytes(
        extract_json_int_array(body, "worker_burst_bytes").empty()
            ? extract_json_int_array(item->config_json, "worker_burst_bytes")
            : extract_json_int_array(body, "worker_burst_bytes"),
        core_count);
    if (tx_port.empty()) {
        status = 400;
        return error_json("tx_port required");
    }

    const std::string mode = item->mode.empty() ? "construct" : item->mode;
    std::string pcap_path = extract_json_string(item->config_json, "pcap_path");
    std::ostringstream create_body;
    auto append_string = [&](const std::string& key, const std::string& fallback = "") {
        create_body << ",";
        append_json_string_field(create_body, key, extract_json_string(item->config_json, key, fallback));
    };
    auto append_int = [&](const std::string& key, int fallback) {
        create_body << ",";
        append_json_int_field(create_body, key, extract_json_int(item->config_json, key, fallback));
    };
    auto append_bool = [&](const std::string& key, bool fallback) {
        create_body << ",";
        append_json_bool_field(create_body, key, extract_json_bool(item->config_json, key, fallback));
    };

    create_body << "{";
    create_body << "\"name\":" << quote(item->name + "-restore") << ",";
    create_body << "\"tx_port\":" << quote(tx_port) << ",";
    create_body << "\"core_count\":" << core_count << ",";
    create_body << "\"mode\":" << quote(mode) << ",";
    create_body << "\"rate_mbps\":" << sum_rates_mbps(worker_rates) << ",";
    create_body << "\"worker_rates_mbps\":" << int_array(worker_rates) << ",";
    create_body << "\"worker_burst_bytes\":" << int_array(worker_bursts);
    if (mode == "pcap") {
        if (!pcap_path.empty()) {
            create_body << ",\"pcap_path\":" << quote(pcap_path);
        }
    } else {
        append_string("l3", "IPv4");
        append_string("l4", "UDP");
        append_string("src_mac", "02:00:00:00:00:01");
        append_string("dst_mac", "02:00:00:00:00:02");
        append_string("src_ip_addr", history_ip_addr(item->config_json, "src", "192.168.0.1"));
        append_int("src_ip_mask", history_ip_mask(item->config_json, "src", 32));
        append_string("src_ip_mode", "fixed");
        append_int("src_ip_step", 1);
        append_string("dst_ip_addr", history_ip_addr(item->config_json, "dst", "192.168.0.2"));
        append_int("dst_ip_mask", history_ip_mask(item->config_json, "dst", 32));
        append_string("dst_ip_mode", "fixed");
        append_int("dst_ip_step", 1);
        append_int("src_port_start", 10000);
        append_int("src_port_end", 10000);
        append_string("src_port_mode", "increment");
        append_int("src_port_step", 1);
        append_int("dst_port_start", 53);
        append_int("dst_port_end", 53);
        append_string("dst_port_mode", "increment");
        append_int("dst_port_step", 1);
        append_int("payload_len", 64);
        append_string("payload", "");
        append_bool("checksum_enabled", true);
    }
    create_body << "}";

    return create_stream(create_body.str(), status);
}

std::string RuntimeState::delete_history_stream(long history_id, int& status) {
    return delete_history_stream("tx", history_id, status);
}

std::string RuntimeState::delete_history_stream(const std::string& direction_value, long history_id, int& status) {
    std::string direction = direction_value;
    std::transform(direction.begin(), direction.end(), direction.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (direction != "rx") {
        direction = "tx";
    }
    StreamHistoryStore store(config_, direction);
    if (!store.enabled()) {
        status = 400;
        return error_json("database disabled");
    }
    if (!store.delete_stream(history_id)) {
        status = store.last_error() == "history stream not found" ? 404 : 500;
        return error_json(store.last_error().empty() ? "delete history stream failed" : store.last_error());
    }
    status = 200;
    return "{\"ok\":true}";
}

void RuntimeState::save_streams_to_database() {
    std::vector<StreamInfo> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = streams_;
    }

    StreamHistoryStore tx_store(config_, "tx");
    StreamHistoryStore rx_store(config_, "rx");
    if (!tx_store.enabled() || copy.empty()) {
        return;
    }
    if (!tx_store.save_streams(copy)) {
        std::cerr << "save tx stream history failed: " << tx_store.last_error() << std::endl;
    }
    if (!rx_store.save_streams(copy)) {
        std::cerr << "save rx stream history failed: " << rx_store.last_error() << std::endl;
    }
}

DeviceInfo* RuntimeState::find_device_locked(const std::string& pci) {
    auto iter = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& device) { return device.pci == pci; });
    return iter == devices_.end() ? nullptr : &*iter;
}

StreamInfo* RuntimeState::find_stream_locked(int id) {
    auto iter = std::find_if(streams_.begin(), streams_.end(), [&](const StreamInfo& stream) { return stream.id == id; });
    return iter == streams_.end() ? nullptr : &*iter;
}

PortStats* RuntimeState::find_stats_locked(int port_id) {
    auto iter = std::find_if(stats_.begin(), stats_.end(), [&](const PortStats& row) { return row.port_id == port_id; });
    return iter == stats_.end() ? nullptr : &*iter;
}

std::vector<int> RuntimeState::allocate_cores_locked(int count, const std::string& stream_name, bool running) {
    std::vector<int> allocated;
    for (auto& core : cores_) {
        if (!core.available || core.status != "idle") {
            continue;
        }
        core.status = running ? "used" : "locked";
        core.role = running ? "运行占用" : "保留占用";
        core.stream = stream_name;
        allocated.push_back(core.id);
        if (static_cast<int>(allocated.size()) == count) {
            break;
        }
    }
    return allocated;
}

std::vector<int> RuntimeState::allocate_queues_locked(const std::string& pci, int count, const std::string& direction) {
    std::vector<int> queues;
    auto* device = find_device_locked(pci);
    if (!device || count <= 0) {
        return queues;
    }

    const bool rx = direction == "rx";
    const int total = rx ? device->total_rx_queues : device->total_tx_queues;
    std::vector<bool> used(static_cast<size_t>(std::max(0, total)), false);
    for (const auto& stream : streams_) {
        const std::string stream_direction = stream.direction.empty() ? "tx" : stream.direction;
        const std::string stream_port = stream_direction == "rx" ? stream.rx_port : stream.tx_port;
        if (stream_direction != direction || stream_port != pci) {
            continue;
        }
        if (!stream.queues.empty()) {
            for (int queue : stream.queues) {
                if (queue >= 0 && queue < static_cast<int>(used.size())) {
                    used[static_cast<size_t>(queue)] = true;
                }
            }
        } else {
            for (int queue = 0; queue < static_cast<int>(stream.cores.size()) && queue < static_cast<int>(used.size()); ++queue) {
                used[static_cast<size_t>(queue)] = true;
            }
        }
    }

    for (int queue = 0; queue < static_cast<int>(used.size()) && static_cast<int>(queues.size()) < count; ++queue) {
        if (!used[static_cast<size_t>(queue)]) {
            queues.push_back(queue);
        }
    }
    return queues;
}

void RuntimeState::release_stream_resources_locked(const StreamInfo& stream) {
    for (int core_id : stream.cores) {
        auto iter = std::find_if(cores_.begin(), cores_.end(), [&](const CoreInfo& core) { return core.id == core_id; });
        if (iter != cores_.end()) {
            if (iter->available) {
                iter->status = "idle";
                iter->role = "空闲";
                iter->stream.clear();
            }
        }
    }
}

void RuntimeState::sync_devices_from_streams_locked() {
    for (auto& device : devices_) {
        device.streams.clear();
        device.tx_streams.clear();
        device.rx_streams.clear();
        device.used_tx_queues = 0;
        device.used_rx_queues = 0;
    }
    for (const auto& stream : streams_) {
        const std::string direction = stream.direction.empty() ? "tx" : stream.direction;
        auto* device = find_device_locked(direction == "rx" ? stream.rx_port : stream.tx_port);
        if (!device) {
            continue;
        }
        device->streams.push_back(stream.name);
        if (direction == "rx") {
            device->rx_streams.push_back(stream.name);
            device->used_rx_queues += static_cast<int>(stream.queues.empty() ? stream.cores.size() : stream.queues.size());
        } else {
            device->tx_streams.push_back(stream.name);
            device->used_tx_queues += static_cast<int>(stream.queues.empty() ? stream.cores.size() : stream.queues.size());
        }
    }
}

void RuntimeState::sync_core_state_locked() {
    for (auto& core : cores_) {
        if (!core.available || core.status == "unavailable" || core.status == "main") {
            continue;
        }
        bool found = false;
        for (const auto& stream : streams_) {
            if (std::find(stream.cores.begin(), stream.cores.end(), core.id) == stream.cores.end()) {
                continue;
            }
            found = true;
            core.stream = stream.name;
            if (stream.status == "running") {
                core.status = "used";
                core.role = "运行占用";
            } else {
                core.status = "locked";
                core.role = "保留占用";
            }
            break;
        }
        if (!found) {
            core.status = "idle";
            core.role = "空闲";
            core.stream.clear();
        }
    }
}

AppConfig load_config(const std::string& path) {
    AppConfig config;
    std::ifstream file(path);
    if (!file) {
        return config;
    }

    auto parse_list_value = [](std::string value) {
        std::vector<std::string> result;
        value = strip_quotes(std::move(value));
        if (value == "[]" || value.empty()) {
            return result;
        }
        if (value.front() == '[' && value.back() == ']') {
            value = value.substr(1, value.size() - 2);
        }
        std::stringstream ss(value);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = strip_quotes(item);
            if (!item.empty()) {
                result.push_back(item);
            }
        }
        return result;
    };

    auto is_device_list_key = [](const std::string& key) {
        return key == "DEVICE_LIST" || key == "device_list" || key == "ALLOW_PORTS";
    };

    auto set_scalar = [&](const std::string& key, const std::string& value) {
        if (key == "SYSTEM_NAME" || key == "system_name") config.system_name = value;
        else if (key == "AUTH_TOKEN") config.auth_token = value;
        else if (key == "HTTP_LISTEN") {
            auto split = value.rfind(':');
            if (split != std::string::npos) {
                config.listen_host = value.substr(0, split);
                config.listen_port = std::stoi(value.substr(split + 1));
            }
        }
        else if (key == "ENABLE_TLS") config.enable_tls = parse_bool(value);
        else if (key == "TLS_CERT_FILE") config.tls_cert_file = value;
        else if (key == "TLS_KEY_FILE") config.tls_key_file = value;
        else if (key == "PCAP_ROOT") config.pcap_root = value;
        else if (key == "PCAP_RECURSIVE") config.pcap_recursive = parse_bool(value);
        else if (key == "PCAP_MAX_SCAN_FILES") config.pcap_max_scan_files = std::stoi(value);
        else if (key == "PCAP_EXTENSIONS") config.pcap_extensions = parse_list_value(value);
        else if (key == "CORE_LIST") config.core_list = value;
        else if (key == "MAIN_LCORE") config.main_lcore = std::stoi(value);
        else if (key == "MEM_CHANNELS") config.mem_channels = std::stoi(value);
        else if (key == "FILE_PREFIX") config.file_prefix = value;
        else if (key == "PROC_TYPE") config.proc_type = value;
        else if (is_device_list_key(key)) {
            config.device_list = parse_list_value(value);
            config.allow_ports = config.device_list;
        }
        else if (key == "TX_RING_SIZE" || key == "tx_ring_size") config.tx_ring_size = std::stoi(value);
        else if (key == "RX_RING_SIZE" || key == "rx_ring_size") config.rx_ring_size = std::stoi(value);
        else if (key == "TX_QUEUE_PER_PORT" || key == "tx_queue_per_port") config.tx_queue_per_port = std::stoi(value);
        else if (key == "RX_QUEUE_PER_PORT" || key == "rx_queue_per_port") config.rx_queue_per_port = std::stoi(value);
        else if (key == "MBUF_POOL_SIZE" || key == "mbuf_pool_size") config.mbuf_pool_size = std::stoi(value);
        else if (key == "MBUF_CACHE_SIZE" || key == "mbuf_cache_size") config.mbuf_cache_size = std::stoi(value);
        else if (key == "MAX_BURST" || key == "max_burst") config.max_burst = std::stoi(value);
        else if (key == "RATE_ACCOUNTING") config.rate_accounting = value;
        else if (key == "STATS_INTERVAL_MS" || key == "stats_interval_ms") config.stats_interval_ms = std::stoi(value);
        else if (key == "MOCK_INITIAL_STREAMS") config.mock_initial_streams = parse_bool(value);
        else if (key == "TX_ENGINE_MODE") config.tx_engine_mode = value;
        else if (key == "RX_PCAP_DUMP_ROOT" || key == "rx_pcap_dump_root") config.rx_pcap_dump_root = value;
        else if (key == "RX_PCAP_MAX_FILE_MB" || key == "rx_pcap_max_file_mb") config.rx_pcap_max_file_mb = std::stoi(value);
        else if (key == "RX_PCAP_STOP_ON_ERROR" || key == "rx_pcap_stop_on_error") config.rx_pcap_stop_on_error = parse_bool(value);
        else if (key == "DB_ENABLED") config.db_enabled = parse_bool(value);
        else if (key == "DB_ENDPOINT") config.db_endpoint = value;
        else if (key == "DB_USER") config.db_user = value;
        else if (key == "DB_PASSWORD") config.db_password = value;
        else if (key == "DB_NAME") config.db_name = value;
        else if (key == "DB_STREAM_TABLE") {
            config.db_stream_table = value;
            config.db_tx_stream_table = value;
        }
        else if (key == "DB_TX_STREAM_TABLE" || key == "db_tx_stream_table") config.db_tx_stream_table = value;
        else if (key == "DB_RX_STREAM_TABLE" || key == "db_rx_stream_table") config.db_rx_stream_table = value;
    };

    std::string active_list_key;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!active_list_key.empty() && line.rfind("- ", 0) == 0) {
            auto value = strip_quotes(line.substr(2));
            if (is_device_list_key(active_list_key)) {
                config.device_list.push_back(value);
                config.allow_ports = config.device_list;
            } else if (active_list_key == "PCAP_EXTENSIONS") {
                config.pcap_extensions.push_back(value);
            }
            continue;
        }
        active_list_key.clear();

        auto pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        auto key = trim(line.substr(0, pos));
        auto raw_value = trim(line.substr(pos + 1));
        if (raw_value.empty()) {
            active_list_key = key;
            if (is_device_list_key(key)) {
                config.device_list.clear();
                config.allow_ports.clear();
            } else if (key == "PCAP_EXTENSIONS") {
                config.pcap_extensions.clear();
            }
            continue;
        }
        auto value = strip_quotes(raw_value);
        set_scalar(key, value);
    }

    if (config.pcap_extensions.empty()) {
        config.pcap_extensions.push_back(".pcap");
    }
    return config;
}

} // namespace dptx
