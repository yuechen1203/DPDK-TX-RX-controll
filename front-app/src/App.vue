<template>
  <section v-if="!authenticated" class="login-screen">
    <div class="login-panel">
      <div class="login-head">
        <div class="brand-mark"><KeyRound :size="20" /></div>
        <div>
          <h1>DPDK TX/RX 登录</h1>
          <p>输入后端配置文件中的控制密钥</p>
        </div>
      </div>

      <label class="field">
        <span>密钥</span>
        <input v-model="loginSecret" type="password" autocomplete="current-password" @keydown.enter="login">
      </label>

      <div class="digest-box">
        <span>请求摘要</span>
        <strong>{{ loginDigest || '等待输入' }}</strong>
      </div>

      <button class="btn primary" type="button" @click="login">
        <LogIn :size="16" />登录
      </button>

      <p v-if="loginError" class="error-text">{{ loginError }}</p>
    </div>
  </section>

  <div v-else class="app-shell">
    <aside class="sidebar">
      <div class="brand">
        <div class="brand-mark"><RadioTower :size="20" /></div>
        <div>
          <strong>{{ runtime.system_name || 'DPDK TX/RX 控制台' }}</strong>
          <span>{{ runtime.version || 'dpdk-tx' }} / {{ runtime.eal?.proc_type || 'primary' }}</span>
        </div>
      </div>

      <nav class="nav">
        <button
          v-for="item in navItems"
          :key="item.key"
          :class="{ active: currentPage === item.key }"
          type="button"
          @click="switchPage(item.key)"
        >
          <component :is="item.icon" :size="17" />{{ item.label }}
        </button>
      </nav>

      <div class="side-panel">
        <h3>后端状态</h3>
        <div class="side-row">
          <span>连接</span>
          <span :class="['badge', backendOnline ? 'ok' : 'danger']"><i></i>{{ backendOnline ? '在线' : '中断' }}</span>
        </div>
        <button v-if="!backendOnline" class="btn danger full-btn" type="button" :disabled="reconnecting" @click="reconnectBackend">
          <RefreshCw :size="16" />{{ reconnecting ? 'Reconnecting' : 'Reconnect' }}
        </button>
        <div class="side-row">
          <span>DPDK 引擎</span>
          <span :class="['badge', txEngineReady ? 'ok' : 'warn']"><i></i>{{ txEngineReady ? '就绪' : '未接入' }}</span>
        </div>
        <div class="side-row">
          <span>大页</span>
          <span :class="['badge', hugepageBadge.kind]"><i></i>{{ hugepageBadge.text }}</span>
        </div>
        <div class="side-row"><span>Huge free</span><strong>{{ formatNumber(hugepages.free_mb, 1) }} / {{ formatNumber(hugepages.total_mb, 1) }} MB</strong></div>
        <div class="side-row"><span>监听</span><strong>{{ runtime.listen }}</strong></div>
        <div class="side-row"><span>Main lcore</span><strong>{{ runtime.eal?.main_lcore }}</strong></div>
        <div class="side-row"><span>运行时间</span><strong>{{ runtime.uptime }}</strong></div>
      </div>

      <div class="side-panel">
        <h3>EAL</h3>
        <div class="side-row"><span>Core list</span><strong>{{ runtime.eal?.core_list }}</strong></div>
        <div class="side-row"><span>File prefix</span><strong>{{ runtime.eal?.file_prefix }}</strong></div>
        <div class="side-row"><span>Proc type</span><strong>{{ runtime.eal?.proc_type }}</strong></div>
      </div>
    </aside>

    <main class="workspace">
      <header class="topbar">
        <h1>{{ activeTitle }}</h1>
        <div class="topbar-actions">
          <button class="btn outline" type="button" @click="refreshCurrent"><RefreshCw :size="16" />刷新</button>
          <button v-if="currentPage === 'streams'" class="btn warn" type="button" @click="stopAllStreams"><Square :size="16" />停止全部</button>
          <button v-if="currentPage === 'streams'" class="btn primary" type="button" :disabled="!txEngineReady" @click="focusCreate"><Plus :size="16" />创建 Stream</button>
          <button class="btn" type="button" @click="logout"><LogOut :size="16" />退出</button>
        </div>
      </header>

      <section class="content">
        <div v-if="!backendOnline" class="alert danger connection-alert">
          <strong>后端连接已中断，请重启后端服务。</strong>
          <button class="btn danger" type="button" :disabled="reconnecting" @click="reconnectBackend"><RefreshCw :size="16" />{{ reconnecting ? 'Reconnecting' : 'Reconnect' }}</button>
        </div>
        <div v-if="backendOnline && runtime.tx_engine && !txEngineReady" class="alert warn">
          <strong>DPDK 引擎未就绪，当前不会实际收发包。</strong>
          <div>{{ txEngineMessage }}</div>
        </div>
        <div v-if="error" class="alert danger">{{ error }}</div>
        <div v-if="refreshMessage" class="alert ok">{{ refreshMessage }}</div>
        <div v-if="unavailableMessages.length" class="alert danger">
          <strong>检测到资源不可用，需重启 dpdk_tx 系统。</strong>
          <div v-for="message in unavailableMessages" :key="message">{{ message }}</div>
        </div>

        <template v-if="currentPage === 'streams'">
          <section class="summary-grid">
            <MetricCard label="TX 吞吐" :value="`${formatNumber(totalRate)} Mbps`" sub="所有端口发送汇总" :icon="RadioReceiver" />
            <MetricCard label="RX 吞吐" :value="`${formatNumber(totalRxRate)} Mbps`" sub="所有端口接收汇总" :icon="RadioReceiver" />
            <MetricCard label="TX/RX PPS" :value="`${formatNumber(totalPps)} / ${formatNumber(totalRxPps)}`" sub="按 3s 窗口计算" :icon="Waves" />
            <MetricCard label="Worker 核" :value="`${usedWorkerCores} / ${workerCores}`" sub="main core 已排除" :icon="Cpu" />
          </section>

          <section class="layout">
            <div class="panel">
              <div class="panel-head">
                <h2>运行中的 Stream</h2>
                <span class="badge ok"><i></i>{{ runningStreams }} running</span>
              </div>
              <div class="panel-body">
                <div>
                  <div class="section-title">
                    <h2>收发设备</h2>
                    <span class="badge idle">EAL 白名单 / TX-RX 共用</span>
                  </div>
                  <div class="device-list">
                    <article v-for="device in devices" :key="device.pci" class="device">
                      <div class="device-top">
                        <div>
                          <div class="device-pci">{{ device.pci }}</div>
                          <span class="muted">port {{ device.port_id }} / socket {{ device.socket_id }}</span>
                        </div>
                        <span :class="['badge', deviceBadge(device).kind]"><i></i>{{ deviceBadge(device).text }}</span>
                      </div>
                      <div class="device-meta">
                        <span>Speed<strong>{{ device.link_speed }}</strong></span>
                          <span>TX queue<strong>{{ device.used_tx_queues }} / {{ device.total_tx_queues }}</strong></span>
                          <span>RX queue<strong>{{ device.used_rx_queues }} / {{ device.total_rx_queues }}</strong></span>
                        <span>Driver<strong>{{ device.driver }}</strong></span>
                      </div>
                    </article>
                  </div>
                </div>

                <div>
                  <div class="section-title">
                    <h2>CPU 核分配</h2>
                    <div class="legend">
                      <span><b class="main-key"></b>Main</span>
                      <span><b class="used-key"></b>运行</span>
                      <span><b class="locked-key"></b>保留</span>
                      <span><b></b>空闲</span>
                    </div>
                  </div>
                  <div class="core-grid">
                    <div v-for="core in cores" :key="core.id" :class="['core', core.status]">{{ core.id }}</div>
                  </div>
                </div>

                <div class="stream-card-list">
                  <article v-for="stream in streams" :key="stream.id" class="stream-card">
                    <div class="stream-card-head">
                      <div class="stream-title-block">
                        <strong>{{ stream.name }}</strong>
                        <span>{{ streamDirection(stream) === 'rx' ? stream.rx_port : stream.tx_port }}</span>
                      </div>
                      <div class="stream-badges">
                        <span :class="['badge', streamDirection(stream) === 'rx' ? 'idle' : 'ok']"><i></i>{{ streamDirection(stream).toUpperCase() }}</span>
                        <span :class="['badge', stream.status === 'running' ? 'ok' : 'warn']"><i></i>{{ stream.status }}</span>
                        <span class="badge idle"><i></i>{{ streamModeText(stream) }}</span>
                      </div>
                      <div class="stream-card-actions">
                        <button class="btn outline" type="button" :title="streamTooltip(stream)" @click="openStreamDetails(stream)"><Info :size="15" />详情</button>
                        <button v-if="stream.status !== 'running'" class="btn primary" type="button" :disabled="!txEngineReady" @click="startStream(stream.id)"><Play :size="15" />开始</button>
                        <button v-else class="btn warn" type="button" @click="stopStream(stream.id)"><Square :size="15" />停止</button>
                        <button class="btn danger" type="button" @click="deleteStream(stream.id)"><Trash2 :size="15" />删除</button>
                      </div>
                    </div>

                    <div class="stream-stat-grid">
                      <div class="stream-stat">
                        <span>目标总计</span>
                        <strong>{{ streamDirection(stream) === 'rx' ? '-' : `${formatNumber(streamTargetTotal(stream))} Mbps` }}</strong>
                      </div>
                      <div class="stream-stat">
                        <span>实际带宽</span>
                        <strong>{{ formatNumber(stream.actual_mbps) }} Mbps</strong>
                      </div>
                      <div class="stream-stat">
                        <span>实际速率</span>
                        <strong>{{ formatNumber(stream.actual_pps) }} pps</strong>
                      </div>
                      <div class="stream-stat">
                        <span>累计</span>
                        <strong>{{ formatNumber(stream.total_gb, 2) }} GB</strong>
                      </div>
                    </div>

                    <div class="stream-resource-grid">
                      <section class="stream-resource-block">
                        <h3>Core</h3>
                        <div class="resource-pill-list">
                          <span v-for="item in coreItems(stream)" :key="item.key" class="resource-pill">
                            <strong>{{ item.label }}</strong>
                            <em>{{ item.sub }}</em>
                          </span>
                        </div>
                      </section>
                      <section class="stream-resource-block">
                        <h3>每核目标</h3>
                        <div v-if="streamDirection(stream) !== 'rx'" class="resource-pill-list">
                          <span v-for="item in workerRateItems(stream)" :key="item.key" class="resource-pill">
                            <strong>{{ item.label }}</strong>
                            <em>{{ item.value }}</em>
                          </span>
                        </div>
                        <span v-else class="muted">RX stream 无发包目标</span>
                      </section>
                      <section class="stream-resource-block">
                        <h3>突发区</h3>
                        <div v-if="streamDirection(stream) !== 'rx'" class="resource-pill-list">
                          <span v-for="item in workerBurstItems(stream)" :key="item.key" class="resource-pill">
                            <strong>{{ item.label }}</strong>
                            <em>{{ item.value }}</em>
                          </span>
                        </div>
                        <span v-else class="muted">-</span>
                      </section>
                    </div>
                  </article>

                  <div v-if="!streams.length" class="stream-empty">
                    暂无运行中的 stream
                  </div>
                </div>
              </div>
            </div>

            <aside class="panel" ref="createPanel">
              <div class="panel-head">
                <h2>创建 Stream</h2>
                <span class="badge idle">调用 /api/streams</span>
              </div>
              <form class="panel-body" @submit.prevent="createStream">
                <div class="form-grid">
                  <label class="field full"><span>Stream 名称</span><input v-model="newStream.name"></label>
                  <div class="field full">
                    <span>Stream 类型</span>
                    <div class="segmented">
                      <button :class="{ active: newStream.direction === 'tx' }" type="button" @click="newStream.direction = 'tx'">TX 发包</button>
                      <button :class="{ active: newStream.direction === 'rx' }" type="button" @click="newStream.direction = 'rx'">RX 收包</button>
                    </div>
                  </div>
                  <label v-if="newStream.direction === 'tx'" class="field">
                    <span>TX 设备</span>
                    <select v-model="newStream.tx_port">
                      <option v-for="device in usableDevices" :key="device.pci" :value="device.pci">{{ device.pci }} / port {{ device.port_id }}</option>
                    </select>
                  </label>
                  <label v-else class="field">
                    <span>RX 设备</span>
                    <select v-model="newStream.rx_port">
                      <option v-for="device in usableDevices" :key="device.pci" :value="device.pci">{{ device.pci }} / port {{ device.port_id }}</option>
                    </select>
                  </label>
                  <label class="field">
                    <span>{{ newStream.direction === 'rx' ? '收包核数' : '发包核数' }}</span>
                    <select v-model.number="newStream.core_count">
                      <option v-for="count in [1, 2, 3, 4]" :key="count" :value="count" :disabled="count > freeCoreCount">{{ count }} core{{ count > freeCoreCount ? ' / 不足' : '' }}</option>
                    </select>
                  </label>

                  <div v-if="newStream.direction === 'rx'" class="mode-panel full">
                    <label class="check-row"><input v-model="newStream.pcap_dump_enabled" type="checkbox">pcap_dump 保存收包</label>
                    <div class="field-note">保存目录 {{ runtime.settings?.rx_pcap_dump_root || runtime.rx_pcap_dump_root || '-' }}；每个 RX worker 单独一个 pcap 文件</div>
                  </div>

                  <template v-if="newStream.direction === 'tx'">
                  <div class="field full">
                    <span>发包模式</span>
                    <div class="segmented">
                      <button :class="{ active: newStream.mode === 'pcap' }" type="button" @click="newStream.mode = 'pcap'">PCAP</button>
                      <button :class="{ active: newStream.mode === 'construct' }" type="button" @click="newStream.mode = 'construct'">构造包</button>
                    </div>
                  </div>

                  <div v-if="newStream.mode === 'pcap'" class="mode-panel full">
                    <label class="field full">
                      <span>PCAP 文件</span>
                      <select v-model="newStream.pcap_path">
                        <option v-for="file in pcapFiles" :key="file.path" :value="file.path">{{ file.name }}</option>
                      </select>
                    </label>
                    <label class="check-row"><input v-model="newStream.loop" type="checkbox">循环发送</label>
                  </div>

                  <div v-else class="mode-panel full construct-panel">
                    <div class="construct-layout">
                      <div class="construct-block">
                        <div class="construct-title">协议与负载</div>
                        <div class="protocol-grid">
                          <label class="field"><span>IP 类型</span><select v-model="newStream.construct.l3"><option>IPv4</option><option>IPv6</option></select></label>
                          <label class="field"><span>传输层</span><select v-model="newStream.construct.l4"><option>UDP</option><option>TCP</option></select></label>
                          <label class="field"><span>Payload 长度</span><input v-model.number="newStream.construct.payload_len" inputmode="numeric"></label>
                          <label class="check-row checksum-row"><input v-model="newStream.construct.checksum_enabled" type="checkbox">计算 checksum</label>
                        </div>
                      </div>

                      <div class="construct-block">
                        <div class="construct-title">二层 MAC</div>
                        <div class="mac-config-grid">
                          <label class="field"><span>源 MAC</span><input v-model="newStream.construct.src_mac"></label>
                          <label class="field"><span>目的 MAC</span><input v-model="newStream.construct.dst_mac"></label>
                        </div>
                      </div>

                      <div class="construct-block">
                        <div class="construct-title">三层 IP 地址</div>
                        <div class="address-config-list">
                          <div class="construct-subblock ip-pair-block">
                            <div class="ip-config-row">
                              <div class="construct-subtitle">源 IP 范围</div>
                              <div class="ip-config-grid">
                                <label class="field ip-address-field"><span>地址</span><input v-model="newStream.construct.src_ip_addr" inputmode="decimal"></label>
                                <label class="field mask-field"><span>Mask</span><input v-model.number="newStream.construct.src_ip_mask" type="number" min="0" max="32" :disabled="newStream.construct.src_ip_mode === 'fixed'" @change="clampIpMask('src')"></label>
                                <label class="field mode-field"><span>发包模式</span><select v-model="newStream.construct.src_ip_mode" @change="setIpMode('src', newStream.construct.src_ip_mode)"><option value="random">随机</option><option value="increment">递增</option><option value="fixed">固定</option></select></label>
                                <label v-if="newStream.construct.src_ip_mode === 'increment'" class="field step-field"><span>递增步长</span><input v-model.number="newStream.construct.src_ip_step" type="number" min="1" inputmode="numeric" @change="clampIpStep('src')"></label>
                              </div>
                            </div>
                            <div class="ip-config-row">
                              <div class="construct-subtitle">目的 IP 范围</div>
                              <div class="ip-config-grid">
                                <label class="field ip-address-field"><span>地址</span><input v-model="newStream.construct.dst_ip_addr" inputmode="decimal"></label>
                                <label class="field mask-field"><span>Mask</span><input v-model.number="newStream.construct.dst_ip_mask" type="number" min="0" max="32" :disabled="newStream.construct.dst_ip_mode === 'fixed'" @change="clampIpMask('dst')"></label>
                                <label class="field mode-field"><span>发包模式</span><select v-model="newStream.construct.dst_ip_mode" @change="setIpMode('dst', newStream.construct.dst_ip_mode)"><option value="random">随机</option><option value="increment">递增</option><option value="fixed">固定</option></select></label>
                                <label v-if="newStream.construct.dst_ip_mode === 'increment'" class="field step-field"><span>递增步长</span><input v-model.number="newStream.construct.dst_ip_step" type="number" min="1" inputmode="numeric" @change="clampIpStep('dst')"></label>
                              </div>
                            </div>
                          </div>
                        </div>
                      </div>

                      <div class="construct-block">
                        <div class="construct-title">四层端口</div>
                        <div class="port-config-list">
                          <div class="construct-subblock port-pair-block">
                            <div class="port-config-row">
                              <div class="construct-subtitle">源端口范围</div>
                              <div class="port-config-grid">
                                <div class="field port-range-field">
                                  <span>起始 - 结束</span>
                                  <div class="port-range"><input v-model.number="newStream.construct.src_port_start" inputmode="numeric" @change="clampPortRange('src')"><span>-</span><input v-model.number="newStream.construct.src_port_end" inputmode="numeric" @change="clampPortRange('src')"></div>
                                </div>
                                <label class="field mode-field"><span>发包模式</span><select v-model="newStream.construct.src_port_mode"><option value="random">随机</option><option value="increment">递增</option></select></label>
                                <label v-if="newStream.construct.src_port_mode === 'increment'" class="field step-field"><span>递增步长</span><input v-model.number="newStream.construct.src_port_step" type="number" min="1" inputmode="numeric" @change="clampPortStep('src')"></label>
                              </div>
                            </div>
                            <div class="port-config-row">
                              <div class="construct-subtitle">目的端口范围</div>
                              <div class="port-config-grid">
                                <div class="field port-range-field">
                                  <span>起始 - 结束</span>
                                  <div class="port-range"><input v-model.number="newStream.construct.dst_port_start" inputmode="numeric" @change="clampPortRange('dst')"><span>-</span><input v-model.number="newStream.construct.dst_port_end" inputmode="numeric" @change="clampPortRange('dst')"></div>
                                </div>
                                <label class="field mode-field"><span>发包模式</span><select v-model="newStream.construct.dst_port_mode"><option value="random">随机</option><option value="increment">递增</option></select></label>
                                <label v-if="newStream.construct.dst_port_mode === 'increment'" class="field step-field"><span>递增步长</span><input v-model.number="newStream.construct.dst_port_step" type="number" min="1" inputmode="numeric" @change="clampPortStep('dst')"></label>
                              </div>
                            </div>
                          </div>
                        </div>
                      </div>

                      <div class="construct-block">
                        <div class="construct-title">Payload 内容</div>
                        <label class="field"><span>Hex 或文本</span><textarea v-model="newStream.construct.payload"></textarea></label>
                      </div>
                    </div>
                  </div>

                  <div class="field full">
                    <span>每核发包参数</span>
                    <div class="worker-rate-list">
                      <div v-for="(_, index) in newStream.worker_rates_mbps" :key="index" class="worker-rate-row">
                        <span>线程 {{ index + 1 }}</span>
                        <input v-model.number="newStream.worker_rates_mbps[index]" type="range" min="100" max="10000" @input="clampWorkerRate(index)">
                        <label class="inline-number"><span>Mbps</span><input v-model.number="newStream.worker_rates_mbps[index]" type="number" min="100" max="10000" inputmode="numeric" @change="clampWorkerRate(index)"></label>
                        <label class="inline-number"><span>Burst B</span><input v-model.number="newStream.worker_burst_bytes[index]" type="number" min="2048" max="16384" step="1024" inputmode="numeric" @change="clampWorkerBurst(index)"></label>
                      </div>
                    </div>
                    <div class="field-note">总目标 {{ formatNumber(configuredTotalRate) }} Mbps；突发区范围 2048-16384 B</div>
                  </div>
                  </template>
                </div>

                <div class="footer-actions">
                  <button class="btn primary" type="submit" :disabled="!txEngineReady"><Play :size="16" />{{ txEngineReady ? '创建并启动' : 'DPDK 引擎未就绪' }}</button>
                </div>
              </form>
            </aside>
          </section>
        </template>

        <template v-else-if="currentPage === 'devices'">
          <section class="summary-grid">
            <MetricCard label="EAL 设备" :value="String(devices.length)" :sub="`${linkUpDevices} 个 link up`" :icon="Server" />
            <MetricCard label="已占用" :value="String(usedDevices)" sub="已有 stream 绑定" :icon="LockKeyhole" />
            <MetricCard label="空闲" :value="String(idleDevices)" sub="可创建新 stream" :icon="Circle" />
            <MetricCard label="TX queue" :value="`${usedQueues} / ${totalQueues}`" sub="按 worker core 预分配" :icon="GitBranch" />
            <MetricCard label="RX queue" :value="`${usedRxQueues} / ${totalRxQueues}`" sub="按 worker core 预分配" :icon="GitBranch" />
          </section>
          <section class="resource-grid">
            <article v-for="device in devices" :key="device.pci" class="device">
              <div class="device-top">
                <div>
                  <div class="device-pci">{{ device.pci }}</div>
                  <span class="muted">port {{ device.port_id }} / socket {{ device.socket_id }} / {{ device.driver }}</span>
                </div>
                <span :class="['badge', deviceBadge(device).kind]"><i></i>{{ deviceBadge(device).text }}</span>
              </div>
              <div class="detail-list">
                <div class="detail-row"><span>链路</span><strong>{{ device.link_up ? `${device.link_speed} up` : 'down' }}</strong></div>
                <div v-if="!device.available" class="detail-row"><span>不可用原因</span><strong>{{ device.unavailable_reason || '需重启 dpdk_tx 系统' }}</strong></div>
                <div class="detail-row"><span>TX stream</span><strong>{{ device.tx_streams?.length ? device.tx_streams.join(', ') : '无' }}</strong></div>
                <div class="detail-row"><span>RX stream</span><strong>{{ device.rx_streams?.length ? device.rx_streams.join(', ') : '无' }}</strong></div>
                <div class="detail-row"><span>TX queue</span><strong>{{ device.used_tx_queues }} / {{ device.total_tx_queues }}</strong></div>
                <div class="detail-row"><span>RX queue</span><strong>{{ device.used_rx_queues }} / {{ device.total_rx_queues }}</strong></div>
                <div class="progress" :class="{ warn: queuePercent(device) > 70, danger: queuePercent(device) > 90 }"><span :style="{ width: `${queuePercent(device)}%` }"></span></div>
              </div>
            </article>
          </section>
        </template>

        <template v-else-if="currentPage === 'cores'">
          <section class="summary-grid">
            <MetricCard label="EAL lcore" :value="runtime.eal?.core_list || '-'" sub="main lcore 不参与发包" :icon="Cpu" />
            <MetricCard label="运行占用" :value="String(runningCoreCount)" sub="stream 正在使用" :icon="Play" />
            <MetricCard label="保留占用" :value="String(lockedCoreCount)" sub="停止后未删除的 stream" :icon="Pause" />
            <MetricCard label="刷新周期" value="3s" sub="由后端统计返回" :icon="TimerReset" />
          </section>
          <section class="panel">
            <div class="panel-head">
              <h2>CPU 使用率</h2>
              <span class="badge idle">3s 刷新</span>
            </div>
            <div class="panel-body resource-grid">
              <article v-for="core in cores" :key="core.id" class="cpu-card">
                <div class="cpu-card-top">
                  <strong>lcore {{ core.id }}</strong>
                  <span :class="['badge', core.status === 'unavailable' ? 'danger' : core.status === 'used' ? 'ok' : core.status === 'locked' ? 'warn' : 'idle']">{{ core.role }}</span>
                </div>
                <div class="kv-row"><span>{{ core.unavailable_reason || core.stream || '可分配' }}</span><strong>{{ core.usage_percent }}%</strong></div>
                <div class="progress" :class="{ warn: core.usage_percent >= 75, danger: core.usage_percent >= 95 }"><span :style="{ width: `${core.usage_percent}%` }"></span></div>
              </article>
            </div>
          </section>
        </template>

        <template v-else-if="currentPage === 'pcap'">
          <section class="summary-grid">
            <MetricCard label="PCAP 文件" :value="String(pcapFiles.length)" sub="来自后端 PCAP_ROOT" :icon="FileStack" />
            <MetricCard label="总包数" :value="formatCompact(totalPcapPackets)" sub="样例文件合计" :icon="PackageIcon" />
            <MetricCard label="总大小" :value="`${formatNumber(totalPcapSize, 1)} MB`" sub="可用于循环发包" :icon="HardDrive" />
            <MetricCard label="包长范围" :value="pcapRange" sub="按 L2 bytes 展示" :icon="Ruler" />
          </section>
          <section class="resource-grid">
            <article v-for="file in pcapFiles" :key="file.path" class="pcap-card">
              <div class="device-top">
                <div>
                  <div class="device-pci">{{ file.name }}</div>
                  <span class="muted">{{ file.path }}</span>
                </div>
                <button class="btn outline" type="button" @click="usePcap(file)"><Play :size="15" />创建</button>
              </div>
              <div class="pcap-stats">
                <div class="pcap-stat">包数<strong>{{ formatNumber(file.packet_count) }}</strong></div>
                <div class="pcap-stat">最大包长<strong>{{ file.max_len }} B</strong></div>
                <div class="pcap-stat">最小包长<strong>{{ file.min_len }} B</strong></div>
                <div class="pcap-stat">平均包长<strong>{{ file.avg_len }} B</strong></div>
              </div>
              <div class="kv-row"><span>文件大小</span><strong>{{ file.size_mb }} MB</strong></div>
            </article>
          </section>
        </template>

        <template v-else-if="currentPage === 'stats'">
          <section class="summary-grid">
            <MetricCard label="TX 速率" :value="`${formatNumber(totalRate)} Mbps`" sub="所有端口发送汇总" :icon="RadioReceiver" />
            <MetricCard label="RX 速率" :value="`${formatNumber(totalRxRate)} Mbps`" sub="所有端口接收汇总" :icon="RadioReceiver" />
            <MetricCard label="累计 TX/RX" :value="`${formatNumber(totalBytes, 2)} / ${formatNumber(totalRxBytes, 2)} GB`" sub="支持 reset 清零" :icon="Database" />
            <MetricCard label="TX/RX PPS" :value="`${formatNumber(totalPps)} / ${formatNumber(totalRxPps)}`" sub="按 3s 窗口计算" :icon="Waves" />
          </section>
          <section class="panel">
            <div class="panel-head">
              <h2>设备口实时统计</h2>
              <button class="btn danger" type="button" @click="resetStats(null)"><RotateCcw :size="16" />Reset 全部</button>
            </div>
            <div class="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>设备</th>
                    <th>Port</th>
                    <th>实时发送速率</th>
                    <th>TX PPS</th>
                    <th>累计发送</th>
                    <th>TX packets</th>
                    <th>TX drop</th>
                    <th>实时收包带宽</th>
                    <th>RX PPS</th>
                    <th>累计接收</th>
                    <th>RX packets</th>
                    <th>RX drop/error</th>
                    <th>操作</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="row in stats" :key="row.port_id">
                    <td><strong>{{ row.pci }}</strong></td>
                    <td>{{ row.port_id }}</td>
                    <td>{{ formatNumber(row.tx_mbps) }} Mbps</td>
                    <td>{{ formatNumber(row.tx_mpps * 1000000) }} pps</td>
                    <td>{{ formatNumber(row.total_tb * 1024, 2) }} GB</td>
                    <td>{{ formatNumber(row.tx_packets_m, 1) }} M</td>
                    <td>{{ formatNumber(row.tx_drops) }}</td>
                    <td>{{ formatNumber(row.rx_mbps) }} Mbps</td>
                    <td>{{ formatNumber(row.rx_mpps * 1000000) }} pps</td>
                    <td>{{ formatNumber(row.rx_total_gb, 2) }} GB</td>
                    <td>{{ formatNumber(row.rx_packets_m, 1) }} M</td>
                    <td>{{ formatNumber(row.rx_drops) }} / {{ formatNumber(row.rx_errors) }}</td>
                    <td><button class="btn outline" type="button" @click="resetStats(row.port_id)"><RotateCcw :size="15" />Reset</button></td>
                  </tr>
                </tbody>
              </table>
            </div>
          </section>
        </template>

        <template v-else-if="currentPage === 'history'">
          <section class="summary-grid">
            <MetricCard label="历史 Stream" :value="String(historyStreams.length)" sub="来自数据库" :icon="Database" />
            <MetricCard label="可用设备" :value="String(usableDevices.length)" sub="恢复时需重新选择" :icon="Server" />
            <MetricCard label="空闲 CPU 核" :value="String(freeCoreCount)" sub="恢复时重新分配" :icon="Cpu" />
            <MetricCard label="数据库" :value="historyEnabled ? '已启用' : '未启用'" :sub="historyError || '配置 DB_* 字段启用'" :icon="Database" />
          </section>

          <section class="panel">
            <div class="panel-head">
              <h2>历史 Stream</h2>
              <div class="actions">
                <div class="segmented compact history-filter">
                  <button :class="{ active: historyDirection === 'all' }" type="button" @click="setHistoryDirection('all')">全部</button>
                  <button :class="{ active: historyDirection === 'tx' }" type="button" @click="setHistoryDirection('tx')">TX</button>
                  <button :class="{ active: historyDirection === 'rx' }" type="button" @click="setHistoryDirection('rx')">RX</button>
                </div>
                <button class="btn outline" type="button" @click="refreshHistory"><RefreshCw :size="16" />刷新历史</button>
              </div>
            </div>
            <div class="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>名称</th>
                    <th>类型</th>
                    <th>模式</th>
                    <th>历史速率</th>
                    <th>包配置</th>
                    <th>保存时间</th>
                    <th>恢复设备</th>
                    <th>核数</th>
                    <th>恢复每核速率</th>
                    <th>恢复突发区</th>
                    <th>操作</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="item in historyStreams" :key="item.id">
                    <td><strong>{{ item.name }}</strong></td>
                    <td><span :class="['badge', historyDirectionOf(item) === 'rx' ? 'idle' : 'ok']"><i></i>{{ historyDirectionOf(item).toUpperCase() }}</span></td>
                    <td>{{ historyDirectionOf(item) === 'rx' ? '收包' : (item.mode === 'pcap' ? 'PCAP' : '构造包') }}</td>
                    <td>{{ historyDirectionOf(item) === 'rx' ? '-' : formatHistoryRates(item) }}</td>
                    <td>
                      <div class="history-config">
                        <template v-if="historyDirectionOf(item) === 'rx'">
                          <div><span>RX 设备</span><strong>{{ historyConfig(item).rx_port || '-' }}</strong></div>
                          <div><span>pcap dump</span><strong>{{ historyConfig(item).pcap_dump_enabled ? '开启' : '关闭' }}</strong></div>
                          <div><span>保存目录</span><strong>{{ runtime.settings?.rx_pcap_dump_root || runtime.rx_pcap_dump_root || '-' }}</strong></div>
                        </template>
                        <div v-else-if="item.mode === 'pcap'"><span>PCAP</span><strong>{{ historyConfig(item).pcap_path || '-' }}</strong></div>
                        <template v-else>
                          <div><span>MAC</span><strong>{{ historyConfig(item).src_mac }} -> {{ historyConfig(item).dst_mac }}</strong></div>
                          <div><span>IP</span><strong>{{ historyIpLine(item, 'src') }} / {{ historyIpLine(item, 'dst') }}</strong></div>
                          <div><span>端口</span><strong>{{ historyPortLine(item, 'src') }} / {{ historyPortLine(item, 'dst') }}</strong></div>
                          <div><span>Payload</span><strong>{{ formatNumber(historyConfig(item).payload_len) }} B</strong></div>
                        </template>
                      </div>
                    </td>
                    <td>{{ item.saved_at }}</td>
                    <td>
                      <select v-if="historyDirectionOf(item) === 'rx'" v-model="item.restore_rx_port">
                        <option v-for="device in usableDevices" :key="device.pci" :value="device.pci">{{ device.pci }}</option>
                      </select>
                      <select v-else v-model="item.restore_tx_port">
                        <option v-for="device in usableDevices" :key="device.pci" :value="device.pci">{{ device.pci }}</option>
                      </select>
                    </td>
                    <td>
                      <select v-model.number="item.restore_core_count" @change="syncHistoryRestoreWorkers(item)">
                        <option v-for="count in [1, 2, 3, 4]" :key="count" :value="count" :disabled="count > freeCoreCount">{{ count }}</option>
                      </select>
                    </td>
                    <td>
                      <div class="history-rate-grid">
                        <label v-if="historyDirectionOf(item) === 'rx'" class="check-row"><input v-model="item.restore_pcap_dump_enabled" type="checkbox">pcap_dump</label>
                        <template v-else>
                          <input
                            v-for="(_, index) in item.restore_worker_rates_mbps"
                            :key="index"
                            v-model.number="item.restore_worker_rates_mbps[index]"
                            type="number"
                            min="100"
                            max="10000"
                            inputmode="numeric"
                            @change="clampHistoryRate(item, index)"
                          >
                        </template>
                      </div>
                    </td>
                    <td>
                      <div class="history-rate-grid">
                        <template v-if="historyDirectionOf(item) !== 'rx'">
                          <input
                            v-for="(_, index) in item.restore_worker_burst_bytes"
                            :key="index"
                            v-model.number="item.restore_worker_burst_bytes[index]"
                            type="number"
                            min="2048"
                            max="16384"
                            step="1024"
                            inputmode="numeric"
                            @change="clampHistoryBurst(item, index)"
                          >
                        </template>
                      </div>
                    </td>
                    <td>
                      <div class="actions">
                        <button class="btn primary" type="button" @click="restoreHistory(item)"><Play :size="15" />恢复</button>
                        <button class="btn danger" type="button" @click="deleteHistory(item)"><Trash2 :size="15" />删除</button>
                      </div>
                    </td>
                  </tr>
                  <tr v-if="!historyStreams.length">
                    <td colspan="11">{{ historyEnabled ? '暂无历史 stream' : '数据库未启用或不可用' }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </section>
        </template>

        <template v-else>
          <section class="summary-grid">
            <MetricCard label="大页总量" :value="`${formatNumber(hugepages.total_mb, 1)} MB`" :sub="`${formatNumber(hugepages.total_pages)} pages`" :icon="Database" />
            <MetricCard label="大页空闲" :value="`${formatNumber(hugepages.free_mb, 1)} MB`" :sub="`${formatNumber(hugepages.free_pages)} pages`" :icon="HardDrive" />
            <MetricCard label="大页保留" :value="`${formatNumber(hugepages.reserved_pages)} pages`" sub="HugePages_Rsvd" :icon="LockKeyhole" />
            <MetricCard label="默认页大小" :value="hugepageSizeLabel" sub="Hugepagesize" :icon="Database" />
          </section>
          <section class="wide-grid">
            <div class="panel">
              <div class="panel-head"><h2>接口配置</h2><span class="badge idle">后端驱动</span></div>
              <div class="panel-body detail-list">
                <div class="detail-row"><span>认证字段</span><strong>auth_md5 = md5(secret + ':' + challenge)</strong></div>
                <div class="detail-row"><span>监听地址</span><strong>{{ runtime.listen }}</strong></div>
                <div class="detail-row"><span>PCAP 根目录</span><strong>{{ runtime.pcap_root }}</strong></div>
                <div class="detail-row"><span>DPDK 引擎</span><strong>{{ runtime.tx_engine?.status || '-' }}</strong></div>
                <div class="detail-row"><span>DPDK 引擎说明</span><strong>{{ txEngineMessage }}</strong></div>
                <div class="detail-row"><span>tx_ring_size</span><strong>{{ runtime.settings?.tx_ring_size ?? '-' }}</strong></div>
                <div class="detail-row"><span>rx_ring_size</span><strong>{{ runtime.settings?.rx_ring_size ?? '-' }}</strong></div>
                <div class="detail-row"><span>mbuf_pool_size</span><strong>{{ formatNumber(runtime.settings?.mbuf_pool_size) }}</strong></div>
                <div class="detail-row"><span>mbuf_cache_size</span><strong>{{ formatNumber(runtime.settings?.mbuf_cache_size) }}</strong></div>
                <div class="detail-row"><span>RX dump 目录</span><strong>{{ runtime.settings?.rx_pcap_dump_root || '-' }}</strong></div>
                <div class="detail-row"><span>数据库</span><strong>{{ runtime.settings?.db_enabled ? '启用' : '关闭' }} / {{ runtime.settings?.db_endpoint || '-' }}</strong></div>
                <div class="detail-row"><span>数据库用户</span><strong>{{ runtime.settings?.db_user || '-' }} / 密码{{ runtime.settings?.db_password_set ? '已配置' : '未配置' }}</strong></div>
                <div class="detail-row"><span>历史表</span><strong>{{ runtime.settings?.db_tx_stream_table || '-' }} / {{ runtime.settings?.db_rx_stream_table || '-' }}</strong></div>
                <div class="detail-row"><span>会话 token</span><strong>{{ sessionToken.slice(0, 16) }}...</strong></div>
              </div>
            </div>
            <div class="panel">
              <div class="panel-head">
                <h2>大页内存</h2>
                <span :class="['badge', hugepageBadge.kind]"><i></i>{{ hugepageBadge.text }}</span>
              </div>
              <div class="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th>Page size</th>
                      <th>Total</th>
                      <th>Free</th>
                      <th>Reserved</th>
                      <th>Surplus</th>
                      <th>Total MB</th>
                      <th>Free MB</th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-for="pool in hugepagePools" :key="pool.page_size_kb">
                      <td><strong>{{ formatPageSize(pool.page_size_kb) }}</strong></td>
                      <td>{{ formatNumber(pool.total_pages) }}</td>
                      <td>{{ formatNumber(pool.free_pages) }}</td>
                      <td>{{ formatNumber(pool.reserved_pages) }}</td>
                      <td>{{ formatNumber(pool.surplus_pages) }}</td>
                      <td>{{ formatNumber(pool.total_mb, 1) }}</td>
                      <td>{{ formatNumber(pool.free_mb, 1) }}</td>
                    </tr>
                    <tr v-if="!hugepagePools.length">
                      <td colspan="7">未发现 hugepage 池</td>
                    </tr>
                  </tbody>
                </table>
              </div>
            </div>
          </section>
        </template>
      </section>
    </main>

    <div v-if="selectedStream" class="modal-backdrop" @click.self="closeStreamDetails">
      <section class="modal-panel stream-detail-modal" role="dialog" aria-modal="true" aria-labelledby="stream-detail-title">
        <div class="modal-head">
          <div>
            <h2 id="stream-detail-title">{{ selectedStream.name }}</h2>
            <p>{{ streamDirection(selectedStream).toUpperCase() }} / {{ streamModeText(selectedStream) }}</p>
          </div>
          <button class="btn outline icon-btn" type="button" @click="closeStreamDetails"><X :size="16" /></button>
        </div>

        <div class="modal-body">
          <section v-for="group in streamDetailGroups(selectedStream)" :key="group.title" class="detail-section">
            <h3>{{ group.title }}</h3>
            <div class="detail-list">
              <div v-for="row in group.rows" :key="row.label" class="detail-row">
                <span>{{ row.label }}</span>
                <strong>{{ row.value }}</strong>
              </div>
            </div>
          </section>
        </div>
      </section>
    </div>
  </div>
</template>

<script setup>
import { computed, defineComponent, h, nextTick, onBeforeUnmount, reactive, ref, watch } from 'vue'
import md5 from 'blueimp-md5'
import {
  Activity,
  ChartNoAxesCombined,
  Circle,
  Cpu,
  Database,
  FileStack,
  GitBranch,
  HardDrive,
  Info,
  KeyRound,
  LockKeyhole,
  LogIn,
  LogOut,
  Package as PackageIcon,
  Pause,
  Play,
  Plus,
  RadioReceiver,
  RadioTower,
  RefreshCw,
  RotateCcw,
  Ruler,
  Server,
  Settings,
  Square,
  TimerReset,
  Trash2,
  Waves,
  X
} from '@lucide/vue'
import { BackendConnectionError, api, setAuthToken } from './api/client'

const MetricCard = defineComponent({
  props: {
    label: { type: String, required: true },
    value: { type: String, required: true },
    sub: { type: String, required: true },
    icon: { type: Object, required: true }
  },
  setup(props) {
    return () => h('div', { class: 'metric-card' }, [
      h('div', { class: 'metric-label' }, [h(props.icon, { size: 16 }), props.label]),
      h('div', { class: 'metric-value' }, props.value),
      h('div', { class: 'metric-sub' }, props.sub)
    ])
  }
})

const navItems = [
  { key: 'streams', label: 'Stream', icon: Activity, title: 'Stream 收发管理' },
  { key: 'devices', label: '设备', icon: Server, title: '设备资源' },
  { key: 'cores', label: 'CPU 核', icon: Cpu, title: 'CPU 核资源' },
  { key: 'pcap', label: 'PCAP', icon: FileStack, title: 'PCAP 文件' },
  { key: 'stats', label: '统计', icon: ChartNoAxesCombined, title: '收发统计' },
  { key: 'history', label: '历史stream', icon: Database, title: '历史 Stream' },
  { key: 'settings', label: '设置', icon: Settings, title: '设置' }
]

const MIN_WORKER_RATE_MBPS = 100
const MAX_WORKER_RATE_MBPS = 10000
const MIN_WORKER_BURST_BYTES = 2048
const MAX_WORKER_BURST_BYTES = 16 * 1024

const authenticated = ref(false)
const currentPage = ref('streams')
const loginSecret = ref('')
const loginDigest = ref('')
const loginError = ref('')
const error = ref('')
const refreshMessage = ref('')
const backendOnline = ref(true)
const reconnecting = ref(false)
const realtimeRefreshing = ref(false)
const sessionToken = ref('')
const createPanel = ref(null)
const selectedStream = ref(null)

const runtime = ref({})
const devices = ref([])
const cores = ref([])
const streams = ref([])
const pcapFiles = ref([])
const stats = ref([])
const historyStreams = ref([])
const historyEnabled = ref(false)
const historyError = ref('')
const historyDirection = ref('all')

const newStream = reactive({
  name: 'udp-load-04',
  direction: 'tx',
  tx_port: '',
  rx_port: '',
  core_count: 2,
  mode: 'pcap',
  pcap_path: '',
  loop: true,
  worker_rates_mbps: [2000, 2000],
  worker_burst_bytes: [2048, 2048],
  pcap_dump_enabled: false,
  construct: {
    l3: 'IPv4',
    l4: 'UDP',
    checksum_enabled: true,
    payload_len: 256,
    src_mac: '02:00:00:00:00:01',
    dst_mac: '02:00:00:00:00:02',
    src_ip_addr: '192.168.10.1',
    src_ip_mask: 24,
    src_ip_mode: 'increment',
    src_ip_step: 1,
    dst_ip_addr: '10.10.0.1',
    dst_ip_mask: 32,
    dst_ip_mode: 'fixed',
    dst_ip_step: 1,
    src_port_start: 10000,
    src_port_end: 20000,
    src_port_mode: 'increment',
    src_port_step: 1,
    dst_port_start: 53,
    dst_port_end: 53,
    dst_port_mode: 'increment',
    dst_port_step: 1,
    payload: '48 59 43 2d 44 50 44 4b 2d 54 58'
  }
})

const activeTitle = computed(() => navItems.find((item) => item.key === currentPage.value)?.title || '')
const txEngine = computed(() => runtime.value.tx_engine || {})
const txEngineReady = computed(() => txEngine.value.ready === true)
const txEngineMessage = computed(() => txEngine.value.message || '真实 DPDK TX/RX 引擎未接入，当前不会实际收发包')
const hugepages = computed(() => runtime.value.hugepages || {
  available: false,
  default_page_size_kb: 0,
  total_pages: 0,
  free_pages: 0,
  reserved_pages: 0,
  surplus_pages: 0,
  total_mb: 0,
  free_mb: 0,
  used_mb: 0,
  pools: []
})
const hugepagePools = computed(() => hugepages.value.pools || [])
const hugepageBadge = computed(() => {
  if (!hugepages.value.available) return { kind: 'danger', text: '未配置' }
  if ((hugepages.value.free_pages || 0) <= 0) return { kind: 'warn', text: '无空闲' }
  return { kind: 'ok', text: '可用' }
})
const hugepageSizeLabel = computed(() => formatPageSize(hugepages.value.default_page_size_kb))
const runningStreams = computed(() => streams.value.filter((stream) => stream.status === 'running').length)
const workerCores = computed(() => cores.value.filter((core) => core.status !== 'main').length)
const usedWorkerCores = computed(() => cores.value.filter((core) => core.status === 'used' || core.status === 'locked').length)
const freeCoreCount = computed(() => cores.value.filter((core) => core.status === 'idle').length)
const runningCoreCount = computed(() => cores.value.filter((core) => core.status === 'used').length)
const lockedCoreCount = computed(() => cores.value.filter((core) => core.status === 'locked').length)
const usableDevices = computed(() => devices.value.filter((device) => device.available !== false && device.link_up))
const linkUpDevices = computed(() => devices.value.filter((device) => device.available !== false && device.link_up).length)
const usedDevices = computed(() => devices.value.filter((device) => device.streams?.length).length)
const idleDevices = computed(() => devices.value.filter((device) => device.available !== false && device.link_up && !device.streams?.length).length)
const usedQueues = computed(() => devices.value.reduce((sum, device) => sum + device.used_tx_queues, 0))
const totalQueues = computed(() => devices.value.reduce((sum, device) => sum + device.total_tx_queues, 0))
const usedRxQueues = computed(() => devices.value.reduce((sum, device) => sum + (device.used_rx_queues || 0), 0))
const totalRxQueues = computed(() => devices.value.reduce((sum, device) => sum + (device.total_rx_queues || 0), 0))
const totalRate = computed(() => stats.value.reduce((sum, row) => sum + row.tx_mbps, 0))
const totalPps = computed(() => stats.value.reduce((sum, row) => sum + row.tx_mpps * 1000000, 0))
const totalBytes = computed(() => stats.value.reduce((sum, row) => sum + row.total_tb * 1024, 0))
const totalRxRate = computed(() => stats.value.reduce((sum, row) => sum + (row.rx_mbps || 0), 0))
const totalRxPps = computed(() => stats.value.reduce((sum, row) => sum + (row.rx_mpps || 0) * 1000000, 0))
const totalRxBytes = computed(() => stats.value.reduce((sum, row) => sum + (row.rx_total_gb || 0), 0))
const configuredTotalRate = computed(() => sumRates(newStream.worker_rates_mbps))
const totalPcapPackets = computed(() => pcapFiles.value.reduce((sum, file) => sum + file.packet_count, 0))
const totalPcapSize = computed(() => pcapFiles.value.reduce((sum, file) => sum + file.size_mb, 0))
const pcapRange = computed(() => {
  if (!pcapFiles.value.length) return '-'
  return `${Math.min(...pcapFiles.value.map((file) => file.min_len))}-${Math.max(...pcapFiles.value.map((file) => file.max_len))}`
})
const unavailableMessages = computed(() => [
  ...devices.value
    .filter((device) => device.available === false)
    .map((device) => `设备 ${device.pci} 不可用：${device.unavailable_reason || '需重启 dpdk_tx 系统'}`),
  ...cores.value
    .filter((core) => core.available === false || core.status === 'unavailable')
    .map((core) => `CPU 核 ${core.id} 不可用：${core.unavailable_reason || '需重启 dpdk_tx 系统'}`)
])

function formatNumber(value, digits = 0) {
  return Number(value || 0).toLocaleString('en-US', {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits
  })
}

function formatCompact(value) {
  return Intl.NumberFormat('en-US', { notation: 'compact', maximumFractionDigits: 2 }).format(value || 0)
}

function clampRate(value) {
  const number = Number(value)
  if (!Number.isFinite(number)) return MIN_WORKER_RATE_MBPS
  return Math.min(MAX_WORKER_RATE_MBPS, Math.max(MIN_WORKER_RATE_MBPS, Math.round(number)))
}

function clampBurstBytes(value) {
  const number = Number(value)
  if (!Number.isFinite(number)) return MIN_WORKER_BURST_BYTES
  return Math.min(MAX_WORKER_BURST_BYTES, Math.max(MIN_WORKER_BURST_BYTES, Math.round(number)))
}

function clampInt(value, min, max, fallback = min) {
  const number = Number(value)
  if (!Number.isFinite(number)) return fallback
  return Math.min(max, Math.max(min, Math.round(number)))
}

function setIpMode(side, mode) {
  newStream.construct[`${side}_ip_mode`] = mode
  if (mode === 'fixed') {
    newStream.construct[`${side}_ip_mask`] = 32
  }
  clampIpMask(side)
  clampIpStep(side)
}

function clampIpMask(side) {
  const key = `${side}_ip_mask`
  if (newStream.construct[`${side}_ip_mode`] === 'fixed') {
    newStream.construct[key] = 32
    return
  }
  newStream.construct[key] = clampInt(newStream.construct[key], 0, 32, 32)
}

function clampIpStep(side) {
  const key = `${side}_ip_step`
  newStream.construct[key] = Math.max(1, clampInt(newStream.construct[key], 1, 0xffffffff, 1))
}

function clampPortRange(side) {
  const startKey = `${side}_port_start`
  const endKey = `${side}_port_end`
  let start = clampInt(newStream.construct[startKey], 0, 65535, 0)
  let end = clampInt(newStream.construct[endKey], 0, 65535, start)
  if (start > end) {
    ;[start, end] = [end, start]
  }
  newStream.construct[startKey] = start
  newStream.construct[endKey] = end
}

function clampPortStep(side) {
  const key = `${side}_port_step`
  newStream.construct[key] = clampInt(newStream.construct[key], 1, 65535, 1)
}

function normalizeConstruct() {
  for (const side of ['src', 'dst']) {
    setIpMode(side, newStream.construct[`${side}_ip_mode`] || 'fixed')
    clampPortRange(side)
    clampPortStep(side)
  }
}

function sumRates(rates = []) {
  return rates.reduce((sum, rate) => sum + clampRate(rate), 0)
}

function ratesForCount(source = [], count, fallbackTotal = 0) {
  const targetCount = Math.max(1, Number(count || 1))
  const existing = Array.isArray(source) ? source.map(clampRate) : []
  if (!existing.length && fallbackTotal > 0) {
    const base = Math.floor(fallbackTotal / targetCount)
    let remainder = fallbackTotal % targetCount
    return Array.from({ length: targetCount }, () => {
      const value = clampRate(base + (remainder > 0 ? 1 : 0))
      if (remainder > 0) remainder -= 1
      return value
    })
  }
  while (existing.length < targetCount) existing.push(existing.length ? existing[existing.length - 1] : MIN_WORKER_RATE_MBPS)
  return existing.slice(0, targetCount)
}

function burstBytesForCount(source = [], count) {
  const targetCount = Math.max(1, Number(count || 1))
  const existing = Array.isArray(source) ? source.map(clampBurstBytes) : []
  while (existing.length < targetCount) existing.push(existing.length ? existing[existing.length - 1] : MIN_WORKER_BURST_BYTES)
  return existing.slice(0, targetCount)
}

function streamRates(stream) {
  return ratesForCount(stream.worker_rates_mbps, stream.cores?.length || 1, stream.target_mbps || 0)
}

function streamBursts(stream) {
  return burstBytesForCount(stream.worker_burst_bytes, stream.cores?.length || 1)
}

function streamTargetTotal(stream) {
  return stream.worker_rates_mbps?.length ? sumRates(stream.worker_rates_mbps) : Number(stream.target_mbps || 0)
}

function formatWorkerRates(stream) {
  const rates = streamRates(stream)
  return rates.map((rate, index) => `核${stream.cores?.[index] ?? index + 1}:${formatNumber(rate)}`).join(' / ')
}

function formatWorkerBursts(stream) {
  const bursts = streamBursts(stream)
  return bursts.map((bytes, index) => `核${stream.cores?.[index] ?? index + 1}:${formatNumber(bytes)}B`).join(' / ')
}

function coreItems(stream) {
  return (stream.cores || []).map((core, index) => ({
    key: `${stream.id || stream.name}-core-${index}`,
    label: `lcore ${core}`,
    sub: stream.queues?.[index] !== undefined ? `queue ${stream.queues[index]}` : 'queue -'
  }))
}

function workerRateItems(stream) {
  return streamRates(stream).map((rate, index) => ({
    key: `${stream.id || stream.name}-rate-${index}`,
    label: `核 ${stream.cores?.[index] ?? index + 1}`,
    value: `${formatNumber(rate)} Mbps`
  }))
}

function workerBurstItems(stream) {
  return streamBursts(stream).map((bytes, index) => ({
    key: `${stream.id || stream.name}-burst-${index}`,
    label: `核 ${stream.cores?.[index] ?? index + 1}`,
    value: `${formatNumber(bytes)} B`
  }))
}

function streamDirection(stream) {
  return stream.direction === 'rx' ? 'rx' : 'tx'
}

function historyDirectionOf(item) {
  return item.direction === 'rx' ? 'rx' : 'tx'
}

function formatHistoryRates(item) {
  const rates = ratesForCount(item.worker_rates_mbps, item.worker_rates_mbps?.length || item.restore_core_count || 1, item.rate_mbps || 0)
  return rates.length ? rates.map((rate) => `${formatNumber(rate)}`).join(' / ') + ' Mbps' : `${formatNumber(item.rate_mbps)} Mbps`
}

function sequenceModeLabel(value) {
  return { random: '随机', increment: '递增', fixed: '固定' }[value] || value || '-'
}

function historyConfig(item) {
  const defaults = {
    direction: 'tx',
    pcap_path: '',
    loop: true,
    rx_port: '',
    pcap_dump_enabled: false,
    l3: 'IPv4',
    l4: 'UDP',
    src_mac: '02:00:00:00:00:01',
    dst_mac: '02:00:00:00:00:02',
    src_ip_addr: '192.168.0.1',
    src_ip_mask: 32,
    src_ip_mode: 'fixed',
    src_ip_step: 1,
    dst_ip_addr: '192.168.0.2',
    dst_ip_mask: 32,
    dst_ip_mode: 'fixed',
    dst_ip_step: 1,
    src_port_start: 10000,
    src_port_end: 10000,
    src_port_mode: 'increment',
    src_port_step: 1,
    dst_port_start: 53,
    dst_port_end: 53,
    dst_port_mode: 'increment',
    dst_port_step: 1,
    payload_len: 64,
    checksum_enabled: true
  }
  if (item.config_summary) return { ...defaults, ...item.config_summary }
  let parsed = {}
  try {
    parsed = JSON.parse(item.config_json || '{}')
  } catch {
    parsed = {}
  }
  const construct = parsed.construct || parsed
  return {
    ...defaults,
    direction: parsed.direction || construct.direction || 'tx',
    pcap_path: parsed.pcap_path || construct.pcap_path || '',
    loop: parsed.loop ?? construct.loop ?? true,
    rx_port: parsed.rx_port || construct.rx_port || '',
    pcap_dump_enabled: parsed.pcap_dump_enabled ?? construct.pcap_dump_enabled ?? false,
    l3: construct.l3 || parsed.l3 || 'IPv4',
    l4: construct.l4 || parsed.l4 || 'UDP',
    src_mac: construct.src_mac || '02:00:00:00:00:01',
    dst_mac: construct.dst_mac || '02:00:00:00:00:02',
    src_ip_addr: construct.src_ip_addr || construct.src_ip || '192.168.0.1',
    src_ip_mask: construct.src_ip_mask ?? 32,
    src_ip_mode: construct.src_ip_mode || 'fixed',
    src_ip_step: construct.src_ip_step ?? 1,
    dst_ip_addr: construct.dst_ip_addr || construct.dst_ip || '192.168.0.2',
    dst_ip_mask: construct.dst_ip_mask ?? 32,
    dst_ip_mode: construct.dst_ip_mode || 'fixed',
    dst_ip_step: construct.dst_ip_step ?? 1,
    src_port_start: construct.src_port_start ?? 10000,
    src_port_end: construct.src_port_end ?? 10000,
    src_port_mode: construct.src_port_mode || 'increment',
    src_port_step: construct.src_port_step ?? 1,
    dst_port_start: construct.dst_port_start ?? 53,
    dst_port_end: construct.dst_port_end ?? 53,
    dst_port_mode: construct.dst_port_mode || 'increment',
    dst_port_step: construct.dst_port_step ?? 1,
    payload_len: construct.payload_len ?? 64,
    checksum_enabled: construct.checksum_enabled ?? true
  }
}

function historyIpLine(item, side) {
  const config = historyConfig(item)
  const mode = config[`${side}_ip_mode`]
  const step = mode === 'increment' ? ` step ${formatNumber(config[`${side}_ip_step`])}` : ''
  return `${side === 'src' ? '源' : '目的'} ${config[`${side}_ip_addr`]}/${config[`${side}_ip_mask`]} ${sequenceModeLabel(mode)}${step}`
}

function historyPortLine(item, side) {
  const config = historyConfig(item)
  const mode = config[`${side}_port_mode`]
  const start = config[`${side}_port_start`]
  const end = config[`${side}_port_end`]
  const step = mode === 'increment' && start !== end ? ` step ${formatNumber(config[`${side}_port_step`])}` : ''
  return `${side === 'src' ? '源' : '目的'} ${start}-${end} ${sequenceModeLabel(mode)}${step}`
}

function streamConfig(stream) {
  return historyConfig(stream || {})
}

function yesNo(value) {
  return value ? '开启' : '关闭'
}

function streamModeText(stream) {
  if (!stream) return '-'
  if (streamDirection(stream) === 'rx') {
    return stream.pcap_dump_enabled ? '收包 / pcap dump' : '收包'
  }
  return stream.mode === 'pcap' ? 'PCAP 循环' : '构造包'
}

function openStreamDetails(stream) {
  selectedStream.value = stream
}

function closeStreamDetails() {
  selectedStream.value = null
}

function streamDetailGroups(stream) {
  if (!stream) return []
  const config = streamConfig(stream)
  const direction = streamDirection(stream)
  const groups = [
    {
      title: '基础信息',
      rows: [
        { label: '名称', value: stream.name || '-' },
        { label: '类型', value: direction.toUpperCase() },
        { label: '状态', value: stream.status || '-' },
        { label: '模式', value: streamModeText(stream) },
        { label: '设备', value: direction === 'rx' ? (stream.rx_port || '-') : (stream.tx_port || '-') },
        { label: 'Core', value: (stream.cores || []).map((core) => `lcore ${core}`).join(', ') || '-' },
        { label: 'Queue', value: (stream.queues || []).join(', ') || '-' }
      ]
    }
  ]

  if (direction === 'rx') {
    groups.push({
      title: 'RX 配置',
      rows: [
        { label: 'pcap dump', value: yesNo(stream.pcap_dump_enabled || config.pcap_dump_enabled) },
        { label: '保存目录', value: stream.pcap_dump_dir || config.pcap_dump_dir || '-' },
        { label: 'dump 文件', value: `${formatNumber(stream.dump_files)} 个` },
        { label: 'dump 错误', value: formatNumber(stream.dump_errors) }
      ]
    })
    return groups
  }

  groups.push({
    title: 'Worker 发包',
    rows: [
      { label: '每核目标', value: formatWorkerRates(stream) || '-' },
      { label: '突发区', value: formatWorkerBursts(stream) || '-' },
      { label: '目标总计', value: `${formatNumber(streamTargetTotal(stream))} Mbps` }
    ]
  })

  if (stream.mode === 'pcap') {
    groups.push({
      title: 'PCAP 配置',
      rows: [
        { label: 'PCAP 文件', value: config.pcap_path || '-' },
        { label: '循环发送', value: yesNo(config.loop) }
      ]
    })
    return groups
  }

  groups.push(
    {
      title: '协议与 MAC',
      rows: [
        { label: 'IP 类型', value: config.l3 || '-' },
        { label: '传输层', value: config.l4 || '-' },
        { label: 'Checksum', value: yesNo(config.checksum_enabled) },
        { label: '源 MAC', value: config.src_mac || '-' },
        { label: '目的 MAC', value: config.dst_mac || '-' }
      ]
    },
    {
      title: 'IP 范围',
      rows: [
        { label: '源 IP', value: historyIpLine(stream, 'src') },
        { label: '目的 IP', value: historyIpLine(stream, 'dst') }
      ]
    },
    {
      title: '端口与 Payload',
      rows: [
        { label: '源端口', value: historyPortLine(stream, 'src') },
        { label: '目的端口', value: historyPortLine(stream, 'dst') },
        { label: 'Payload 长度', value: `${formatNumber(config.payload_len)} B` }
      ]
    }
  )
  return groups
}

function streamTooltip(stream) {
  return streamDetailGroups(stream)
    .flatMap((group) => [
      `[${group.title}]`,
      ...group.rows.map((row) => `${row.label}: ${row.value}`)
    ])
    .join('\n')
}

function mergeStreamStats(streamStats = []) {
  if (!Array.isArray(streamStats) || !streamStats.length) return
  const actualById = new Map(streamStats.map((item) => [item.id, item]))
  streams.value = streams.value.map((stream) => (
    actualById.has(stream.id)
      ? { ...stream, ...actualById.get(stream.id) }
      : stream
  ))
}

function formatPageSize(kb) {
  const value = Number(kb || 0)
  if (!value) return '-'
  if (value >= 1024 * 1024) return `${formatNumber(value / 1024 / 1024, 1)} GB`
  if (value >= 1024) return `${formatNumber(value / 1024, 1)} MB`
  return `${formatNumber(value)} KB`
}

function deviceBadge(device) {
  if (device.available === false) return { kind: 'danger', text: '不可用' }
  if (!device.link_up) return { kind: 'danger', text: '不可用' }
  if (device.streams?.length) return { kind: 'ok', text: '占用中' }
  return { kind: 'idle', text: '空闲' }
}

function queuePercent(device) {
  if (!device.total_tx_queues) return 0
  return Math.round((device.used_tx_queues / device.total_tx_queues) * 100)
}

function clampWorkerRate(index) {
  newStream.worker_rates_mbps[index] = clampRate(newStream.worker_rates_mbps[index])
}

function clampWorkerBurst(index) {
  newStream.worker_burst_bytes[index] = clampBurstBytes(newStream.worker_burst_bytes[index])
}

function syncNewStreamWorkers() {
  newStream.worker_rates_mbps = ratesForCount(newStream.worker_rates_mbps, newStream.core_count, configuredTotalRate.value || 0)
  newStream.worker_burst_bytes = burstBytesForCount(newStream.worker_burst_bytes, newStream.core_count)
}

function syncHistoryRestoreWorkers(item) {
  item.restore_worker_rates_mbps = ratesForCount(item.restore_worker_rates_mbps, item.restore_core_count, item.rate_mbps || 0)
  item.restore_worker_burst_bytes = burstBytesForCount(item.restore_worker_burst_bytes, item.restore_core_count)
}

function clampHistoryRate(item, index) {
  item.restore_worker_rates_mbps[index] = clampRate(item.restore_worker_rates_mbps[index])
}

function clampHistoryBurst(item, index) {
  item.restore_worker_burst_bytes[index] = clampBurstBytes(item.restore_worker_burst_bytes[index])
}

function markBackendOnline() {
  backendOnline.value = true
  if (error.value === '后端连接中断，请重启后端服务') {
    error.value = ''
  }
}

function handleRequestError(err, target = error) {
  if (err instanceof BackendConnectionError || err?.isConnectionError) {
    backendOnline.value = false
    target.value = '后端连接中断，请重启后端服务'
    return
  }
  if (String(err?.message || '').includes('401') || err?.message === 'unauthorized') {
    backendOnline.value = false
    target.value = '登录会话失效，请点击 Reconnect 重新登录'
    return
  }
  target.value = err?.message || '请求失败'
}

async function login() {
  loginError.value = ''
  backendOnline.value = true
  const secret = loginSecret.value.trim()
  if (!secret) {
    loginError.value = '请输入密钥'
    return
  }

  try {
    const challenge = await api.getChallenge()
    const digest = md5(`${secret}:${challenge.challenge}`)
    loginDigest.value = digest
    const result = await api.login(digest)
    sessionToken.value = result.token
    setAuthToken(result.token)
    authenticated.value = true
    markBackendOnline()
    await refreshAll()
  } catch (err) {
    handleRequestError(err, loginError)
  }
}

function logout() {
  authenticated.value = false
  sessionToken.value = ''
  loginSecret.value = ''
  loginDigest.value = ''
  setAuthToken('')
}

async function reconnectBackend() {
  reconnecting.value = true
  loginError.value = ''
  error.value = ''
  try {
    await api.getChallenge()
    backendOnline.value = true
    authenticated.value = false
    sessionToken.value = ''
    loginSecret.value = ''
    loginDigest.value = ''
    setAuthToken('')
  } catch (err) {
    handleRequestError(err)
  } finally {
    reconnecting.value = false
  }
}

async function switchPage(page) {
  currentPage.value = page
  refreshMessage.value = ''
  if (page === 'history') {
    try {
      await refreshHistory()
    } catch (err) {
      handleRequestError(err)
    }
  }
}

async function refreshAll() {
  error.value = ''
  refreshMessage.value = ''
  try {
    const [runtimeData, deviceData, coreData, streamData, pcapData, statData] = await Promise.all([
      api.runtime(),
      api.devices(),
      api.cores(),
      api.streams(),
      api.pcapFiles(),
      api.stats()
    ])
    runtime.value = runtimeData
    devices.value = deviceData.devices
    cores.value = coreData.cores
    streams.value = streamData.streams
    pcapFiles.value = pcapData.files
    stats.value = statData.ports
    mergeStreamStats(statData.streams)
    markBackendOnline()
    if (!newStream.tx_port && usableDevices.value.length) newStream.tx_port = usableDevices.value[0].pci
    if (!newStream.rx_port && usableDevices.value.length) newStream.rx_port = usableDevices.value[0].pci
    if (!newStream.pcap_path && pcapFiles.value.length) newStream.pcap_path = pcapFiles.value[0].path
    if (currentPage.value === 'history') await refreshHistory()
  } catch (err) {
    handleRequestError(err)
  }
}

async function refreshCurrent() {
  error.value = ''
  refreshMessage.value = ''
  try {
    if (currentPage.value === 'devices') {
      await api.refreshResources('devices')
      const deviceData = await api.devices()
      devices.value = deviceData.devices
      markBackendOnline()
      refreshMessage.value = '设备资源已刷新'
      return
    }
    if (currentPage.value === 'cores') {
      await api.refreshResources('cores')
      const coreData = await api.cores()
      cores.value = coreData.cores
      markBackendOnline()
      refreshMessage.value = 'CPU 核资源已刷新'
      return
    }
    if (currentPage.value === 'pcap') {
      await api.refreshResources('pcap')
      const pcapData = await api.pcapFiles()
      pcapFiles.value = pcapData.files
      markBackendOnline()
      if (!newStream.pcap_path && pcapFiles.value.length) newStream.pcap_path = pcapFiles.value[0].path
      refreshMessage.value = 'PCAP 目录已重新扫描'
      return
    }
    if (currentPage.value === 'history') {
      await refreshHistory()
      refreshMessage.value = '历史 Stream 已刷新'
      return
    }
    await refreshAll()
    refreshMessage.value = '数据已刷新'
  } catch (err) {
    handleRequestError(err)
  }
}

async function refreshHistory() {
  const result = await api.historyStreams(historyDirection.value)
  markBackendOnline()
  historyEnabled.value = result.enabled !== false
  historyError.value = result.error || ''
  const defaultPort = usableDevices.value[0]?.pci || ''
  historyStreams.value = (result.streams || []).map((item) => ({
    ...item,
    restore_tx_port: item.restore_tx_port || defaultPort,
    restore_rx_port: item.restore_rx_port || historyConfig(item).rx_port || defaultPort,
    restore_pcap_dump_enabled: item.restore_pcap_dump_enabled ?? historyConfig(item).pcap_dump_enabled,
    restore_core_count: item.restore_core_count || Math.max(1, item.worker_rates_mbps?.length || item.worker_burst_bytes?.length || 1),
    restore_worker_rates_mbps: ratesForCount(item.worker_rates_mbps, item.restore_core_count || item.worker_rates_mbps?.length || 1, item.rate_mbps || 0),
    restore_worker_burst_bytes: burstBytesForCount(item.worker_burst_bytes, item.restore_core_count || item.worker_burst_bytes?.length || item.worker_rates_mbps?.length || 1)
  }))
}

async function setHistoryDirection(direction) {
  historyDirection.value = direction
  await refreshHistory()
}

async function refreshRealtime() {
  if (!authenticated.value || !backendOnline.value || realtimeRefreshing.value) return
  realtimeRefreshing.value = true
  try {
    const [coreData, statData] = await Promise.all([api.cores(), api.stats()])
    markBackendOnline()
    cores.value = coreData.cores
    stats.value = statData.ports
    mergeStreamStats(statData.streams)
  } catch (err) {
    handleRequestError(err)
  } finally {
    realtimeRefreshing.value = false
  }
}

async function createStream() {
  if (!txEngineReady.value) {
    error.value = txEngineMessage.value
    return
  }
  try {
    if (newStream.direction === 'tx') {
      syncNewStreamWorkers()
    }
    if (newStream.direction === 'tx' && newStream.mode === 'construct') {
      normalizeConstruct()
    }
    const payload = newStream.direction === 'rx'
      ? {
          direction: 'rx',
          name: newStream.name,
          rx_port: newStream.rx_port,
          core_count: newStream.core_count,
          pcap_dump_enabled: newStream.pcap_dump_enabled
        }
      : {
          ...newStream,
          direction: 'tx',
          rate_mbps: configuredTotalRate.value,
          worker_rates_mbps: newStream.worker_rates_mbps.map(clampRate),
          worker_burst_bytes: newStream.worker_burst_bytes.map(clampBurstBytes)
        }
    const result = await api.createStream(payload)
    await refreshAll()
    if (result?.warning) {
      refreshMessage.value = result.warning
    }
  } catch (err) {
    handleRequestError(err)
  }
}

async function startStream(id) {
  try {
    await api.startStream(id)
    await refreshAll()
  } catch (err) {
    handleRequestError(err)
  }
}

async function stopStream(id) {
  try {
    await api.stopStream(id)
    await refreshAll()
  } catch (err) {
    handleRequestError(err)
  }
}

async function stopAllStreams() {
  try {
    await Promise.all(streams.value.filter((stream) => stream.status === 'running').map((stream) => api.stopStream(stream.id)))
    await refreshAll()
  } catch (err) {
    handleRequestError(err)
  }
}

async function deleteStream(id) {
  try {
    await api.deleteStream(id)
    await refreshAll()
  } catch (err) {
    handleRequestError(err)
  }
}

async function resetStats(portId) {
  try {
    await api.resetStats(portId)
    await refreshAll()
  } catch (err) {
    handleRequestError(err)
  }
}

async function restoreHistory(item) {
  try {
    syncHistoryRestoreWorkers(item)
    if (historyDirectionOf(item) === 'rx') {
      await api.restoreHistoryStream(item.id, {
        direction: 'rx',
        rx_port: item.restore_rx_port,
        core_count: item.restore_core_count,
        pcap_dump_enabled: item.restore_pcap_dump_enabled
      }, 'rx')
    } else {
      await api.restoreHistoryStream(item.id, {
        direction: 'tx',
        tx_port: item.restore_tx_port,
        core_count: item.restore_core_count,
        worker_rates_mbps: item.restore_worker_rates_mbps.map(clampRate),
        worker_burst_bytes: item.restore_worker_burst_bytes.map(clampBurstBytes),
        rate_mbps: sumRates(item.restore_worker_rates_mbps)
      }, 'tx')
    }
    currentPage.value = 'streams'
    await refreshAll()
  } catch (err) {
    handleRequestError(err)
  }
}

async function deleteHistory(item) {
  if (!window.confirm(`删除历史 stream "${item.name}"？`)) return
  try {
    await api.deleteHistoryStream(item.id, historyDirectionOf(item))
    await refreshHistory()
    refreshMessage.value = '历史 Stream 已删除'
  } catch (err) {
    handleRequestError(err)
  }
}

function usePcap(file) {
  currentPage.value = 'streams'
  newStream.mode = 'pcap'
  newStream.pcap_path = file.path
  focusCreate()
}

function focusCreate() {
  nextTick(() => createPanel.value?.scrollIntoView?.({ behavior: 'smooth', block: 'start' }))
}

watch(() => newStream.core_count, syncNewStreamWorkers, { immediate: true })

const timer = window.setInterval(refreshRealtime, 3000)
onBeforeUnmount(() => window.clearInterval(timer))
</script>
