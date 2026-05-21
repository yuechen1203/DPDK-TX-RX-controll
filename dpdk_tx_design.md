# DPDK 发包工具设计文档

## 1. 目标

基于当前 `sample` 工程的 C++/DPDK 初始化和线程布置方式，设计一个支持远程控制的发包工具。工具由一个 C++ 后端进程和一个前端控制台组成，后端负责 DPDK EAL 初始化、设备和 lcore 资源管理、stream 生命周期管理、PCAP 循环发包、自定义构造包发包、速率控制和统计上报；前端只通过后端暴露的信息展示和下发配置，不直接假设设备、核或队列资源。

本阶段产出是设计文档和前端静态样例，不实现后端接口调用。

## 2. sample 工程参考点

当前 `sample` 中可复用的框架思想如下：

- CMake 使用 C++17，显式链接 DPDK 组件库，并链接 `pthread`、`pcap`、`spdlog` 等依赖。新发包工具可以沿用这种工程组织方式。
- `dpdkManager::load_config()` 从 YAML 中读取 `RX_PORTS`、`TX_PORTS`、`CORE_LIST`、`PCAP_TX_LBK_PORT` 等配置，适合作为新工具的配置入口参考。
- `dpdkManager::init()` 拼装 EAL 参数：`-l CORE_LIST`、`-n 2`、`--file-prefix`、`--proc-type primary`、设备白名单 `-a <pci>`，然后调用 `rte_eal_init()`。
- 初始化后通过 `rte_lcore_is_enabled()` 得到可用 lcore，并排除 `rte_get_main_lcore()`，避免 main core 执行业务发包。
- `dpdk_device::enable()` 负责 PCI 名称到 port id 的解析、`rte_eth_dev_configure()`、RX/TX queue setup、启动网口和混杂模式开启。
- `dpdkManager::start_work()` 使用 `rte_eal_remote_launch()` 把工作线程投递到指定 lcore。
- `start_pcap_loop()` 已经有 PCAP 离线读取到内存、按 queue 发包的雏形，可以演进为 stream 化的 PCAP packet source。

新工具建议保留这些边界：EAL 初始化只在进程启动时发生；main lcore 作为控制面；worker lcore 只做高速发包；设备和核心资源由后端统一分配并上报给前端。

## 3. 总体架构

后端进程建议命名为 `dpdk-tx`，内部模块如下：

| 模块 | 职责 |
| --- | --- |
| `ConfigLoader` | 读取 YAML/命令行配置，生成 EAL 参数、控制台监听地址、PCAP 文件根目录等 |
| `EalManager` | 封装 `rte_eal_init()`、lcore 枚举、main lcore 识别、NUMA 信息读取 |
| `DeviceManager` | 管理 DPDK port、PCI 地址、TX queue 数量、链路状态、port 统计 |
| `CoreAllocator` | 管理可用 lcore，记录 core 被哪个 stream 占用，删除 stream 前不释放 |
| `QueueAllocator` | 管理每个 TX port 的 queue id，确保一个 worker 独占一个 TX queue |
| `StreamManager` | 创建、启动、停止、删除 stream，维护 stream 状态机 |
| `PacketSource` | 抽象包来源，包含 `PcapPacketSource` 和 `ConstructPacketSource` |
| `BurstPacer` | 基于 DPDK TSC 和每核突发区周期闸门的 Mbps 限速 |
| `StatsCollector` | 聚合 stream、worker、port 统计，提供前端轮询或 WebSocket 推送 |
| `ControlApi` | 运行在 main lcore，提供 HTTP/WebSocket API 和静态前端页面 |
| `FileBrowser` | 限定在配置的 PCAP 根目录下浏览文件，避免任意路径访问 |

推荐进程模型：

```text
dpdk-tx process
  main lcore:
    EAL init
    HTTP/WebSocket control API
    stream/core/device manager
    stats aggregation

  worker lcores:
    stream worker 0 -> tx port A queue 0
    stream worker 1 -> tx port A queue 1
    stream worker 2 -> tx port B queue 0
```

前端可以部署在远端浏览器中，也可以由 `dpdk-tx` 直接托管静态文件。前端设备列表、核列表、端口状态、stream 状态全部来自后端 API。

## 4. 启动和配置

EAL 参数必须在进程启动时确定，因此前端只能选择已经被 EAL 初始化的设备和 lcore。

建议 YAML：

```yaml
EAL:
  CORE_LIST: "0-7"
  MEM_CHANNELS: 2
  FILE_PREFIX: "dpdk-tx"
  PROC_TYPE: "primary"
  device_list:
    - "0002:05:00.0"
    - "0002:06:00.0"

TX:
  TX_RING_SIZE: 4096
  MBUF_POOL_SIZE: 262143
  MBUF_CACHE_SIZE: 512
  MAX_BURST: 32
  DEFAULT_RATE_UNIT: "Mbps"
  RATE_ACCOUNTING: "wire"   # wire 或 l2

CONTROL:
  HTTP_LISTEN: "0.0.0.0:8080"
  PCAP_ROOT: "/data/pcap"
  ENABLE_TLS: false
  AUTH_TOKEN: ""
```

启动命令示例：

```bash
./dpdk-tx -f config.yaml
```

后端内部生成 EAL 参数：

```text
./dpdk-tx-eal -l 0-7 -n 2 --file-prefix dpdk-tx --proc-type primary \
  -a 0002:05:00.0 -a 0002:06:00.0
```

main lcore 会从可发包核心池中排除。TX queue 数量建议在端口启动前按可用 worker lcore 数预分配，例如可用 worker lcore 为 7 个，则每个 TX port 配置 7 个 TX queue。运行期创建 stream 时只分配已有 queue，不在端口运行中重配队列。

## 5. Stream 模型

一个 stream 是前端可管理的一条发包业务，包含包来源、TX 设备、占用核心、总速率、运行状态和统计。

状态机：

```text
CREATED -> RUNNING -> STOPPING -> STOPPED -> RUNNING
CREATED -> DELETED
STOPPED -> DELETED
RUNNING -> ERROR -> STOPPED
```

资源规则：

- 创建 stream 时选择 TX port 和 core 数量。
- 后端从 `CoreAllocator` 分配指定数量的空闲 worker lcore。
- 每个 worker lcore 在对应 TX port 上独占一个 TX queue。
- stream 停止后保留 core 和 queue 占用，便于再次启动使用相同资源。
- 只有删除 stream 才释放 core 和 queue。
- core 不足、queue 不足、TX port 链路异常、PCAP 文件不可读、构造包参数非法时创建失败，并返回明确错误。

多核心 stream：

- 一个 stream 使用多个 CPU 核时，后端为每个核启动一个 DPDK worker。
- 每个 worker 使用同一个 stream 配置，执行同样的发包逻辑。
- stream 配置中的 Mbps 表示该 stream 的总速率，后端默认按 worker 数均分到每个 worker。
- 如果需要“每核心 Mbps”，可以作为前端高级选项，但默认总速率更符合控制台直觉。

## 6. PCAP 发包模式

PCAP 模式流程：

1. 前端通过文件选择弹窗选择 PCAP 根目录下的文件。
2. 后端使用 `pcap_open_offline()` 打开文件。
3. 逐包读取 `caplen` 和包内容，复制到只读缓存 `std::vector<PacketTemplate>`。
4. 校验包长不超过端口 MTU 或 jumbo 配置，过短以太帧按需要补齐到最小 L2 长度。
5. worker 发送时从缓存循环取包，分配 mbuf，复制原始 L2 bytes，设置 `data_len`、`pkt_len`、`port`。
6. 调用 `rte_eth_tx_burst()` 批量发送，未发送的 mbuf 释放并计入 drop/backpressure 统计。

建议结构：

```cpp
struct PacketTemplate {
    std::vector<uint8_t> bytes;
    uint32_t l2_len;
    uint32_t wire_bits;
};

class PcapPacketSource : public PacketSource {
public:
    bool load(const std::string& path);
    uint16_t fill_burst(rte_mempool* pool, rte_mbuf** bufs, uint16_t max_burst);
};
```

性能优化路径：

- 基础版使用 `rte_pktmbuf_alloc_bulk()` 批量申请 mbuf，逐包 memcpy，简单可靠。
- PCAP 包较大且重复发送时，可评估 indirect mbuf 或 external buffer，降低拷贝成本，但要严格管理 refcnt 和生命周期。
- 按 NUMA socket 为 worker 创建 mempool，减少跨 socket 内存访问。

## 7. 构造包发包模式

构造模式由前端填写以太层、IP 层、L4 层和 payload 参数，后端编译为 `ConstructPacketSource`。

支持字段建议：

| 层级 | 字段 |
| --- | --- |
| Ethernet | src mac、dst mac、ether type，可选 VLAN |
| IPv4 | src/dst IP 地址、mask、选择模式（随机/递增/固定）、递增步长 |
| IPv6 | src/dst 前缀或单地址、hop limit、flow label |
| TCP | src/dst port 起止范围、选择模式（随机/递增）、flags、seq/ack 初值、window |
| UDP | src/dst port 起止范围、选择模式（随机/递增） |
| Payload | 固定长度、hex/text 内容、递增 pattern、随机 pattern |

范围语法：

- IPv4：地址输入框 + mask 输入框，例如 `192.168.1.10` + `24`；固定模式强制 mask 为 `32`。
- IPv6：`2001:db8::1` 或 `2001:db8:1::/64`
- TCP/UDP port：起始端口输入框 - 结束端口输入框，`start=end` 即固定端口。

构造策略：

- 后端按 `ip_addr/ip_mask` 计算起止地址，worker 每包按固定、递增或随机模式选择源/目的 IP。
- L4 端口范围解析为 `start_port` 和 `end_port`，worker 每包按递增或随机模式选择；`start=end` 时自然固定。
- payload 长度不足以达到以太网最小帧时补齐；超过 MTU 时拒绝或要求启用 jumbo。
- IPv4 header checksum 使用 `rte_ipv4_cksum()`。
- TCP/UDP checksum 可使用 `rte_ipv4_udptcp_cksum()`、`rte_ipv6_udptcp_cksum()`，或在确认网卡支持时使用 TX checksum offload。
- 构造包模板中不变字段预先写好，worker 只修改变化字段和 checksum。

## 8. 指定速率发包实现

速率控制建议在每个 worker 内实现，使用 DPDK TSC 周期接口：

- `rte_get_tsc_hz()` 获取每秒周期数。
- `rte_get_tsc_cycles()` 获取当前周期。
- 发包等待路径不调用 `sleep`、`rte_delay_us_sleep()` 或 `rte_pause()`，只满负荷循环检查 TSC。

### 8.1 速率口径

前端输入单位为 Mbps。后端统一转换：

```text
stream_bps = mbps * 1000 * 1000
worker_bps = stream_bps / worker_count
```

建议默认按线速口径统计，即：

```text
wire_bits = (l2_packet_len + ethernet_overhead) * 8
ethernet_overhead = 20 bytes  # preamble 8 + IFG 12
```

如果用户希望只按 L2 帧长度限速，可将 `ethernet_overhead` 配置为 0。前端可以显示“线速口径 / L2 口径”切换。

### 8.2 突发区周期闸门方案

每个 worker 独立配置发送速率和突发区大小。突发区大小范围为 `2048-16384 bytes`，worker 启动时按该 worker 的速率计算发送一个突发区所需的 TSC 周期数。

每个 worker 维护：

```cpp
struct BurstPacer {
    uint64_t tsc_hz;
    uint64_t rate_bps;
    uint64_t burst_bytes;
    uint64_t burst_bits;
    uint64_t cycles_per_burst;
    uint64_t cycle_stamp;
    int64_t remaining_bits;
};
```

初始化时：

```cpp
p.tsc_hz = rte_get_tsc_hz();
p.rate_bps = worker_rate_mbps * 1000 * 1000;
p.burst_bytes = worker_burst_bytes;
p.burst_bits = p.burst_bytes * 8;
p.cycles_per_burst = p.burst_bits * p.tsc_hz / p.rate_bps;
p.cycle_stamp = rte_get_tsc_cycles();
p.remaining_bits = p.burst_bits;
```

每填充一个 mbuf 后扣减突发区额度：

```cpp
p.remaining_bits -= packet_bits;
if (p.remaining_bits <= 0) {
    uint64_t target = p.cycle_stamp + p.cycles_per_burst;
    while ((int64_t)(rte_get_tsc_cycles() - target) < 0) {
    }
    p.cycle_stamp = rte_get_tsc_cycles();
    p.remaining_bits += p.burst_bits;
}
```

`rte_eth_tx_burst()` 与突发区逻辑解耦，worker 仍按 `MAX_BURST` 攒满一批 mbuf 后提交一次。

### 8.3 worker 主循环

伪代码：

```cpp
while (stream_state == RUNNING) {
    rte_mbuf* bufs[MAX_BURST];
    uint16_t n = 0;

    while (n < MAX_BURST) {
        auto pkt_meta = source.peek_next();
        bufs[n] = source.build_next_mbuf(pool);
        if (bufs[n] == nullptr) {
            stats.nombuf++;
            break;
        }
        n++;
        pacer.consume(pkt_meta.wire_bits);
    }

    if (n == 0) {
        continue;
    }

    uint16_t sent = rte_eth_tx_burst(port_id, queue_id, bufs, n);
    stats.tx_packets += sent;
    stats.tx_bytes += sum_l2_len(bufs, sent);

    if (sent < n) {
        rte_pktmbuf_free_bulk(&bufs[sent], n - sent);
        stats.tx_dropped += (n - sent);
    }
}
```

突发区等待逻辑：

```cpp
void consume(BurstPacer& p, uint64_t packet_bits) {
    p.remaining_bits -= packet_bits;
    if (p.remaining_bits > 0) {
        return;
    }

    uint64_t target = p.cycle_stamp + p.cycles_per_burst;
    while ((int64_t)(rte_get_tsc_cycles() - target) < 0) {
    }
    p.cycle_stamp = rte_get_tsc_cycles();
    p.remaining_bits += p.burst_bits;
}
```

等待期间不调用 `sleep`、`rte_delay_us_sleep()` 或 `rte_pause()`，而是满负荷循环读取 TSC。
发包线程只在每组 `MAX_BURST` 提交后读取一次停止标志，降低热路径中的原子读取开销。

### 8.4 多核心速率分配

stream 不再只配置一个总速率。前端为每个 worker 单独配置 `worker_rates_mbps[i]` 和 `worker_burst_bytes[i]`，后端按 worker 顺序绑定到 `cores[i]` 和 `queues[i]`。

每个 worker 独立 pacer，stream 目标总速率为所有 `worker_rates_mbps` 之和。统计汇总时由 main lcore 将所有 worker 的 `tx_bits / interval` 加总，展示 stream 实际 Mbps。

### 8.5 运行期调整速率

前端修改速率时，ControlApi 更新 stream 配置中的 atomic rate：

```cpp
std::atomic<uint64_t> target_bps;
std::atomic<uint64_t> config_version;
```

worker 在循环中定期检查 `config_version`，发现变化后：

1. 读取新的 stream 总速率。
2. 按当前 worker 数重新计算本 worker rate。
3. 更新 `BurstPacer::rate_bps`、`burst_bits` 和 `cycles_per_burst`。
4. 重置 `cycle_stamp` 和突发区剩余额度，避免旧配置影响新速率。

### 8.6 准确性和边界处理

- `rte_eth_tx_burst()` 返回小于请求数量时，未发送 mbuf 必须释放或进入短重试，避免内存泄漏。
- 如果 TX ring 长期拥塞，实际速率低于目标速率，应在前端显示 backpressure/drop。
- 单 worker 速率范围为 `100-10000 Mbps`；无限速应显式选择 `unlimited`，不要用 0 表示。
- Mbps 上限应参考端口 link speed，并在创建 stream 时校验同端口所有 running stream 的总速率。
- 对于小包，高 Mbps 可能受 PPS 上限限制；前端应同时展示 PPS 和 Mbps。
- PCAP 文件包含变长包时，按每包实际 wire bits 扣减突发区剩余额度。
- 构造模式固定长度时，可预计算 `wire_bits`，降低循环开销。

## 9. 控制 API 草案

前端不直接调用 DPDK，只调用后端控制面 API。

```text
GET    /api/runtime
GET    /api/devices
GET    /api/cores
GET    /api/streams
POST   /api/streams
POST   /api/streams/{id}/start
POST   /api/streams/{id}/stop
PATCH  /api/streams/{id}
DELETE /api/streams/{id}
GET    /api/files?dir=/pcap/subdir
GET    /api/stats
WS     /api/events
```

`GET /api/runtime` 返回示例：

```json
{
  "started": true,
  "eal": {
    "main_lcore": 0,
    "enabled_lcores": [0, 1, 2, 3, 4, 5, 6, 7],
    "worker_lcores": [1, 2, 3, 4, 5, 6, 7],
    "device_list": ["0002:05:00.0", "0002:06:00.0"]
  },
  "version": "0.1.0"
}
```

## 10. 前端页面建议

除需求中列出的功能外，建议补充：

- 连接状态：显示后端是否在线、启动时间、EAL file prefix、main lcore。
- 资源视图：按颜色展示空闲 core、已占用 core、main core、不可用 core。
- 端口视图：显示 PCI、port id、link up/down、速率、socket、TX queue 使用量。
- stream 列表：显示状态、模式、TX 设备、占用 core、目标 Mbps、实际 Mbps、PPS、drop。
- 创建向导：基础配置、包来源、速率与资源确认分步展示，减少误配置。
- 参数校验：IP/mask、MAC、端口范围、payload 长度、每核速率和突发区范围实时校验。
- PCAP 摘要：包数、最小/最大/平均包长、预计 PPS、文件大小。
- 包预览：构造模式展示 L2/L3/L4 字段摘要和 payload hex 预览。
- 批量操作：停止全部 stream、导出/导入 stream 配置、复制 stream。
- 告警提示：core 不足、queue 不足、端口链路 down、TX backpressure、nombuf。
- 权限和远程部署：token 登录、HTTPS、只读观察者模式、操作审计日志。

## 11. 实施顺序建议

1. 搭建新 C++ 工程骨架，沿用 sample 的 CMake DPDK 链接方式。
2. 实现 EAL 初始化、设备枚举、main lcore 排除、TX queue 预分配。
3. 实现 `CoreAllocator`、`QueueAllocator`、`StreamManager`。
4. 实现 PCAP 模式 packet source 和基础 worker 发包。
5. 加入 `BurstPacer`，用统计验证不同包长下的 Mbps。
6. 实现构造包模式和 checksum。
7. 实现 HTTP/WebSocket 控制 API。
8. 接入前端页面并增加校验、统计和告警。
