# DPDK TX 控制台接口设计

## 1. 认证接口

### `GET /api/auth/challenge`

前端登录前获取 challenge。

响应：

```json
{
  "challenge": "dpdk-tx-static-challenge"
}
```

### `POST /api/auth/login`

前端发送：

```json
{
  "auth_md5": "md5(secret + ':' + challenge)"
}
```

后端使用配置文件中的 `AUTH_TOKEN` 做同样 MD5 计算，匹配后返回会话 token。

响应：

```json
{
  "token": "session-token"
}
```

后续接口使用：

```text
Authorization: Bearer <token>
```

生产环境建议启用 HTTPS，并将 challenge 做成短期有效的一次性随机值，避免固定摘要被重放。

## 2. 运行时接口

### `GET /api/runtime`

提供前端启动态、EAL 参数、main lcore、worker lcore、PCAP 根目录、TX 引擎状态、大页内存状态等。

`tx_engine.ready=false` 时，前端必须提示“不会实际发包”，后端会拒绝创建或启动 stream，并且不会生成模拟吞吐或累计统计。

`hugepages` 字段来自 `/sys/kernel/mm/hugepages` 和 `/proc/meminfo`，用于展示 DPDK 可用大页内存：

- `available`: 是否配置了大页。
- `default_page_size_kb`: 系统默认 hugepage size。
- `hugetlb_mb`: `/proc/meminfo` 中 Hugetlb 总量。
- `total_pages` / `free_pages` / `reserved_pages` / `surplus_pages`: 汇总页数。
- `total_mb` / `free_mb` / `used_mb`: 汇总容量。
- `pools`: 按 page size 展示每个 hugepage 池的 total/free/reserved/surplus 和 MB 容量。

### `GET /api/devices`

提供设备页和 stream 创建页所需的设备资源。

设备来源规则：

- `tx_engine.ready=true` 时，设备列表来自 DPDK 初始化后的 port，link、speed、driver、port id 来自 `rte_eth_*` API。
- `tx_engine.ready=false` 时，设备列表用于配置检查：`device_list` 非空则展示该设备列表；为空或未配置则扫描 `/sys/bus/pci/drivers/vfio-pci`。

关键字段：

- `pci`: PCI 地址，例如 `0002:05:00.0`
- `port_id`: DPDK port id
- `link_up`: 链路是否可用
- `link_speed`: 链路速率
- `total_tx_queues`: 初始化时预分配的 TX queue 数
- `used_tx_queues`: 已被 stream 占用的 TX queue 数
- `available`: 白名单设备当前是否仍可用
- `unavailable_reason`: 不可用原因；前端应提示重启 `dpdk_tx` 系统
- `streams`: 当前占用该设备的 stream 名称

### `GET /api/cores`

提供 CPU 核页和创建 stream 时的可用核判断。

关键字段：

- `id`: lcore id
- `status`: `main`、`used`、`locked`、`idle`、`unavailable`
- `role`: 展示文案
- `stream`: 占用 stream 名称
- `usage_percent`: 1 秒刷新一次的 CPU 使用率
- `available`: CPU 核当前是否可用
- `unavailable_reason`: 不可用原因；前端应提示重启 `dpdk_tx` 系统

### `GET /api/pcap/files`

提供 PCAP 页面和 PCAP 模式创建 stream 的文件列表。

文件来源规则：

- 扫描配置文件 `PCAP_ROOT` 指定的目录。
- `PCAP_RECURSIVE` 控制是否递归扫描。
- `PCAP_EXTENSIONS` 控制允许的扩展名，默认解析 `.pcap` 和 `.pcapng`。
- 后端会读取 pcap 全局头和每包头，统计包数、最大包长、最小包长、平均包长、文件大小。
- 当前实现支持 classic pcap little/big endian，也支持 pcapng 的 Enhanced Packet Block、Simple Packet Block 和旧 Packet Block 摘要解析。

关键字段：

- `packet_count`
- `max_len`
- `min_len`
- `avg_len`
- `size_mb`

## 3. Stream 接口

### `GET /api/streams`

返回 stream 列表。每条 stream 使用 `direction` 区分：

- `direction=tx`: 发包 stream，`mode` 为 `pcap` 或 `construct`。
- `direction=rx`: 收包 stream，`mode` 为 `receive`。

关键速率字段：

- `worker_rates_mbps`: 每个发包 worker 的目标速率，顺序与 `cores`、`queues` 对齐。
- `worker_burst_bytes`: 每个发包 worker 的突发区大小，顺序与 `worker_rates_mbps` 对齐。
- `target_mbps`: `worker_rates_mbps` 的合计值，用于兼容旧 UI 和展示总目标速率。
- `actual_mbps`: stream 实际收/发带宽，由 worker 字节计数计算。
- `actual_pps`: stream 实际收/发包速率。
- `total_gb`: stream 累计收/发数据量。

### `POST /api/streams`

创建 stream。PCAP 模式请求示例：

```json
{
  "direction": "tx",
  "name": "pcap-replay-01",
  "tx_port": "0002:05:00.0",
  "core_count": 2,
  "mode": "pcap",
  "pcap_path": "/data/pcap/http_mix_64_1518.pcap",
  "loop": true,
  "rate_mbps": 5000,
  "worker_rates_mbps": [2000, 3000],
  "worker_burst_bytes": [2048, 8192]
}
```

构造包模式请求示例：

```json
{
  "direction": "tx",
  "name": "udp-load-04",
  "tx_port": "0002:05:00.0",
  "core_count": 2,
  "mode": "construct",
  "rate_mbps": 2500,
  "worker_rates_mbps": [1000, 1500],
  "worker_burst_bytes": [2048, 2048],
  "construct": {
    "l3": "IPv4",
    "l4": "UDP",
    "checksum_enabled": true,
    "src_mac": "02:00:00:00:00:01",
    "dst_mac": "02:00:00:00:00:02",
    "src_ip_addr": "192.168.10.1",
    "src_ip_mask": 24,
    "src_ip_mode": "increment",
    "src_ip_step": 1,
    "dst_ip_addr": "10.10.0.1",
    "dst_ip_mask": 32,
    "dst_ip_mode": "fixed",
    "src_port_start": 10000,
    "src_port_end": 20000,
    "src_port_mode": "increment",
    "src_port_step": 1,
    "dst_port_start": 53,
    "dst_port_end": 53,
    "dst_port_mode": "increment",
    "dst_port_step": 1,
    "payload_len": 256,
    "payload": "48 59 43"
  }
}
```

RX 收包模式请求示例：

```json
{
  "direction": "rx",
  "name": "rx-capture-01",
  "rx_port": "0002:05:00.0",
  "core_count": 2,
  "pcap_dump_enabled": true
}
```

资源规则：

- TX 创建时分配 lcore 和 TX queue。
- RX 创建时分配 lcore 和 RX queue。
- RX stream 不配置目标速率，收包速率由外部流量决定。
- `pcap_dump_enabled=true` 时，每个 RX worker 会在 `rx_pcap_dump_root` 下生成独立 pcap 文件。
- `worker_rates_mbps` 按发包 worker 顺序指定每个 CPU 核的速率，数组长度应等于 `core_count`。
- 单个 worker 速率范围是 `100-10000` Mbps；`rate_mbps` 作为兼容字段和总目标速率展示，后端实际使用 `worker_rates_mbps`。
- `worker_burst_bytes` 按发包 worker 顺序指定每个 CPU 核的突发区大小，范围是 `2048-16384` bytes。
- 构造包 IP 使用 `<prefix>_ip_addr` + `<prefix>_ip_mask` 指定范围，`<prefix>_ip_mode` 支持 `random`、`increment`、`fixed`；固定模式按 `/32` 单地址处理。
- 构造包端口使用 `<prefix>_port_start`、`<prefix>_port_end` 指定范围，`<prefix>_port_mode` 支持 `random`、`increment`；`start=end` 即固定值。
- `checksum_enabled=true` 时使用 DPDK `rte_ipv4_cksum()`、`rte_ipv4_udptcp_cksum()` 计算 IPv4 与 UDP/TCP checksum；为 `false` 时 checksum 字段置 0。
- 发包限速不再使用 token bucket 突发窗口；每个 worker 按自己的突发区大小计算固定周期，突发区扣完后忙等到该周期结束，再恢复突发区额度。
- DPDK 引擎未就绪时，创建 stream 返回 `503`，不会创建“看起来在运行”的假 stream。
- 停止 stream 不释放资源。
- 删除 stream 才释放 lcore 和 TX/RX queue。

### `POST /api/streams/{id}/start`

启动已创建 stream。

### `POST /api/streams/{id}/stop`

停止 stream，core 状态从 `used` 变为 `locked`。

### `DELETE /api/streams/{id}`

删除 stream 并释放资源。

## 4. 统计接口

### `GET /api/stats`

返回每个端口的实时收发速率和累计统计，同时返回 stream 的实时统计增量。

`tx_engine.ready=true` 时，端口速率和累计统计来自 `rte_eth_stats_get()`；TX/RX stream 实际速率来自各自 worker 计数。`tx_engine.ready=false` 时，统计值保持为 0，不生成模拟数据。

关键字段：

- `tx_mbps`: 实时发送速率
- `tx_mpps`: 实时 PPS
- `total_tb`: 累计发送数据量
- `tx_packets_m`: 累计发送包数，单位百万
- `tx_drops`: TX drop
- `rx_mbps`: 实时接收带宽
- `rx_mpps`: 实时 RX PPS
- `rx_total_gb`: 累计接收数据量，单位 GB
- `rx_packets_m`: 累计接收包数，单位百万
- `rx_drops`: RX missed/drop
- `rx_errors`: RX errors
- `rx_nombuf`: RX no mbuf
- `streams[].actual_mbps`: stream 实际收/发带宽。前端运行中 stream 表格的“实际”字段应从该接口同步更新。
- `streams[].actual_pps`: stream 实际收/发 PPS。
- `streams[].total_gb`: stream 累计收/发 GB。

### `POST /api/stats/reset`

请求：

```json
{
  "port_id": 0
}
```

`port_id` 为 `null` 或缺省时重置全部端口统计。

## 5. 刷新接口

### `POST /api/resources/refresh`

重新扫描资源。前端刷新按钮按当前界面传入目标：

```json
{
  "target": "pcap"
}
```

`target` 可选值：

- `devices`: 重新检查 `device_list` 或 vfio-pci 设备列表。
- `cores`: 重新检查 `CORE_LIST` 中 CPU 核是否在线。
- `pcap`: 重新遍历 `PCAP_ROOT`，新增 pcap 文件会出现在 `GET /api/pcap/files`。
- `all`: 全部刷新。

## 6. 历史 Stream 接口

### `GET /api/history/streams?direction=all|tx|rx`

从数据库读取历史 stream。数据库未启用时返回：

```json
{
  "enabled": false,
  "streams": [],
  "error": "database disabled"
}
```

历史记录会返回保存时的完整 `config_json`，以及从中提取的 `config_summary`。TX 历史展示 MAC、源/目的 IP、源/目的端口和 `payload_len`；RX 历史展示 RX 设备、pcap dump 开关等信息。

### `POST /api/history/streams/{direction}/{id}/restore`

恢复历史 stream。恢复时必须重新选择设备和核数：

```json
{
  "tx_port": "0002:05:00.0",
  "core_count": 2,
  "worker_rates_mbps": [1000, 1500],
  "worker_burst_bytes": [2048, 8192]
}
```

后端会读取数据库中保存的 stream 配置，覆盖新的 `tx_port`、`core_count`、`worker_rates_mbps` 和 `worker_burst_bytes` 后重新创建 stream；构造包的 MAC、IP、端口、payload 长度等配置会沿用历史记录。

RX 恢复请求示例：

```json
{
  "rx_port": "0002:05:00.0",
  "core_count": 2,
  "pcap_dump_enabled": true
}
```

### `DELETE /api/history/streams/{direction}/{id}`

删除指定历史 stream 缓存记录。

### 退出保存

进程收到 `SIGINT` 或 `SIGTERM` 后会将当前所有 stream 写入数据库。若表不存在，会自动创建表。

## 7. 配置字段

`config/default.yaml` 已列出当前支持的全部配置项：

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `AUTH_TOKEN` | `110110` | 登录密钥 |
| `HTTP_LISTEN` | `0.0.0.0:8080` | 控制面监听地址 |
| `ENABLE_TLS` | `false` | TLS 开关，当前骨架未启用 TLS socket |
| `TLS_CERT_FILE` | `""` | TLS 证书路径预留 |
| `TLS_KEY_FILE` | `""` | TLS 私钥路径预留 |
| `PCAP_ROOT` | `/home/huang/pcap` | PCAP 文件扫描目录 |
| `PCAP_RECURSIVE` | `false` | 是否递归扫描 PCAP |
| `PCAP_MAX_SCAN_FILES` | `1024` | 最大扫描文件数量 |
| `PCAP_EXTENSIONS` | `[".pcap", ".pcapng"]` | 允许扫描的文件扩展名 |
| `CORE_LIST` | `0-7` | EAL lcore 列表 |
| `MAIN_LCORE` | `0` | 控制面 main lcore |
| `MEM_CHANNELS` | `2` | EAL memory channel 数 |
| `FILE_PREFIX` | `dpdk-tx` | EAL file prefix |
| `PROC_TYPE` | `primary` | EAL proc type |
| `device_list` | `[]` | EAL 设备列表；非空时按列表加载，空或未配置则扫描 vfio-pci |
| `rx_ring_size` | `1024` | RX ring 大小 |
| `tx_ring_size` | `4096` | TX ring 大小 |
| `tx_queue_per_port` | `0` | 每端口 TX queue 数；0 表示按 worker lcore 数 |
| `rx_queue_per_port` | `0` | 每端口 RX queue 数；0 表示按 worker lcore 数 |
| `mbuf_pool_size` | `262143` | mbuf pool 大小 |
| `mbuf_cache_size` | `512` | mbuf cache 大小 |
| `max_burst` | `32` | TX/RX burst 上限 |
| `RATE_ACCOUNTING` | `wire` | 速率口径，`wire` 或 `l2` |
| `stats_interval_ms` | `3000` | 统计刷新周期 |
| `TX_ENGINE_MODE` | `dpdk` | DPDK 引擎类型 |
| `rx_pcap_dump_root` | `/home/huang/rx_pcap` | RX pcap dump 保存目录 |
| `rx_pcap_max_file_mb` | `0` | RX dump 单文件最大大小；0 表示不轮转 |
| `rx_pcap_stop_on_error` | `true` | RX dump 写失败时是否停止 stream |
| `DB_ENABLED` | `true` | 是否启用历史 stream 数据库 |
| `DB_ENDPOINT` | `127.0.0.1:3306` | 数据库地址，格式 `ip:port` |
| `DB_USER` | `root` | 数据库用户名 |
| `DB_PASSWORD` | `""` | 数据库密码 |
| `DB_NAME` | `dpdk_tx` | 数据库名，需要已存在 |
| `db_tx_stream_table` | `tx_stream_history` | TX 历史 stream 表名，不存在时自动创建 |
| `db_rx_stream_table` | `rx_stream_history` | RX 历史 stream 表名，不存在时自动创建 |
| `MOCK_INITIAL_STREAMS` | `false` | demo 开关，默认关闭 |

## 8. DPDK TX/RX 引擎

后端通过 `TxEngine` 接入真实 DPDK 收发包模块。使用真实收发包需要以 DPDK 方式构建：

```bash
cmake -S . -B build-dpdk -DENABLE_DPDK=ON
cmake --build build-dpdk -j
```

已实现的发包路径：

- EAL 初始化：根据 `CORE_LIST`、`MEM_CHANNELS`、`FILE_PREFIX`、`PROC_TYPE`、`device_list` 拼装参数并调用 `rte_eal_init()`。
- 设备初始化：配置 TX/RX queue、启动 port、读取 link/driver/port id。
- PCAP 模式：加载 classic pcap 和 pcapng 包内容，worker 循环发送。
- 构造包模式：生成 IPv4 UDP/TCP 以太帧模板，并按 `checksum_enabled` 决定是否用 DPDK checksum 接口计算。
- 速率控制：worker 内使用 DPDK TSC 和每核突发区周期闸门按 Mbps 限速。
- RX 模式：每个 RX worker 独占一个 RX queue，调用 `rte_eth_rx_burst()` 收包，可选按 worker 单独写 pcap dump。
- 统计：端口统计来自 `rte_eth_stats_get()`，stream 实际速率来自 worker 收/发字节增量。

当前 `CMakeLists.txt` 使用 `/usr/local/dpdk/include` 和 `/usr/local/dpdk/lib` 直接引用 DPDK，并在 `ENABLE_DPDK=ON` 时启用真实 TX/RX 引擎。未启用 DPDK 构建时，`tx_engine.ready=false`，创建/启动 stream 会被拒绝。
