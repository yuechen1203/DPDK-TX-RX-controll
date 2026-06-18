# DPDK TX/RX 控制台扩展设计

本文基于当前系统功能，设计新增 RX 收包 stream、RX pcap dump、RX/TX 统计展示、历史 stream 区分、设置页补充信息，以及移除运行时 `TX_ENGINE_ENABLED` 配置。本文只描述设计，不包含实际开发改动。

## 1. 目标

系统名称调整为：

```text
DPDK TX/RX 控制台
```

新增能力：

- 创建 stream 时选择 `TX` 或 `RX`。
- `TX` 保持当前 PCAP 发包、构造包发包、每核速率、每核突发区等能力。
- `RX` 支持选择白名单设备作为收包口，配置 RX 收包线程数。
- `RX` 支持 `pcap_dump` 开关。打开后，每个 RX worker 单独写一个 pcap 文件到配置文件指定目录。
- 运行中 stream 和统计页同时展示 TX/RX 方向的实时速率、带宽、累计数据、包数、drop/error。
- 历史 stream 区分 RX 和 TX，数据库使用不同表保存。
- 设置页展示 ring、mbuf、数据库、RX dump 路径等关键运行配置。
- 去除配置文件中的 `TX_ENGINE_ENABLED` 字段。

## 2. 关键约束

### 2.1 RX 不应侵入 TX worker

新增 RX 功能时，不能把 RX 逻辑写进现有 TX worker 主循环，也不能让 TX 构造包、TX PCAP、TX 限速器依赖 RX 类型判断。

设计目标是：

- TX worker 仍只做发包。
- RX worker 只做收包和可选 dump。
- TX/RX 共享的部分仅限 DPDK EAL、端口、mempool、统计快照、lcore/queue 资源分配这些底层资源。
- `RuntimeState` 只做控制面编排，不承担 TX/RX 数据面细节。

### 2.2 DPDK port 初始化是唯一必须共享的部分

DPDK 的 RX/TX queue 数量需要在 `rte_eth_dev_configure()` 阶段确定，端口启动后不能再随意追加 RX queue。

当前 TX 初始化逻辑类似：

```cpp
rte_eth_dev_configure(port_id, 0, tx_queues, &port_conf);
rte_eth_tx_queue_setup(port_id, queue, tx_desc, socket_id, nullptr);
rte_eth_dev_start(port_id);
```

支持 RX 后，端口初始化必须变为：

```cpp
rte_eth_dev_configure(port_id, rx_queues, tx_queues, &port_conf);
rte_eth_rx_queue_setup(port_id, queue, rx_desc, socket_id, nullptr, mbuf_pool);
rte_eth_tx_queue_setup(port_id, queue, tx_desc, socket_id, nullptr);
rte_eth_dev_start(port_id);
```

这是无法完全避免的公共改动。但 RX stream 逻辑本身不应进入 TX worker，也不应改变 TX 限速、构造包填充、PCAP 循环发送等代码路径。

## 3. 推荐模块拆分

当前 `TxEngine` 同时承担 DPDK EAL 初始化、端口初始化、TX worker、统计读取等职责。为了避免 RX 和 TX 耦合，建议拆为以下模块。

| 模块 | 职责 |
| --- | --- |
| `DpdkContext` | 负责 EAL 初始化、mempool 创建、端口发现、RX/TX queue setup、端口启动、端口统计读取 |
| `TxStreamEngine` | 只负责 TX stream 的创建、启动、停止、删除，以及 TX worker |
| `RxStreamEngine` | 只负责 RX stream 的创建、启动、停止、删除、pcap dump，以及 RX worker |
| `StreamRuntimeRegistry` | 保存运行中的 stream 元数据，按 stream id 查询、更新运行状态和统计 |
| `RuntimeState` | HTTP 控制面编排，按 `direction` 把请求派发给 TX 或 RX engine |
| `StreamHistoryStore` | 历史 stream 数据库访问，按 RX/TX 选择不同表 |

推荐文件组织：

```text
back-app/include/dptx/DpdkContext.h
back-app/include/dptx/TxStreamEngine.h
back-app/include/dptx/RxStreamEngine.h
back-app/include/dptx/StreamTypes.h
back-app/src/DpdkContext.cpp
back-app/src/TxStreamEngine.cpp
back-app/src/RxStreamEngine.cpp
```

为了降低风险，迁移顺序建议是：

1. 先抽出 `DpdkContext`，保持 TX 行为不变。
2. 让现有 TX 逻辑通过 `DpdkContext` 获取 port、queue、mempool。
3. 再新增 `RxStreamEngine`。
4. 最后扩展 API 和前端。

这样可以把“重构公共层”和“新增 RX 功能”分开验证，避免一次性改动 TX 数据面。

## 4. 配置文件设计

### 4.1 删除字段

从 `default.yaml` 删除：

```yaml
TX_ENGINE_ENABLED: true
```

删除后语义变为：

- 以 DPDK 方式构建的后端默认尝试初始化 DPDK。
- 如果 DPDK 初始化失败，`/api/runtime` 返回 engine 不可用，前端提示后端 DPDK 引擎未就绪。
- 非 DPDK 构建只保留控制面 mock/检查能力，但不允许创建真实 TX/RX stream。

保守实现可以在解析器中暂时忽略旧配置里的 `TX_ENGINE_ENABLED`，但不再出现在 `default.yaml` 和设置页。

### 4.2 新增/调整字段

建议配置项统一使用 lower_snake_case。为了兼容当前已有大写字段，解析器可以短期同时接受大写和小写，但 `default.yaml` 推荐只保留小写。

```yaml
# 系统展示名称。
system_name: "DPDK TX/RX 控制台"

# DPDK EAL 可用 lcore 列表。
core_list: "0-7"

# main lcore。
main_lcore: 0

# DPDK device allowlist。非空时等价于 EAL -a <pci>；空数组时扫描 vfio-pci 设备。
device_list: []

# 每个 TX queue 的 descriptor 数量。后端启动初始化端口时生效。
tx_ring_size: 4096

# 每个 RX queue 的 descriptor 数量。所有 RX worker 的 RX queue 共用同一个配置值。
rx_ring_size: 1024

# 每个端口预分配的 TX queue 数。0 表示按 worker lcore 数自动计算。
tx_queue_per_port: 0

# 每个端口预分配的 RX queue 数。0 表示按 worker lcore 数自动计算。
rx_queue_per_port: 0

# DPDK mbuf pool 总 mbuf 数。
mbuf_pool_size: 262143

# DPDK mbuf cache 大小。
mbuf_cache_size: 512

# TX/RX burst 上限。TX 满 32 个 mbuf 调 tx_burst；RX 每轮最多收 32 个 mbuf。
max_burst: 32

# 前端统计刷新周期。
stats_interval_ms: 3000

# RX pcap dump 保存目录。
rx_pcap_dump_root: "/home/huang/rx_pcap"

# RX pcap 文件最大大小，MB。0 表示不轮转。
rx_pcap_max_file_mb: 0

# RX pcap dump 写入失败时是否停止对应 RX stream。
rx_pcap_stop_on_error: true

# 数据库开关。
db_enabled: true

# 数据库地址，格式 ip:port。
db_endpoint: "127.0.0.1:3306"

# 数据库用户名。
db_user: "root"

# 数据库密码。设置页展示时必须脱敏。
db_password: ""

# 数据库名。
db_name: "dpdk_tx"

# TX 历史 stream 表名。
db_tx_stream_table: "tx_stream_history"

# RX 历史 stream 表名。
db_rx_stream_table: "rx_stream_history"
```

补充说明：

- 用户要求增加 `rx_ring_size`，当前配置里已有 `RX_RING_SIZE`。实现时建议把 `rx_ring_size` 作为新 canonical 字段，同时保留读取 `RX_RING_SIZE` 的兼容逻辑。
- 用户要求设置页展示 `rx_ring_size`、`tx_ring_size`、mbuf 池大小等字段，因此 `/api/runtime` 或新增 `/api/settings` 需要返回这些值。
- `rx_queue_per_port` 是补充字段。原因是 RX stream 每个收包线程通常需要独立 RX queue，如果只配置 `rx_ring_size`，仍缺少“每端口初始化多少个 RX queue”的信息。默认 `0` 时可按 `core_list` 中 worker lcore 数推导，保持配置简单。

## 5. 数据模型设计

### 5.1 Stream 方向

新增枚举：

```cpp
enum class StreamDirection {
    Tx,
    Rx
};
```

`StreamInfo` 建议扩展为：

```cpp
struct StreamInfo {
    int id;
    std::string name;
    StreamDirection direction;  // tx/rx
    std::string status;         // running/stopped/error

    std::string port;           // TX 时是 tx_port，RX 时是 rx_port
    std::vector<int> cores;
    std::vector<int> queues;

    // TX only
    std::string tx_mode;        // pcap/construct
    int target_mbps;
    std::vector<int> worker_rates_mbps;
    std::vector<int> worker_burst_bytes;

    // RX only
    bool pcap_dump_enabled;
    std::string pcap_dump_dir;

    // runtime stats
    int actual_mbps;
    double actual_pps;
    double total_gb;
    uint64_t packets;
    uint64_t drops;
    uint64_t errors;

    std::string config_json;
};
```

兼容规则：

- 老的 TX 创建请求如果没有 `direction`，默认按 `direction="tx"` 处理。
- 老的 `mode` 字段继续表示 TX 的 `pcap/construct`。
- 新增 RX 时，不再把 `mode="rx"` 混入 TX 的 `pcap/construct` 语义，避免歧义。

### 5.2 DeviceInfo

设备需要同时展示 TX/RX queue 占用：

```cpp
struct DeviceInfo {
    std::string pci;
    int port_id;
    bool link_up;
    std::string link_speed;
    int socket_id;
    std::string driver;

    int total_tx_queues;
    int used_tx_queues;
    int total_rx_queues;
    int used_rx_queues;

    bool available;
    std::string unavailable_reason;
    std::vector<std::string> tx_streams;
    std::vector<std::string> rx_streams;
};
```

### 5.3 PortStats

端口统计同时返回 TX/RX：

```cpp
struct PortStats {
    std::string pci;
    int port_id;

    int tx_mbps;
    double tx_pps;
    double tx_total_gb;
    uint64_t tx_packets;
    uint64_t tx_drops;

    int rx_mbps;
    double rx_pps;
    double rx_total_gb;
    uint64_t rx_packets;
    uint64_t rx_drops;
    uint64_t rx_errors;
    uint64_t rx_nombuf;
};
```

DPDK 字段来源：

| 展示字段 | DPDK 统计来源 |
| --- | --- |
| TX bytes | `rte_eth_stats.obytes` |
| TX packets | `rte_eth_stats.opackets` |
| TX drops/errors | `rte_eth_stats.oerrors` |
| RX bytes | `rte_eth_stats.ibytes` |
| RX packets | `rte_eth_stats.ipackets` |
| RX missed/drops | `rte_eth_stats.imissed` |
| RX errors | `rte_eth_stats.ierrors` |
| RX no mbuf | `rte_eth_stats.rx_nombuf` |

## 6. RX stream 创建流程

请求示例：

```json
{
  "direction": "rx",
  "name": "rx-stream-1",
  "rx_port": "0002:01:00.1",
  "core_count": 4,
  "pcap_dump_enabled": true
}
```

后端流程：

1. 校验 DPDK runtime 已就绪。
2. 校验 `rx_port` 属于 `device_list` 或 vfio-pci 扫描出的设备，并且 link up。
3. 校验 `core_count >= 1`。
4. 从空闲 lcore 中分配 `core_count` 个 RX worker lcore。
5. 从该 port 的 RX queue 池中分配 `core_count` 个 RX queue。
6. 如果 `pcap_dump_enabled=true`：
   - 校验 `rx_pcap_dump_root` 存在或可创建。
   - 校验目录可写。
   - 为每个 worker 生成独立 pcap 文件路径。
7. 创建 `RxStreamContext`。
8. 对每个 worker 调用 `rte_eal_remote_launch(rx_worker_main, ctx, lcore_id)`。
9. 将 stream 状态写入运行态表。

RX stream 不配置目标速率，因为收包由外部流量决定。界面展示实际 RX Mbps、pps、累计收包、drop/error。

## 7. RX worker 工作流程

每个 RX worker 独占一个 lcore 和一个 RX queue。

启动时：

1. 绑定 port id、queue id、lcore id。
2. 保存 stream id/name。
3. 如果开启 dump，打开 worker 独立 pcap 文件并写入 classic pcap global header。
4. 初始化计数器：
   - `packets`
   - `bytes`
   - `drops`
   - `errors`
   - `dumped_packets`
   - `dump_errors`

主循环：

```text
while running:
    n = rte_eth_rx_burst(port_id, queue_id, mbufs, max_burst)
    if n == 0:
        continue

    for each mbuf:
        pkt_len = rte_pktmbuf_pkt_len(mbuf)
        counters.packets += 1
        counters.bytes += pkt_len

        if pcap_dump_enabled:
            write pcap packet header
            write mbuf payload, including multi-segment mbuf

        rte_pktmbuf_free(mbuf)

    每处理完一个 burst 后检查 running
```

停止流程：

1. 控制面设置 `running=false`。
2. `rte_eal_wait_lcore(lcore)` 等待 worker 退出。
3. worker 关闭 pcap 文件。
4. 释放 RX queue、core 占用状态。

性能原则：

- RX worker 不调用 sleep。
- 空包时可以忙轮询；如后续需要降低 CPU 占用，可增加配置项控制是否允许 `rte_pause()`，但默认保持高性能收包。
- pcap dump 是高开销功能。开启后实际 RX 能力会受磁盘写入速度影响，界面需要明确显示 dump 状态和 dump 错误数。

## 8. RX pcap dump 设计

### 8.1 文件命名

每个 RX worker 单独一个文件，避免多线程写同一个 pcap 文件带来的锁竞争。

命名格式：

```text
<rx_pcap_dump_root>/
  stream-<stream_id>_<safe_stream_name>_port-<port_id>_queue-<queue_id>_lcore-<lcore_id>_<timestamp>.pcap
```

示例：

```text
/home/huang/rx_pcap/stream-12_rx-dns_port-0_queue-3_lcore-5_20260521-143012.pcap
```

### 8.2 pcap 格式

使用 classic pcap，链路类型为 Ethernet：

```text
magic_number = 0xa1b2c3d4
version_major = 2
version_minor = 4
thiszone = 0
sigfigs = 0
snaplen = 65535
network = 1
```

每个包写：

```text
ts_sec
ts_usec
incl_len
orig_len
packet bytes
```

时间戳用 `clock_gettime(CLOCK_REALTIME)`。如果后续需要更低开销，可在 worker 中缓存时间或按批次更新时间，但 v1 先保证文件可被 Wireshark/tcpdump 正确读取。

### 8.3 多段 mbuf

不能假设所有包都在单个 mbuf segment 中。写 pcap payload 时需要：

```text
for seg in mbuf chain:
    fwrite(rte_pktmbuf_mtod(seg), rte_pktmbuf_data_len(seg))
```

### 8.4 磁盘保护

建议补充以下保护：

- `rx_pcap_max_file_mb=0` 表示不轮转。
- 大于 0 时，worker 文件达到阈值后按序号新建文件。
- dump 写失败时：
  - `rx_pcap_stop_on_error=true`：停止该 stream，状态变为 `error`。
  - `false`：关闭该 worker 的 dump，继续收包并累计 `dump_errors`。

## 9. TX/RX 统计流程

统计仍保持现有原则：只有 `/api/stats` 触发 TX/RX 吞吐采样。

### 9.1 端口统计

`DpdkContext::snapshot_port_stats()` 调用：

```cpp
rte_eth_stats_get(port_id, &stats)
```

计算：

```text
tx_mbps = (obytes_delta * 8) / elapsed / 1,000,000
tx_pps  = opackets_delta / elapsed

rx_mbps = (ibytes_delta * 8) / elapsed / 1,000,000
rx_pps  = ipackets_delta / elapsed
```

累计量：

```text
tx_total_gb = obytes / 1024 / 1024 / 1024
rx_total_gb = ibytes / 1024 / 1024 / 1024
```

### 9.2 Stream 统计

TX stream：

- 使用 TX worker 成功 `rte_eth_tx_burst()` 后累计的 bytes/packets。
- 展示实际发包 Mbps、pps、累计发送 GB、drop。

RX stream：

- 使用 RX worker 从 `rte_eth_rx_burst()` 收到并释放的 bytes/packets。
- 展示实际收包 Mbps、pps、累计接收 GB、dump 文件数、dump 错误数。

### 9.3 `/api/stats` 响应

建议响应：

```json
{
  "ports": [
    {
      "pci": "0002:01:00.1",
      "port_id": 0,
      "tx_mbps": 950,
      "tx_pps": 1488000,
      "tx_total_gb": 120.5,
      "tx_packets": 180000000,
      "tx_drops": 0,
      "rx_mbps": 930,
      "rx_pps": 1450000,
      "rx_total_gb": 98.2,
      "rx_packets": 150000000,
      "rx_drops": 12,
      "rx_errors": 0,
      "rx_nombuf": 0
    }
  ],
  "streams": [
    {
      "id": 1,
      "direction": "tx",
      "actual_mbps": 950,
      "actual_pps": 1488000,
      "total_gb": 120.5,
      "packets": 180000000,
      "drops": 0
    },
    {
      "id": 2,
      "direction": "rx",
      "actual_mbps": 930,
      "actual_pps": 1450000,
      "total_gb": 98.2,
      "packets": 150000000,
      "drops": 12,
      "errors": 0,
      "dump_files": 4,
      "dump_errors": 0
    }
  ]
}
```

## 10. API 设计

### 10.1 创建 TX stream

保持兼容：

```json
{
  "direction": "tx",
  "name": "tx-1",
  "tx_port": "0002:01:00.1",
  "mode": "construct",
  "core_count": 2,
  "worker_rates_mbps": [1000, 1000],
  "worker_burst_bytes": [2048, 2048]
}
```

如果不传 `direction`，后端默认 `tx`。

### 10.2 创建 RX stream

新增：

```http
POST /api/streams
```

请求：

```json
{
  "direction": "rx",
  "name": "rx-1",
  "rx_port": "0002:01:00.1",
  "core_count": 2,
  "pcap_dump_enabled": true
}
```

响应：

```json
{
  "stream_id": 8,
  "status": "running"
}
```

### 10.3 Stream 列表

```http
GET /api/streams
```

每条 stream 增加：

```json
{
  "direction": "rx",
  "port": "0002:01:00.1",
  "rx_port": "0002:01:00.1",
  "pcap_dump_enabled": true,
  "pcap_dump_dir": "/home/huang/rx_pcap",
  "actual_mbps": 930,
  "actual_pps": 1450000,
  "total_gb": 98.2,
  "packets": 150000000,
  "drops": 12,
  "errors": 0
}
```

### 10.4 历史 stream

查询：

```http
GET /api/history/streams?direction=all
GET /api/history/streams?direction=tx
GET /api/history/streams?direction=rx
```

恢复：

```http
POST /api/history/streams/tx/{id}/restore
POST /api/history/streams/rx/{id}/restore
```

删除：

```http
DELETE /api/history/streams/tx/{id}
DELETE /api/history/streams/rx/{id}
```

也可以保留当前旧接口：

```http
POST /api/history/streams/{id}/restore
DELETE /api/history/streams/{id}
```

但旧接口必须要求 body 或 query 指定 `direction`，否则无法确定查哪张表。推荐前端使用带方向的新接口。

### 10.5 设置接口

可以复用 `/api/runtime`，也可以新增：

```http
GET /api/settings
```

响应只返回可展示配置，敏感字段脱敏：

```json
{
  "system_name": "DPDK TX/RX 控制台",
  "core_list": "0-7",
  "device_list": ["0002:01:00.1"],
  "tx_ring_size": 4096,
  "rx_ring_size": 1024,
  "tx_queue_per_port": 0,
  "rx_queue_per_port": 0,
  "mbuf_pool_size": 262143,
  "mbuf_cache_size": 512,
  "max_burst": 32,
  "rx_pcap_dump_root": "/home/huang/rx_pcap",
  "db_enabled": true,
  "db_endpoint": "127.0.0.1:3306",
  "db_user": "root",
  "db_password_set": true,
  "db_name": "dpdk_tx",
  "db_tx_stream_table": "tx_stream_history",
  "db_rx_stream_table": "rx_stream_history"
}
```

## 11. 数据库设计

用户要求 RX/TX 使用不同表。推荐：

```yaml
db_tx_stream_table: "tx_stream_history"
db_rx_stream_table: "rx_stream_history"
```

### 11.1 TX 表

```sql
CREATE TABLE IF NOT EXISTS tx_stream_history (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    tx_mode VARCHAR(32) NOT NULL,
    rate_mbps INT NOT NULL DEFAULT 0,
    config_json LONGTEXT NOT NULL,
    saved_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### 11.2 RX 表

```sql
CREATE TABLE IF NOT EXISTS rx_stream_history (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    core_count INT NOT NULL DEFAULT 1,
    pcap_dump_enabled TINYINT(1) NOT NULL DEFAULT 0,
    config_json LONGTEXT NOT NULL,
    saved_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

也可以使用完全相同的通用 schema，只靠表名区分方向：

```sql
CREATE TABLE IF NOT EXISTS <table> (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    mode VARCHAR(32) NOT NULL,
    rate_mbps INT NOT NULL DEFAULT 0,
    config_json LONGTEXT NOT NULL,
    saved_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

推荐使用通用 schema，原因是当前 `config_json` 已保存完整恢复配置，变更最小，TX/RX 两张表可以复用同一个 `StreamHistoryStore` 实现。

## 12. 前端界面设计

### 12.1 导航和标题

标题改为：

```text
DPDK TX/RX 控制台
```

导航保持当前结构，主要页面：

- Stream
- 设备
- CPU 核
- PCAP
- 统计
- 历史 stream
- 设置

### 12.2 创建 Stream

在创建表单顶部增加方向选择：

```text
[ TX 发包 ] [ RX 收包 ]
```

TX 选中时：

- 使用现有 TX UI。
- 端口字段显示为“TX 设备”。
- 保留 PCAP/构造包模式、每核速率、每核 Burst B。

RX 选中时：

- 只展示 RX 必要字段，避免和 TX 配置混杂：
  - Stream 名称
  - RX 设备
  - RX 收包线程数
  - pcap_dump 开关
  - dump 保存目录只读展示，来自配置
- 不展示 TX 速率、构造包、端口范围、payload 等配置。

推荐布局：

```text
创建 Stream
┌─────────────────────────────┐
│ 类型  [ TX 发包 ][ RX 收包 ] │
├─────────────────────────────┤
│ 名称        rx-dns           │
│ RX 设备     0002:01:00.1     │
│ RX 线程数   4                │
│ pcap dump   [开关]           │
│ 保存目录    /home/huang/rx_pcap │
└─────────────────────────────┘
```

### 12.3 运行中的 Stream

表格增加方向列：

| 名称 | 类型 | 模式 | 设备 | 核 | 目标 | 实际带宽 | 实际速率 | 累计 | 状态 | 操作 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| tx-1 | TX | construct | 0002:01:00.1 | 2 | 2000 Mbps | 1950 Mbps | 2,900,000 pps | 120 GB | running | 停止 |
| rx-1 | RX | receive | 0002:01:00.1 | 4 | - | 930 Mbps | 1,450,000 pps | 98 GB | running | 停止 |

RX 行额外展示：

- `pcap dump: on/off`
- dump 文件数量
- dump 错误数

### 12.4 统计页

每端口同时展示 TX/RX：

| 设备 | Link | TX Mbps | TX pps | TX GB | RX Mbps | RX pps | RX GB | RX drop | RX error | Reset |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

Reset 语义：

- 单口 reset：清零该端口 TX/RX 采样累计基准。
- 全部 reset：清零所有端口 TX/RX 采样累计基准。
- 不应停止 stream。

### 12.5 历史 Stream

历史页面增加筛选：

```text
[全部] [TX] [RX]
```

表格列：

| 名称 | 类型 | 模式 | 设备 | 核数 | 配置摘要 | 保存时间 | 操作 |
| --- | --- | --- | --- | --- | --- | --- | --- |

TX 配置摘要：

- PCAP 文件或构造包 MAC/IP/端口/payload 长度。
- 每核速率和突发区。

RX 配置摘要：

- RX 设备。
- RX 线程数。
- pcap dump 开关。
- dump 目录。

恢复 RX 历史 stream 时：

- 允许重新选择 RX 设备。
- 允许重新选择 RX 线程数。
- 允许重新选择 pcap dump 开关。
- dump 根目录仍来自配置文件，不在历史恢复弹窗中改路径。

### 12.6 设置页

设置页新增关键只读信息分组。

系统：

- 系统名称
- DPDK 构建状态
- DPDK runtime 状态
- core_list
- main_lcore
- device_list

队列和内存：

- tx_ring_size
- rx_ring_size
- tx_queue_per_port
- rx_queue_per_port
- mbuf_pool_size
- mbuf_cache_size
- max_burst

数据库：

- db_enabled
- db_endpoint
- db_user
- db_password_set
- db_name
- db_tx_stream_table
- db_rx_stream_table

RX pcap dump：

- rx_pcap_dump_root
- rx_pcap_max_file_mb
- rx_pcap_stop_on_error

## 13. 后端实现边界

### 13.1 TX 不变性要求

实现 RX 时必须保持以下 TX 行为不变：

- TX 创建请求兼容当前字段。
- TX PCAP 模式不变。
- TX 构造包模式不变。
- 每核 `worker_rates_mbps` 范围仍是 `100-10000 Mbps`。
- 每核 `worker_burst_bytes` 范围仍是 `2048-16384 bytes`。
- TX worker 仍按 `MAX_BURST` 满 32 个 mbuf 调一次 `rte_eth_tx_burst()`。
- TX 限速策略不被 RX 逻辑修改。
- `/api/stats` 仍是唯一触发 TX/RX 统计采样的接口。

### 13.2 RX 独立性要求

RX 新代码应该只落在：

- `RxStreamEngine`
- `RxWorker`
- `PcapDumpWriter`
- `RxStreamHistory` 或通用 history store 的 RX 表分支
- 前端 RX 表单和展示组件

不允许：

- 在 TX worker 主循环里加入 `if direction == rx`。
- 在 TX 构造包填充里加入 RX 判断。
- 在 TX PCAP 发包路径里加入 RX dump 逻辑。
- 让 RX pcap dump 复用 TX PCAP 读取器的数据结构作为写入状态。

## 14. 开发步骤建议

1. 新增配置字段解析，移除 `default.yaml` 的 `TX_ENGINE_ENABLED`。
2. 新增 `/api/settings` 或扩展 `/api/runtime` 返回设置页字段。
3. 抽出 `DpdkContext`，保持 TX 行为不变并验证 TX 发包。
4. 在 `DpdkContext` 中增加 RX queue 初始化。
5. 新增 `RxStreamEngine`，先实现不 dump 的 RX 收包计数。
6. 扩展 `/api/streams` 支持 `direction=rx`。
7. 扩展 `/api/stats` 返回 RX port stats 和 RX stream stats。
8. 新增 `PcapDumpWriter`，实现每 worker 独立 pcap 文件。
9. 历史库拆分 TX/RX 表，支持 RX 保存、读取、恢复、删除。
10. 前端新增 TX/RX 创建切换、运行 stream 展示、统计页 RX 列、历史筛选、设置页配置展示。
11. 回归验证 TX 创建、TX 发包、TX 统计、TX 历史恢复，确认 RX 改动没有破坏 TX。

## 15. 验证计划

### 15.1 构建

后续只使用 DPDK 构建目录：

```bash
cmake --build back-app/build-dpdk -j
npm --prefix front-app-v2 run build
```

不再使用 `back-app/build`。

### 15.2 TX 回归

- 创建 TX PCAP stream，确认发包正常。
- 创建 TX construct stream，确认发包正常。
- 检查每核速率和每核突发区仍生效。
- `/api/stats` 中 TX Mbps/pps/GB 更新。
- 历史 TX 保存、展示、恢复、删除正常。

### 15.3 RX 功能

- 创建 RX stream，不开启 dump，确认能收包。
- `/api/stats` 中 RX Mbps/pps/GB 更新。
- RX stream 停止后 core 和 RX queue 释放。
- 创建 RX stream，开启 dump，确认每个 worker 生成一个 pcap 文件。
- 用 `tcpdump -r` 或 Wireshark 打开 dump 文件，确认格式正确。
- 多段 mbuf 包能完整写入 pcap。
- dump 目录不可写时创建失败或 stream 进入明确错误状态。

### 15.4 资源冲突

- 同一设备 TX/RX queue 独立占用。
- TX stream 不消耗 RX queue。
- RX stream 不消耗 TX queue。
- 同一 lcore 不允许同时被 TX/RX stream 占用。
- 设备或 CPU 核不可用时，前端仍提示需要重启系统。

## 16. 风险和补充点

1. RX dump 会显著降低收包能力。界面需要展示 dump 开关和 dump 错误数，避免用户误以为纯收包性能异常。
2. 如果磁盘写满，必须有明确处理策略。建议默认 `rx_pcap_stop_on_error=true`，让问题显性化。
3. `rx_queue_per_port` 虽不是用户原始要求，但实际需要。没有它时只能按 worker lcore 数自动预分配，可能无法满足多个 RX stream 同时运行。
4. DPDK port 初始化会从 TX-only 变为 TX/RX 同时配置，这是公共层必要变化。为降低风险，应先抽出 `DpdkContext` 并完成 TX 回归，再接入 RX worker。
5. 历史 stream 使用不同表后，旧 `stream_history` 中已有的 TX 记录需要迁移或保留旧表读取兼容。建议 v1 仍兼容旧表读取 TX 历史，但新保存写入 `tx_stream_history`。
6. 设置页展示数据库密码时不能展示明文，只展示 `db_password_set=true/false`。
7. 运行中 stream 的 `mode` 字段不应再承载 RX/TX 方向。方向使用 `direction`，TX 的 PCAP/construct 使用 `tx_mode` 或继续兼容旧 `mode`。
