# 构造包发包方式与每核线程工作流程

本文说明 `construct` 模式下后端如何把前端参数转换为可发送的数据包，以及每个 DPDK lcore 发包线程的运行流程。当前实现位于 `back-app/src/TxEngine.cpp`。

## 1. 输入模型

创建 stream 时，前端通过 `POST /api/streams` 下发构造包配置：

```json
{
  "name": "udp-load-01",
  "tx_port": "0002:05:00.0",
  "core_count": 2,
  "mode": "construct",
  "worker_rates_mbps": [1000, 1500],
  "worker_burst_bytes": [2048, 8192],
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

关键字段：

- `core_count`: 当前 stream 要启动几个发包 worker。
- `worker_rates_mbps`: 每个 worker 的目标速率，范围 `100-10000 Mbps`，顺序与后端分配的 `cores`、`queues` 对齐。
- `worker_burst_bytes`: 每个 worker 的突发区大小，范围 `2048-16384 bytes`，顺序与 `worker_rates_mbps` 对齐。
- `tx_port`: DPDK TX port 对应的 PCI/port 名称。
- `mode=construct`: 进入构造包路径。

当前实现约束：

- L3 当前只实际支持 `IPv4`。
- L4 支持 `UDP` 和 `TCP`；不是 `TCP` 时按 `UDP` 处理。
- IP 地址范围由 `ip_addr/ip_mask` 计算，支持 `random`、`increment`、`fixed`。
- 端口范围支持 `random`、`increment`；`start=end` 时等价固定端口。
- `checksum_enabled=true` 时使用 DPDK `rte_ipv4_cksum()`、`rte_ipv4_udptcp_cksum()` 计算 IPv4 与 UDP/TCP checksum；关闭时 checksum 字段置 0。
- 每个 stream 当前生成 1 个 `PacketTemplate`，worker 每包复制模板后按 IP/端口模式改写字段，并按 `checksum_enabled` 处理 checksum。

## 2. Stream 创建阶段

后端 `RuntimeState::create_stream()` 负责控制面校验和资源分配。

流程：

1. 解析 `name`、`tx_port`、`mode`、`core_count`、`worker_rates_mbps`、`worker_burst_bytes`。
2. 校验 TX 引擎是否就绪；未启用 DPDK TX 引擎时直接拒绝创建。
3. 校验 TX 设备可用、link up。
4. 为 stream 分配 `core_count` 个 lcore。
5. 为同一个 TX port 分配同等数量的 TX queue。
6. 将 `worker_rates_mbps` 规范化到 `100-10000 Mbps`，并计算 `target_mbps=sum(worker_rates_mbps)`。
7. 将 `worker_burst_bytes` 规范化到 `2048-16384 bytes`。
8. 构造 `StreamInfo`，调用 `TxEngine::start_stream()`。

资源映射关系：

```text
stream
  core[0]  -> tx queue[0] -> worker_rates_mbps[0] + worker_burst_bytes[0]
  core[1]  -> tx queue[1] -> worker_rates_mbps[1] + worker_burst_bytes[1]
  ...
```

每个 worker 独占一个 lcore 和一个 TX queue，避免多个线程同时写同一个 queue。

## 3. 构造 PacketTemplate

`DpdkTxEngine::start_stream()` 发现 `stream.mode != "pcap"` 时，会调用 `build_construct_templates()` 生成构造包模板。

当前模板生成流程：

1. 读取 `l3` 和 `l4`。
2. 校验 `l3 == IPv4`。
3. 解析源/目的 MAC。
4. 解析源/目的 IPv4 地址、掩码、模式和步长。
5. 根据 `l4` 决定 L4 header 长度：
   - UDP: `rte_udp_hdr`
   - TCP: `rte_tcp_hdr`
6. 读取 `payload_len`，并保证最终帧长至少达到以太网最小帧长度 60 bytes。
7. 解析 `payload`：
   - 如果是空格分隔十六进制字节，按 hex 转为 bytes。
   - 如果解析失败但字符串非空，则按普通文本 bytes 使用。
8. 分配一段连续 `std::vector<uint8_t>` 作为完整 L2 帧。
9. 填充 Ethernet header。
10. 填充 IPv4 header。
11. 填充 UDP 或 TCP header。
12. 写入 payload。
13. 如果启用 checksum，则用 DPDK 接口计算 IPv4 header checksum。
14. 记录 IP/端口动态改写规则。
15. 如果启用 checksum，则用 DPDK 接口计算 UDP/TCP checksum；关闭时置 0。
16. 包装为 `PacketTemplate`。

`PacketTemplate` 内保存：

```cpp
struct PacketTemplate {
    std::vector<uint8_t> bytes;
    uint32_t l2_len;
    uint64_t wire_bits;
};
```

其中 `wire_bits` 用于限速。配置 `RATE_ACCOUNTING=wire` 时按 `L2 长度 + 20 bytes` 计算，20 bytes 对应 preamble 和 IFG；否则只按 L2 长度计算。

## 4. Worker 创建与启动

每个 stream 会创建一个 `StreamContext`，其中包含共享的包模板和多个 `WorkerContext`。

每个 `WorkerContext` 保存：

- `stream_id`
- `lcore_id`
- `port_id`
- `queue_id`
- `rate_bps`
- `max_burst`
- `burst_bytes`
- `mempool`
- 共享的 `PacketTemplate` 列表
- `running` 状态
- packets/bytes/drops/no_mbuf 计数器

启动时，`launch_stream_locked()` 会将 `worker_rates_mbps[i]` 转为：

```text
worker.rate_bps = worker_rates_mbps[i] * 1000 * 1000
worker.burst_bytes = worker_burst_bytes[i]
```

然后调用：

```cpp
rte_eal_remote_launch(dpdk_worker_main, worker.get(), worker->lcore_id)
```

也就是说，一个 stream 如果选择了 N 个 CPU 核，就会启动 N 个 DPDK remote worker；这些 worker 做相同的发包工作，但速率、lcore、queue 各自独立。

## 5. 每个核线程的工作流程

每个 lcore 上运行 `dpdk_worker_main()`。

总体循环：

```text
worker start
  初始化 BurstPacer
  根据 worker.rate_bps 和 worker.burst_bytes 计算 cycles_per_burst
  cycle_stamp = rte_get_tsc_cycles()
  分配 burst 临时数组
  while running:
    构造最多 MAX_BURST 个 mbuf:
      取下一个 PacketTemplate
      从 mempool 申请 mbuf
      append packet 长度
      memcpy 模板 bytes 到 mbuf data
      按 IP/端口模式改写 mbuf 中的 IPv4 和 L4 字段
      启用 checksum 时重算 IPv4 与 UDP/TCP checksum
      设置 mbuf->port
      放入 burst 数组
      burst_remaining -= packet.wire_bits
      如果 burst_remaining <= 0:
        满负载循环检查 rte_get_tsc_cycles() - cycle_stamp
        到达 cycles_per_burst 后更新 cycle_stamp
        恢复 burst_remaining

    如果本轮没有 mbuf:
      continue

    rte_eth_tx_burst(port_id, queue_id, bufs, count)
    统计已发送 packets/bytes
    释放未成功发送的 mbuf
    统计 drops
    检查一次 running，决定是否进入下一组 MAX_BURST
```

### 5.1 限速

每个 worker 独立持有一个 `BurstPacer`。限速基于 DPDK TSC：

- `rte_get_tsc_hz()` 获取 TSC 频率。
- `rte_get_tsc_cycles()` 获取当前周期。
- `worker_burst_bytes` 转为 `burst_bits = worker_burst_bytes * 8`。
- `cycles_per_burst = burst_bits * tsc_hz / rate_bps`。
- `cycle_stamp` 保存当前突发区周期的开始 TSC。
- 每填充一个 mbuf 后，根据 `packet.wire_bits` 扣减当前突发区剩余额度。

等待策略：

- 发包线程不调用 `sleep` 或 `rte_delay_us_sleep()`。
- 如果突发区剩余额度扣完，worker 满负荷循环读取 `rte_get_tsc_cycles()`。
- 当 `rte_get_tsc_cycles() - cycle_stamp >= cycles_per_burst` 时，更新 `cycle_stamp` 为当前 TSC，并恢复突发区初始大小。
- 忙等期间不检查 `running`，减少发包热路径中的原子读取开销。
- worker 只在每组 `MAX_BURST` 提交后检查一次 `running`；停止 stream 后可能多发当前这一组包。
- `rte_eth_tx_burst()` 与突发区耗尽无关，worker 仍按 `MAX_BURST` 攒满一批 mbuf 后提交一次。

这样每个 CPU 核的发包速率互不影响，stream 总速率等于所有 worker 实际速率之和。

### 5.2 mbuf 填充

当前构造包模式不是从零生成每个 header，而是：

1. stream 启动前生成完整 L2 packet bytes 模板。
2. worker 每发一个包，从 mempool 申请一个 mbuf。
3. 将模板 bytes 拷贝到 mbuf。
4. 根据本 worker 的本地游标选择源/目的 IP、源/目的端口。
5. 改写 mbuf 中 IPv4 header 和 UDP/TCP header。
6. 启用 checksum 时用 DPDK 接口重算 IPv4 与 UDP/TCP checksum，关闭时置 0。
7. 调用 `rte_eth_tx_burst()` 发送。

这种方式避免每包重新拼完整帧，同时支持 IP/端口范围递增和随机发包。

### 5.3 burst 发送

worker 每轮最多准备 `MAX_BURST` 个 mbuf，然后调用一次：

```cpp
rte_eth_tx_burst(worker->port_id, worker->queue_id, bufs.data(), count)
```

返回值 `sent` 表示网卡实际接收的 mbuf 数量：

- `sent == count`: 全部提交给 TX queue。
- `sent < count`: 剩余 mbuf 必须释放，并计入 drop。

当前 drop 语义是“没有提交到 TX queue 的 mbuf 数”，通常代表 TX queue/backpressure 或网卡发送能力不足。

## 6. 停止与删除

停止 stream：

1. 控制面调用 `TxEngine::stop_stream()`。
2. 后端将每个 worker 的 `running=false`。
3. 对每个 lcore 调用 `rte_eal_wait_lcore()` 等待线程退出。
4. stream 进入 stopped，core/queue 仍保留。

删除 stream：

1. 先执行 stop。
2. 删除 `StreamContext`。
3. 控制面释放 lcore 和 TX queue 资源。

## 7. 统计来源

统计分两层：

- 端口统计来自 `rte_eth_stats_get()`：
  - `opackets`
  - `obytes`
  - `oerrors`
- stream 实际速率来自 worker 内部累计发送 bytes 的差值。

前端展示：

- `target_mbps`: 每核目标速率之和。
- `worker_rates_mbps`: 每核目标速率。
- `worker_burst_bytes`: 每核突发区大小。
- `actual_mbps`: stream 实际发送速率。
- 每个端口的 `tx_mbps`、`pps`、累计发送、drop。

## 8. 后续扩展建议

当前构造包路径已经具备基本发包能力，但还有几个明确扩展点：

- IPv6 构造包支持。
- TX checksum offload。
- 多 PacketTemplate 支持，例如按参数生成一组模板循环发送。
- payload pattern：固定、递增、随机、时间戳。
- 使用 `rte_pktmbuf_alloc_bulk()` 批量申请 mbuf，降低 per-packet 开销。
- 对重复模板评估 external buffer 或 indirect mbuf，减少 memcpy。
