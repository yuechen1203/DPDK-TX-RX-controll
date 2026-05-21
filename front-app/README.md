# DPDK TX Vue 前端

## 启动

```bash
npm install
npm run dev
```

默认 Vite 地址：

```text
http://127.0.0.1:5173
```

`vite.config.js` 已将 `/api` 代理到：

```text
http://127.0.0.1:8080
```

## 登录

前端登录流程：

1. `GET /api/auth/challenge`
2. 计算 `auth_md5 = md5(secret + ':' + challenge)`
3. `POST /api/auth/login`
4. 保存后端返回的 `token`
5. 后续请求携带 `Authorization: Bearer <token>`

默认后端密钥见 `../back-app/config/default.yaml`，当前为：

```text
110110
```

## 页面

- Stream：stream 列表、创建 stream、开始/停止/删除，每个发包线程可单独设置 `100-10000 Mbps` 和 `2048-16384 bytes` 突发区
- 设备：可用设备、占用设备、空闲设备、TX queue 使用量
- CPU 核：lcore 状态和 1s 使用率刷新
- PCAP：包数、最大/最小/平均包长、文件大小
- 统计：端口实时速率、累计发送数据、reset

当后端 `/api/runtime` 返回 `tx_engine.ready=false` 时，页面会提示 TX 引擎未就绪，并禁用创建/启动 stream，避免界面显示“正在发包”但后端实际没有发包。
