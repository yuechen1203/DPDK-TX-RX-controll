const API_BASE = import.meta.env.VITE_API_BASE || ''
const DEFAULT_TIMEOUT_MS = Number(import.meta.env.VITE_API_TIMEOUT_MS || 2500)

let authToken = ''

export class BackendConnectionError extends Error {
  constructor(message = '后端连接中断，请重启后端服务') {
    super(message)
    this.name = 'BackendConnectionError'
    this.isConnectionError = true
  }
}

async function request(path, options = {}) {
  const timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS
  const controller = new AbortController()
  const timeoutId = window.setTimeout(() => controller.abort(), timeoutMs)
  const headers = {
    Accept: 'application/json',
    ...(options.body ? { 'Content-Type': 'application/json' } : {}),
    ...(authToken ? { Authorization: `Bearer ${authToken}` } : {}),
    ...options.headers
  }

  let response
  try {
    response = await fetch(`${API_BASE}${path}`, {
      ...options,
      headers,
      signal: controller.signal
    })
  } catch (err) {
    throw new BackendConnectionError()
  } finally {
    window.clearTimeout(timeoutId)
  }

  const text = await response.text()
  let payload = null
  if (text) {
    try {
      payload = JSON.parse(text)
    } catch {
      payload = null
    }
  }

  if (!response.ok) {
    if (response.status >= 500 && /proxy|ECONNREFUSED|fetch|connect/i.test(text)) {
      throw new BackendConnectionError()
    }
    const message = payload?.error || `${response.status} ${response.statusText}`
    throw new Error(message)
  }

  return payload
}

export function setAuthToken(token) {
  authToken = token || ''
}

export const api = {
  getChallenge() {
    return request('/api/auth/challenge')
  },

  login(authMd5) {
    return request('/api/auth/login', {
      method: 'POST',
      body: JSON.stringify({ auth_md5: authMd5 })
    })
  },

  runtime() {
    return request('/api/runtime')
  },

  devices() {
    return request('/api/devices')
  },

  cores() {
    return request('/api/cores')
  },

  streams() {
    return request('/api/streams')
  },

  pcapFiles() {
    return request('/api/pcap/files')
  },

  stats() {
    return request('/api/stats')
  },

  refreshResources(target = 'all') {
    return request('/api/resources/refresh', {
      method: 'POST',
      body: JSON.stringify({ target })
    })
  },

  historyStreams(direction = 'all') {
    return request(`/api/history/streams?direction=${encodeURIComponent(direction)}`)
  },

  restoreHistoryStream(id, payload, direction = 'tx') {
    return request(`/api/history/streams/${direction}/${id}/restore`, {
      method: 'POST',
      body: JSON.stringify(payload)
    })
  },

  deleteHistoryStream(id, direction = 'tx') {
    return request(`/api/history/streams/${direction}/${id}`, { method: 'DELETE' })
  },

  createStream(stream) {
    return request('/api/streams', {
      method: 'POST',
      body: JSON.stringify(stream)
    })
  },

  startStream(id) {
    return request(`/api/streams/${id}/start`, { method: 'POST' })
  },

  stopStream(id) {
    return request(`/api/streams/${id}/stop`, { method: 'POST' })
  },

  deleteStream(id) {
    return request(`/api/streams/${id}`, { method: 'DELETE' })
  },

  resetStats(portId = null) {
    return request('/api/stats/reset', {
      method: 'POST',
      body: JSON.stringify({ port_id: portId })
    })
  }
}
