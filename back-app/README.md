# DPDK TX/RX C++ 后端

这是 DPDK TX/RX 控制台后端。以 `ENABLE_DPDK=ON` 构建后，后端会初始化 DPDK、启动 TX/RX worker 并用真实 worker 计数和 `rte_eth_stats_get()` 更新前端统计。TX stream 支持 `worker_rates_mbps` 为每个发包线程单独设置 `100-10000 Mbps`，并用 `worker_burst_bytes` 单独设置 `2048-16384 bytes` 突发区；RX stream 支持多核收包和可选 pcap dump。

## 构建

```bash
cmake -S . -B build-dpdk -DENABLE_DPDK=ON
cmake --build build-dpdk -j
```

## 启动

```bash
./build-dpdk/dpdk-tx-control -f config/default.yaml
```

默认监听：

```text
0.0.0.0:8080
```

默认登录密钥见 `config/default.yaml`，当前为：

```text
110110
```

设备来源：

- `device_list` 非空时，设备列表使用该配置。
- `device_list` 为空或未配置时，扫描 `/sys/bus/pci/drivers/vfio-pci`。

PCAP 来源：

- 扫描 `PCAP_ROOT` 指定目录。
- 默认解析 `.pcap` 和 `.pcapng`，不递归。

历史 stream：

- 设置 `DB_ENABLED: true` 后启用 MariaDB/MySQL 历史 stream。
- 退出进程时会保存当前 stream。
- 表不存在时自动创建，数据库本身需要提前创建。

## 接口文档

见 [docs/API.md](docs/API.md)。

## DPDK 接入

真实发包需要系统安装 DPDK。当前 `CMakeLists.txt` 按本机安装路径直接引用：

```text
/usr/local/dpdk/include
/usr/local/dpdk/lib
```

构建方式：

```bash
cmake -S . -B build-dpdk -DENABLE_DPDK=ON
cmake --build build-dpdk -j
```

DPDK 构建会启用 `TxEngine`：

- `rte_eal_init()` 初始化 EAL。
- 按配置启动端口并设置 TX/RX queue。
- PCAP 模式循环发送 classic pcap/pcapng 中的真实包。
- 构造包模式生成 IPv4 UDP/TCP 帧。
- RX 模式使用 `rte_eth_rx_burst()` 收包，可选为每个 RX worker 单独写 pcap 文件。
- 速率控制使用 DPDK TSC 和每核突发区周期闸门。
- 前端统计来自 `rte_eth_stats_get()` 和 worker 收/发字节计数。

如果当前机器没有 DPDK 或未以 `ENABLE_DPDK=ON` 构建，`/api/runtime` 会返回 `tx_engine.ready=false`，前端会显示 DPDK 引擎未就绪告警，创建/启动 stream 会被拒绝。
