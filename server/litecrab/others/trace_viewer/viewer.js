/* =====================================================================
 * LiteCrab Trace Viewer
 * Parses agent_trace_*.jsonl files, groups events by session/trace,
 * and renders an interactive execution-flow visualization.
 * ===================================================================== */

'use strict';

// ===== Global state =====
const state = {
  files: [],           // [{ name, size, mtime }]
  events: [],          // all raw events across all files (parsed)
  sessions: new Map(), // sessionId -> SessionData
  selectedSessionId: null,
  expandedTraces: new Set(),
  expandedEvents: new Set(),
  filters: { search: '', error: false, tool: false, slow: false },
  sessionFilter: '',
};
const traceModel = globalThis.TraceModel;

// ===== Util =====
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
};

function fmtTime(ms) {
  if (!ms) return '-';
  const d = new Date(ms);
  const pad = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
function fmtTimeShort(ms) {
  if (!ms) return '-';
  const d = new Date(ms);
  const pad = n => String(n).padStart(2, '0');
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
function fmtDuration(ms) {
  if (ms == null) return '-';
  if (ms < 1) return ms + 'ms';
  if (ms < 1000) return ms.toFixed(0) + 'ms';
  if (ms < 60000) return (ms/1000).toFixed(2) + 's';
  const m = Math.floor(ms/60000);
  const s = Math.floor((ms%60000)/1000);
  return `${m}m${s}s`;
}
function fmtNum(n) {
  if (n == null) return '0';
  if (n < 1000) return String(n);
  if (n < 1e6) return (n/1000).toFixed(1) + 'k';
  return (n/1e6).toFixed(2) + 'M';
}
function truncate(s, n) {
  if (!s) return '';
  s = String(s);
  return s.length > n ? s.slice(0, n) + '…' : s;
}
function escapeHtml(s) {
  if (s == null) return '';
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}
function tryParseJson(s) {
  if (!s) return null;
  try { return JSON.parse(s); } catch { return null; }
}
function prettyJson(obj) {
  if (obj == null) return '';
  try { return JSON.stringify(obj, null, 2); } catch { return String(obj); }
}
// Render the messages array the agent sent to the LLM in a readable layout.
// Each message is shown as a labeled block ([system] / [user] / [assistant] /
// [tool] ...) with newlines inside `content` preserved as real line breaks
// (instead of \n-escaped JSON strings) so system prompts and user messages
// remain readable. Falls back to the raw string if the input isn't a
// messages array.
function formatLlmInput(raw) {
  if (raw == null || raw === '') return '';
  // Newer traces store input as a parsed JSON array of {role, content}
  // objects; older traces stored it as a JSON string. Handle both, plus
  // the single-message object case.
  let messages = null;
  if (Array.isArray(raw)) {
    messages = raw;
  } else if (typeof raw === 'object') {
    messages = [raw];
  } else {
    const obj = tryParseJson(raw);
    if (Array.isArray(obj)) messages = obj;
    else if (obj && typeof obj === 'object') messages = [obj];
  }
  if (!messages || messages.length === 0) {
    return String(raw);
  }

  const parts = [];
  for (const msg of messages) {
    if (!msg || typeof msg !== 'object') {
      parts.push(String(msg));
      continue;
    }
    const role = msg.role || 'unknown';
    let contentStr;
    const content = msg.content;
    if (content == null) {
      contentStr = '(空)';
    } else if (typeof content === 'string') {
      contentStr = content;
    } else if (Array.isArray(content)) {
      // Multimodal content (e.g. OpenAI vision format:
      // [{type:'text',text:...},{type:'image_url',...}]).
      contentStr = content.map(part => {
        if (!part || typeof part !== 'object') return String(part);
        if (part.type === 'text') return part.text || '';
        if (part.type === 'image_url') {
          const url = part.image_url && part.image_url.url;
          return `[image: ${url ? '(url)' : '(inline)'}]`;
        }
        return prettyJson(part);
      }).join('\n');
    } else {
      contentStr = prettyJson(content);
    }

    const label = `[${role}]`;
    const sep = '─'.repeat(Math.max(0, 72 - label.length - 4));
    const lines = [`── ${label} ${sep}`];
    lines.push(contentStr);
    if (msg.tool_calls) lines.push('', '[tool_calls]', prettyJson(msg.tool_calls));
    if (msg.name) lines.push(`[name] ${msg.name}`);
    if (msg.tool_call_id) lines.push(`[tool_call_id] ${msg.tool_call_id}`);
    parts.push(lines.join('\n'));
  }
  return parts.join('\n\n');
}

// Parse the structured tool output text into { summary, content, truncated, exitCode }
// Format (from LiteCrab builtin_tools):
//   tool: <name>
//   success: <bool>
//   exit_code: <int>
//   <blank>
//   summary:
//     <indented summary>
//   <blank>
//   content:
//     <indented content lines>
//   <blank>
//   truncated: <bool>
//   raw_ref: ...
function parseToolOutput(text) {
  const empty = { raw: text || '', summary: '', content: '', truncated: false, exitCode: null };
  if (!text) return empty;
  const out = { ...empty };
  const lines = text.split('\n');

  // Extract exit_code from header
  for (const ln of lines) {
    if (ln.startsWith('exit_code:')) {
      const v = ln.slice(9).trim();
      out.exitCode = v === '' ? null : Number(v);
    }
  }

  // Locate section markers and slice between them.
  let summaryStart = -1, summaryEnd = -1;
  let contentStart = -1, contentEnd = lines.length;
  let truncIdx = -1;
  for (let i = 0; i < lines.length; i++) {
    const ln = lines[i];
    if (ln === 'summary:' || ln.startsWith('summary:')) {
      summaryStart = i + 1;
    } else if (ln === 'content:' || ln.startsWith('content:')) {
      contentStart = i + 1;
      if (summaryEnd < 0) summaryEnd = i;
    } else if (ln === 'truncated:' || ln.startsWith('truncated:')) {
      truncIdx = i;
      const v = ln.slice(10).trim().toLowerCase();
      out.truncated = v === 'true';
      if (contentEnd > i) contentEnd = i;
    } else if (ln === 'raw_ref:' || ln.startsWith('raw_ref:')) {
      if (contentEnd > i) contentEnd = i;
    }
  }

  if (summaryStart >= 0) {
    if (summaryEnd < 0) summaryEnd = contentStart >= 1 ? contentStart - 1 : lines.length;
    out.summary = lines.slice(summaryStart, summaryEnd).join('\n').trim();
  }
  if (contentStart >= 0) {
    out.content = lines.slice(contentStart, contentEnd).join('\n').replace(/\s+$/, '');
  }
  return out;
}

// ===== Toast =====
let toastTimer = null;
function toast(msg, dur = 2200) {
  const t = $('#toast');
  t.textContent = msg;
  t.classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.add('hidden'), dur);
}

// ===== Modal =====
function showModal(title, body) {
  $('#modalTitle').textContent = title;
  $('#modalBody').textContent = body;
  $('#modal').classList.remove('hidden');
}
function hideModal() { $('#modal').classList.add('hidden'); }

// ===== File loading =====
async function loadFiles(fileList) {
  const files = Array.from(fileList).filter(f =>
    f.name.endsWith('.jsonl') || f.name.endsWith('.json') ||
    f.type.includes('json') || f.type === ''
  );
  if (files.length === 0) { toast('未找到 .jsonl 文件'); return; }
  let added = 0, failed = 0;
  for (const f of files) {
    try {
      const text = await f.text();
      const events = parseJsonl(text);
      for (const ev of events) {
        ev._file = f.name;
        state.events.push(ev);
      }
      state.files.push({ name: f.name, size: f.size, mtime: f.lastModified });
      added++;
    } catch (e) {
      console.error('Failed to load', f.name, e);
      failed++;
    }
  }
  rebuildSessions();
  renderAll();
  toast(`加载 ${added} 个文件${failed ? `，${failed} 个失败` : ''}`);
}

function parseJsonl(text) {
  const out = [];
  const lines = text.split(/\r?\n/);
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line) continue;
    try {
      out.push(traceModel.normalizeEvent(JSON.parse(line)));
    } catch (e) {
      // skip malformed line
    }
  }
  return out;
}

// ===== Build session / trace index =====
function rebuildSessions() {
  state.sessions.clear();

  // Pass 1: build traceId -> sessionId map using trace_start events
  // (only trace_start events reliably carry both traceId and sessionId).
  const traceToSession = new Map();
  for (const ev of state.events) {
    if (ev.type === 'trace_start' && ev.traceId && ev.sessionId) {
      traceToSession.set(ev.traceId, ev.sessionId);
    }
  }

  // Pass 2: route each event that has an explicit sessionId OR a traceId.
  // Events with neither are deferred to Pass 3 (time-window assignment).
  const orphanEvents = [];
  for (const ev of state.events) {
    const sid = ev.sessionId ||
                (ev.traceId && traceToSession.get(ev.traceId));
    if (sid) {
      addEventToSession(sid, ev);
    } else {
      orphanEvents.push(ev);
    }
  }

  // Pass 3: assign orphan events (typically log/skill lifecycle entries
  // that carry neither sessionId nor traceId) to the session whose time
  // range covers them. Pick the session with the smallest enclosing range.
  for (const ev of orphanEvents) {
    let bestSid = null;
    let bestSpan = Infinity;
    for (const s of state.sessions.values()) {
      if (ev.timeMs >= s.firstMs && ev.timeMs <= s.lastMs) {
        const span = s.lastMs - s.firstMs;
        if (span < bestSpan) {
          bestSpan = span;
          bestSid = s.sessionId;
        }
      }
    }
    if (bestSid) {
      addEventToSession(bestSid, ev);
    } else {
      addEventToSession('_orphan', ev);
    }
  }

  // Pass 4: trace windows are complete now. Attach session-scoped logs to the
  // narrowest enclosing trace. Doing this while ingesting events loses logs
  // emitted before trace_end has expanded the trace time window.
  for (const s of state.sessions.values()) {
    for (const ev of s.events) {
      if (ev.type !== 'log' || ev.traceId) continue;
      let bestTrace = null;
      let bestSpan = Infinity;
      for (const tr of s.traces.values()) {
        if (ev.timeMs < tr.startTimeMs || ev.timeMs > tr.endTimeMs) continue;
        const span = tr.endTimeMs - tr.startTimeMs;
        if (span < bestSpan) {
          bestSpan = span;
          bestTrace = tr;
        }
      }
      if (bestTrace) bestTrace.events.push(ev);
    }
  }

  // Pass 5: per-trace summary + per-session aggregate
  for (const s of state.sessions.values()) {
    s.traceIds.sort((a, b) => {
      const ta = s.traces.get(a).startTimeMs;
      const tb = s.traces.get(b).startTimeMs;
      return ta - tb;
    });
    s.totalTraces = s.traceIds.length;
    s.okCount = 0; s.errorCount = 0; s.anomalyCount = 0;
    s.totalDuration = 0;
    s.totalPrompt = 0; s.totalCompletion = 0;
    s.totalLlmCalls = 0; s.totalToolCalls = 0;
    for (const tid of s.traceIds) {
      const tr = finalizeTrace(s.traces.get(tid));
      s.totalDuration += tr.durationMs || 0;
      s.totalPrompt += tr.promptTokens || 0;
      s.totalCompletion += tr.completionTokens || 0;
      s.totalLlmCalls += tr.llmCalls || 0;
      s.totalToolCalls += tr.toolCalls || 0;
      s.anomalyCount += tr.anomalyCount;
      if (tr.status === 'ok' && tr.anomalyCount === 0) s.okCount++;
      else if (tr.status === 'error' || tr.anomalyCount > 0) s.errorCount++;
    }
    s.hasError = s.errorCount > 0;
  }
}

function addEventToSession(sid, ev) {
  if (!state.sessions.has(sid)) {
    state.sessions.set(sid, {
      sessionId: sid,
      userIds: new Set(),
      traceIds: [],
      traces: new Map(),
      events: [],
      firstMs: ev.timeMs,
      lastMs: ev.timeMs,
    });
  }
  const s = state.sessions.get(sid);
  s.events.push(ev);
  if (ev.userId) s.userIds.add(ev.userId);
  if (ev.timeMs < s.firstMs) s.firstMs = ev.timeMs;
  if (ev.timeMs > s.lastMs) s.lastMs = ev.timeMs;

  if (ev.traceId) {
    if (!s.traces.has(ev.traceId)) {
      s.traces.set(ev.traceId, {
        traceId: ev.traceId,
        sessionId: sid,
        events: [],
        startTimeMs: ev.timeMs,
        endTimeMs: ev.timeMs,
      });
      s.traceIds.push(ev.traceId);
    }
    const tr = s.traces.get(ev.traceId);
    tr.events.push(ev);
    if (ev.timeMs < tr.startTimeMs) tr.startTimeMs = ev.timeMs;
    if (ev.timeMs > tr.endTimeMs) tr.endTimeMs = ev.timeMs;
  }
}

function finalizeTrace(tr) {
  if (tr._finalized) return tr;
  tr.input = null;
  tr.finalOutput = null;
  tr.status = 'unknown';
  tr.durationMs = tr.endTimeMs - tr.startTimeMs;
  tr.promptTokens = 0; tr.completionTokens = 0;
  tr.llmCalls = 0; tr.toolCalls = 0;
  tr.tools = []; tr.anomalies = []; tr.logs = [];
  tr.anomalyCount = 0;
  tr.userId = null;

  for (const ev of tr.events) {
    switch (ev.type) {
      case 'trace_start':
        tr.input = ev.input || '';
        tr.userId = ev.userId;
        tr.rootSpanId = ev.rootSpanId;
        break;
      case 'trace_end':
        tr.finalOutput = ev.finalOutput || '';
        tr.status = ev.status || 'unknown';
        tr.durationMs = ev.durationMs != null ? ev.durationMs : tr.durationMs;
        tr.promptTokens = ev.promptTokens || 0;
        tr.completionTokens = ev.completionTokens || 0;
        tr.llmCalls = ev.llmCalls || 0;
        tr.toolCalls = ev.toolCalls || 0;
        tr.endTimeMs = ev.timeMs;
        break;
      case 'tool':
        tr.tools.push(ev);
        break;
      case 'anomaly':
        tr.anomalies.push(ev);
        tr.anomalyCount++;
        break;
      case 'log':
        // Logs already attached to this trace via traceId or time-window routing
        // (see addEventToSession). Include them so the event flow is complete.
        tr.logs.push(ev);
        break;
      case 'span_start': case 'span_end': case 'llm': case 'user_input':
      case 'skill_event': case 'skill_start': case 'skill_end':
      case 'skill_step': case 'skill_resume':
        // Track these but mostly render via logs
        break;
    }
  }
  if (tr.status === 'unknown' && tr.anomalyCount > 0) tr.status = 'error';
  tr._finalized = true;
  return tr;
}

// ===== Renderers =====
function renderAll() {
  const hasData = state.events.length > 0;
  $('#dropZone').classList.toggle('hidden', hasData);
  $('#app').classList.toggle('hidden', !hasData);
  if (!hasData) return;
  renderStats();
  renderSessions();
  renderTracePanel();
}

function computeGlobalStats() {
  let totalTraces = 0, okCount = 0, errorCount = 0, anomalyCount = 0;
  let totalDuration = 0, totalPrompt = 0, totalCompletion = 0;
  let totalLlm = 0, totalTools = 0;
  let firstMs = Infinity, lastMs = -Infinity;
  let toolNames = new Set(), userIds = new Set();
  for (const s of state.sessions.values()) {
    if (s.sessionId === '_orphan') continue;
    totalTraces += s.totalTraces;
    okCount += s.okCount;
    errorCount += s.errorCount;
    anomalyCount += s.anomalyCount;
    totalDuration += s.totalDuration;
    totalPrompt += s.totalPrompt;
    totalCompletion += s.totalCompletion;
    totalLlm += s.totalLlmCalls;
    totalTools += s.totalToolCalls;
    if (s.firstMs < firstMs) firstMs = s.firstMs;
    if (s.lastMs > lastMs) lastMs = s.lastMs;
    for (const u of s.userIds) userIds.add(u);
  }
  return {
    files: state.files.length,
    sessions: Array.from(state.sessions.keys()).filter(id => id !== '_orphan').length,
    totalTraces, okCount, errorCount, anomalyCount,
    totalDuration, totalPrompt, totalCompletion,
    totalLlm, totalTools,
    firstMs: firstMs === Infinity ? null : firstMs,
    lastMs: lastMs === -1 ? null : lastMs,
    userIds: userIds.size,
  };
}

function renderStats() {
  const s = computeGlobalStats();
  const successRate = s.totalTraces ? ((s.okCount / s.totalTraces) * 100).toFixed(1) + '%' : '-';
  const totalTokens = s.totalPrompt + s.totalCompletion;
  const wallMs = (s.firstMs != null && s.lastMs != null) ? (s.lastMs - s.firstMs) : null;

  const cards = [
    { label: 'Trace 文件', value: fmtNum(s.files), cls: '' },
    { label: 'Sessions', value: fmtNum(s.sessions), cls: 'info' },
    { label: '用户数', value: fmtNum(s.userIds), cls: 'cyan' },
    { label: '总请求数', value: fmtNum(s.totalTraces), cls: 'purple' },
    { label: '成功 / 失败', value: `${fmtNum(s.okCount)} / ${fmtNum(s.errorCount)}`, sub: `成功率 ${successRate}`, cls: s.errorCount > 0 ? 'error' : 'ok' },
    { label: 'LLM 调用', value: fmtNum(s.totalLlm), cls: 'purple' },
    { label: '工具调用', value: fmtNum(s.totalTools), cls: 'cyan' },
    { label: '异常事件', value: fmtNum(s.anomalyCount), cls: s.anomalyCount > 0 ? 'error' : '' },
    { label: 'Token 总量', value: fmtNum(totalTokens), sub: `↑${fmtNum(s.totalPrompt)} / ↓${fmtNum(s.totalCompletion)}`, cls: 'info' },
    { label: 'Agent 处理耗时', value: fmtDuration(s.totalDuration), sub: '所有请求累计', cls: 'warn' },
    { label: '日志时间跨度', value: wallMs != null ? fmtDuration(wallMs) : '-', sub: wallMs != null ? `${fmtTimeShort(s.firstMs)} → ${fmtTimeShort(s.lastMs)}` : '', cls: 'pink' },
  ];
  const bar = $('#statsBar');
  bar.innerHTML = '';
  for (const c of cards) {
    const card = el('div', `stat-card ${c.cls}`);
    card.appendChild(el('div', 'stat-label', c.label));
    card.appendChild(el('div', 'stat-value', c.value));
    if (c.sub) card.appendChild(el('div', 'stat-sub', c.sub));
    bar.appendChild(card);
  }
}

function renderSessions() {
  const list = $('#sessionList');
  const search = state.sessionFilter.trim().toLowerCase();
  list.innerHTML = '';
  $('#sessionCount').textContent = Array.from(state.sessions.keys()).filter(id => id !== '_orphan').length;

  const sessions = Array.from(state.sessions.values())
    .filter(s => {
      if (!search) return true;
      if (s.sessionId.toLowerCase().includes(search)) return true;
      for (const u of s.userIds) if (u.toLowerCase().includes(search)) return true;
      return false;
    })
    .sort((a, b) => a.sessionId === '_orphan' ? 1 : b.sessionId === '_orphan' ? -1 : a.firstMs - b.firstMs);

  if (sessions.length === 0) {
    list.innerHTML = '<div style="padding:14px;text-align:center;color:var(--text-muted);font-size:12px;">无匹配 session</div>';
    return;
  }

  for (const s of sessions) {
    const orphan = s.sessionId === '_orphan';
    const item = el('div', 'session-item');
    if (s.sessionId === state.selectedSessionId) item.classList.add('active');

    const head = el('div', 'session-item-head');
    head.appendChild(el('div', 'session-id', orphan ? '未归属日志' : s.sessionId));
    const status = el('div', `session-status ${s.hasError ? 'has-error' : ''}`,
      orphan ? '诊断' : s.hasError ? '有异常' : 'OK');
    head.appendChild(status);
    item.appendChild(head);

    const meta = el('div', 'session-meta');
    meta.appendChild(el('span', null, orphan ? `${s.events.length} 事件` : `${s.totalTraces} 请求`));
    meta.appendChild(el('span', null, `${s.totalToolCalls} 工具`));
    meta.appendChild(el('span', null, `${s.totalLlmCalls} LLM`));
    if (s.anomalyCount > 0) meta.appendChild(el('span', null, `${s.anomalyCount} 异常`));
    item.appendChild(meta);

    item.appendChild(el('div', 'session-time', fmtTime(s.firstMs)));

    item.addEventListener('click', () => {
      state.selectedSessionId = s.sessionId;
      state.expandedTraces.clear();
      renderSessions();
      renderTracePanel();
    });
    list.appendChild(item);
  }
}

function renderTracePanel() {
  const panel = $('#tracePanel');
  panel.innerHTML = '';
  if (!state.selectedSessionId) {
    panel.innerHTML = '<div class="empty-state"><p>从左侧选择一个 session 查看其执行流程</p></div>';
    return;
  }
  const s = state.sessions.get(state.selectedSessionId);
  if (!s) {
    panel.innerHTML = '<div class="empty-state"><p>Session 不存在</p></div>';
    return;
  }

  const content = el('div', 'trace-panel-content');
  content.appendChild(renderSessionHeader(s));
  content.appendChild(renderTimeline(s));

  const title = el('div', 'section-title', `执行流程 · ${s.totalTraces} 个请求（按时间顺序）`);
  content.appendChild(title);

  let traces = s.traceIds.map(tid => s.traces.get(tid));
  traces = applyFilters(traces);
  if (traces.length === 0) {
    content.appendChild(el('div', 'empty-state', '当前过滤条件下没有 trace'));
  } else {
    for (let i = 0; i < traces.length; i++) {
      content.appendChild(renderTraceCard(traces[i], i + 1));
    }
  }

  panel.appendChild(content);
}

function renderSessionHeader(s) {
  const header = el('div', 'session-header');
  header.appendChild(el('h3', null, s.sessionId));
  const userLine = el('div', 'session-user');
  userLine.textContent = `用户: ${s.userIds.size ? Array.from(s.userIds).join(', ') : '-'} · 时间: ${fmtTime(s.firstMs)} → ${fmtTime(s.lastMs)}`;
  header.appendChild(userLine);

  const stats = el('div', 'session-stats');
  const cards = [
    { label: '请求数', value: s.totalTraces },
    { label: 'OK / 异常', value: `${s.okCount} / ${s.errorCount}` },
    { label: '工具调用', value: s.totalToolCalls },
    { label: 'LLM 调用', value: s.totalLlmCalls },
    { label: 'Prompt tokens', value: fmtNum(s.totalPrompt) },
    { label: 'Completion tokens', value: fmtNum(s.totalCompletion) },
    { label: '异常事件', value: s.anomalyCount },
    { label: '累计处理', value: fmtDuration(s.totalDuration) },
  ];
  for (const c of cards) {
    const card = el('div', 'session-stat');
    card.appendChild(el('div', 'stat-label', c.label));
    card.appendChild(el('div', 'stat-value', String(c.value)));
    stats.appendChild(card);
  }
  header.appendChild(stats);
  return header;
}

function renderTimeline(s) {
  const wrap = el('div');
  const title = el('div', 'section-title', '请求时间轴');
  wrap.appendChild(title);
  const tl = el('div', 'timeline');
  if (s.traceIds.length === 0 || s.lastMs === s.firstMs) {
    tl.style.opacity = '.4';
  }
  const span = Math.max(1, s.lastMs - s.firstMs);
  for (const tid of s.traceIds) {
    const tr = s.traces.get(tid);
    const left = ((tr.startTimeMs - s.firstMs) / span) * 100;
    const width = Math.max(0.5, ((tr.endTimeMs - tr.startTimeMs) / span) * 100);
    const bar = el('div', `timeline-bar ${tr.status === 'error' || tr.anomalyCount > 0 ? 'error' : (tr.status === 'ok' ? '' : 'warn')}`);
    bar.style.left = left + '%';
    bar.style.width = width + '%';
    bar.title = `${tr.traceId}\n${truncate(tr.input, 80)}\n${fmtTime(tr.startTimeMs)} · ${fmtDuration(tr.durationMs)}`;
    bar.addEventListener('click', () => {
      state.expandedTraces.add(tr.traceId);
      renderTracePanel();
      // scroll into view
      setTimeout(() => {
        const card = $(`[data-trace-id="${cssEscape(tr.traceId)}"]`);
        if (card) card.scrollIntoView({ behavior: 'smooth', block: 'center' });
      }, 50);
    });
    tl.appendChild(bar);
  }
  wrap.appendChild(tl);
  const axis = el('div', 'timeline-axis');
  axis.appendChild(el('span', null, fmtTimeShort(s.firstMs)));
  axis.appendChild(el('span', null, fmtTimeShort(s.lastMs)));
  wrap.appendChild(axis);
  return wrap;
}

function cssEscape(s) {
  if (window.CSS && typeof CSS.escape === 'function') return CSS.escape(s);
  return String(s).replace(/[^a-zA-Z0-9_-]/g, m => '\\' + m);
}

function applyFilters(traces) {
  const f = state.filters;
  const search = f.search.trim().toLowerCase();
  return traces.filter(tr => {
    if (f.error && tr.anomalyCount === 0 && tr.status !== 'error') return false;
    if (f.tool && tr.toolCalls === 0) return false;
    if (f.slow && (tr.durationMs || 0) < 3000) return false;
    if (search) {
      const hay = [
        tr.traceId, tr.input, tr.finalOutput, tr.status,
        ...tr.tools.map(t => t.toolName + ' ' + (t.input || '')),
        ...tr.anomalies.map(a => a.detail || ''),
        ...tr.logs.map(l => l.message || ''),
      ].join('\n').toLowerCase();
      if (!hay.includes(search)) return false;
    }
    return true;
  });
}

function renderTraceCard(tr, idx) {
  const card = el('div', 'trace-card');
  card.dataset.traceId = tr.traceId;
  if (tr.anomalyCount > 0) card.classList.add('has-error');
  else if (tr.status === 'ok') card.classList.add('ok');
  else if (tr.status === 'error') card.classList.add('has-error');
  if (state.expandedTraces.has(tr.traceId)) card.classList.add('expanded');

  // head
  const head = el('div', 'trace-head');
  head.appendChild(el('div', 'trace-index', String(idx)));
  const title = el('div', 'trace-title');
  title.appendChild(el('div', 'trace-input-summary', truncate(tr.input || '(无输入)', 120)));
  title.appendChild(el('div', 'trace-id', `${tr.traceId} · ${fmtTime(tr.startTimeMs)}`));
  head.appendChild(title);

  const badges = el('div', 'trace-badges');
  badges.appendChild(el('div', `badge status-${tr.status === 'ok' ? 'ok' : 'error'}`, tr.status || '-'));
  badges.appendChild(el('div', 'badge duration', fmtDuration(tr.durationMs)));
  if (tr.llmCalls > 0) badges.appendChild(el('div', 'badge llm', `${tr.llmCalls} LLM`));
  if (tr.toolCalls > 0) badges.appendChild(el('div', 'badge tool', `${tr.toolCalls} 工具`));
  if (tr.anomalyCount > 0) badges.appendChild(el('div', 'badge anomaly', `${tr.anomalyCount} 异常`));
  const totalTok = (tr.promptTokens || 0) + (tr.completionTokens || 0);
  if (totalTok > 0) badges.appendChild(el('div', 'badge tokens', `${fmtNum(tr.promptTokens)}/${fmtNum(tr.completionTokens)}`));
  head.appendChild(badges);

  head.appendChild(el('div', 'trace-chevron', '▸'));
  head.addEventListener('click', () => {
    if (state.expandedTraces.has(tr.traceId)) state.expandedTraces.delete(tr.traceId);
    else state.expandedTraces.add(tr.traceId);
    card.classList.toggle('expanded');
  });
  card.appendChild(head);

  // body
  const body = el('div', 'trace-body');

  // Input section
  const inputSec = el('div', 'trace-section');
  inputSec.appendChild(el('div', 'trace-section-label', `用户输入${tr.userId ? ' · ' + tr.userId : ''}`));
  const inputBox = el('div', 'trace-input', tr.input || '(无输入)');
  inputBox.style.cursor = 'pointer';
  inputBox.title = '点击查看完整内容';
  inputBox.addEventListener('click', () => showModal('用户输入', tr.input || ''));
  inputSec.appendChild(inputBox);
  body.appendChild(inputSec);

  // Event flow
  body.appendChild(renderEventFlow(tr));

  // Final output
  const outSec = el('div', 'trace-section');
  outSec.appendChild(el('div', 'trace-section-label', `最终输出${tr.status ? ' · ' + tr.status : ''}`));
  const outBox = el('div', 'trace-output', tr.finalOutput || '(无输出)');
  outBox.style.cursor = 'pointer';
  outBox.title = '点击查看完整内容';
  outBox.addEventListener('click', () => showModal('最终输出', tr.finalOutput || ''));
  outSec.appendChild(outBox);
  body.appendChild(outSec);

  card.appendChild(body);
  return card;
}

function renderEventFlow(tr) {
  const wrap = el('div', 'trace-section');
  wrap.appendChild(el('div', 'trace-section-label', '执行流程'));
  const flow = el('div', 'event-flow');

  // Build ordered event list (logs, tools, anomalies, skill events) interleaved by timeMs.
  const items = [];
  for (const ev of tr.events) {
    if (ev.type === 'trace_start' || ev.type === 'trace_end') continue;
    if (ev.type === 'log') {
      // skip logs that don't belong to this trace
      if (ev.traceId && ev.traceId !== tr.traceId) continue;
      items.push({ ev, kind: classifyLog(ev), time: ev.timeMs });
    } else if (ev.type === 'tool') {
      items.push({ ev, kind: 'tool', time: ev.timeMs });
    } else if (ev.type === 'anomaly') {
      items.push({ ev, kind: 'anomaly', time: ev.timeMs });
    } else if (ev.type && ev.type.startsWith('skill')) {
      items.push({ ev, kind: 'skill_lifecycle', time: ev.timeMs });
    } else if (ev.type === 'llm' || ev.type === 'span_start' || ev.type === 'span_end' || ev.type === 'user_input') {
      items.push({ ev, kind: ev.type, time: ev.timeMs });
    }
  }
  items.sort((a, b) => a.time - b.time);

  if (items.length === 0) {
    const empty = el('div', 'event-item');
    empty.style.paddingLeft = '0';
    empty.style.fontStyle = 'italic';
    empty.style.color = 'var(--text-muted)';
    empty.textContent = '(无中间事件)';
    flow.innerHTML = '';
    flow.appendChild(empty);
    wrap.appendChild(flow);
    return wrap;
  }

  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    flow.appendChild(renderEventItem(it, i, tr));
  }
  wrap.appendChild(flow);
  return wrap;
}

function classifyLog(ev) {
  const comp = ev.component || '';
  const msg = ev.message || '';
  if (comp === 'skill' && msg.includes('skill.run.state_changed')) return 'skill_state';
  if (comp === 'agent' && msg.includes('skill_')) return 'skill_lifecycle';
  if (comp === 'skill_router') return 'skill_router';
  return 'log';
}

function renderEventItem(it, idx, tr) {
  const { ev, kind } = it;
  const itemKey = `${tr.traceId}#${idx}`;
  const item = el('div', `event-item ${kind}`);
  if (ev.level === 'warn' || ev.level === 'warning') item.classList.add('warn');
  if (ev.level === 'error') item.classList.add('error');
  if (ev.status === 'error' || ev.exitCode != null && ev.exitCode !== 0) item.classList.add('error');
  if (state.expandedEvents.has(itemKey)) item.classList.add('expanded');

  const head = el('div', 'event-head');
  let typeLabel = ev.type;
  let name = '';
  let detailObj = null;
  let detailText = '';

  if (kind === 'tool') {
    typeLabel = 'TOOL';
    name = ev.toolName || '(tool)';
    const parsed = parseToolOutput(ev.output);
    const status = ev.status || (ev.exitCode === 0 ? 'ok' : 'error');
    detailObj = {
      toolName: ev.toolName,
      input: ev.input,
      exitCode: ev.exitCode,
      status: ev.status,
      summary: parsed.summary,
      truncated: parsed.truncated,
      content: parsed.content,
      raw: parsed.raw,
    };
    detailText = `【输入】\n${ev.input || '(空)'}\n\n【输出】\n${parsed.content || '(空)'}${parsed.truncated ? '\n\n(输出被截断，原始已保留)' : ''}`;
  } else if (kind === 'anomaly') {
    typeLabel = 'ANOMALY';
    name = ev.category || 'anomaly';
    detailText = `category: ${ev.category || ''}\nlevel: ${ev.level || ''}\n\n${ev.detail || ''}`;
  } else if (kind === 'skill_state') {
    typeLabel = 'SKILL_STATE';
    const transition = extractSkillTransition(ev);
    const skillName = extractSkillName(ev);
    name = [transition, skillName].filter(Boolean).join(' · ') || 'skill';
    detailText = `component: ${ev.component || ''}\nlevel: ${ev.level || ''}\nmessage: ${ev.message || ''}`;
  } else if (kind === 'skill_lifecycle') {
    typeLabel = 'SKILL_LIFECYCLE';
    const action = extractSkillAction(ev);
    const skillName = extractSkillName(ev);
    name = [action, skillName].filter(Boolean).join(' · ') || 'skill';
    detailText = `component: ${ev.component || ''}\nlevel: ${ev.level || ''}\nmessage: ${ev.message || ''}`;
  } else if (kind === 'skill_router') {
    typeLabel = 'SKILL_ROUTE';
    const data = ev.structured || traceModel.parseStructuredMessage(ev.message);
    const action = traceModel.routeActionLabel(data.action);
    name = [action, data.skill].filter(Boolean).join(' · ');
    detailText = `event: ${data.event || ''}\naction: ${action}\nskill: ${data.skill || '(none)'}\nconfidence: ${data.confidence ?? '-'}\nreason: ${data.reason || data.reasonCode || ''}\nsessionId: ${ev.sessionId || ''}\nskillRunId: ${ev.skillRunId || ''}\ninterruptionId: ${ev.interruptionId || ''}\ncorrelationToken: ${ev.correlationToken || ''}`;
  } else if (kind === 'log') {
    typeLabel = ev.level ? ev.level.toUpperCase() : 'LOG';
    name = ev.component || '';
    detailText = `component: ${ev.component || ''}\nlevel: ${ev.level || ''}\n\n${ev.message || ''}`;
  } else if (kind === 'llm') {
    typeLabel = 'LLM';
    name = `${ev.model || '(unknown model)'} · round ${ev.iteration || '?'}`;
    const tools = Array.isArray(ev.toolNames) ? ev.toolNames.join(', ') : (ev.toolNames || '');
    const llmInput = ev.input ? formatLlmInput(ev.input) : '';
    const toolCalls = Array.isArray(ev.toolCalls) && ev.toolCalls.length ? prettyJson(ev.toolCalls) : '(无)';
    const errorCode = ev.errorCode == null ? '(未记录)' : ev.errorCode;
    detailText = `status: ${ev.status || ''}\niteration: ${ev.iteration || ''}\nmodel: ${ev.model || ''}\ntoolUse: ${ev.toolUse ? 'yes' : 'no'}\ntoolNames: ${tools || '(none)'}\npromptTokens: ${ev.promptTokens || 0}\ncompletionTokens: ${ev.completionTokens || 0}\nerrorCode: ${errorCode}` +
      (llmInput ? `\n\n【发给LLM的内容】\n${llmInput}` : '') +
      `\n\n【LLM返回的文本】\n${ev.output || '(无文本)'}` +
      `\n\n【LLM返回的工具调用】\n${toolCalls}`;
  } else {
    typeLabel = ev.type.toUpperCase();
    name = '';
    detailText = prettyJson(ev);
  }

  const typeEl = el('span', `event-type ${kind}`, typeLabel);
  head.appendChild(typeEl);
  if (name) head.appendChild(el('span', 'event-name', name));
  if (kind === 'tool' && ev.toolName) {
    const inputShort = truncate(traceModel.redactSecrets(tryExtractShort(ev.input)), 60);
    if (inputShort) head.appendChild(el('span', 'event-name', inputShort));
  }
  if (ev.status) {
    head.appendChild(el('span', `event-status ${ev.status === 'ok' ? 'ok' : 'error'}`, ev.status));
  } else if (ev.exitCode != null) {
    head.appendChild(el('span', `event-status ${ev.exitCode === 0 ? 'ok' : 'error'}`, `exit=${ev.exitCode}`));
  }
  head.appendChild(el('span', 'event-time', fmtTimeShort(ev.timeMs)));

  item.appendChild(head);

  detailText = traceModel.redactSecrets(detailText);
  const detail = el('div', 'event-detail', detailText);
  item.appendChild(detail);

  head.addEventListener('click', () => {
    if (state.expandedEvents.has(itemKey)) state.expandedEvents.delete(itemKey);
    else state.expandedEvents.add(itemKey);
    item.classList.toggle('expanded');
  });

  // double-click opens full raw in modal
  head.addEventListener('dblclick', (e) => {
    e.stopPropagation();
    let title = `${typeLabel} · ${name}`;
    let body = detailText;
    if (kind === 'tool') {
      title = `Tool: ${ev.toolName}`;
      body = `== 输入 ==\n${ev.input || '(空)'}\n\n== 完整输出 ==\n${ev.output || '(空)'}`;
    } else if (kind === 'anomaly') {
      title = `Anomaly: ${ev.category}`;
      body = prettyJson(ev);
    } else if (kind === 'llm') {
      title = `LLM: ${ev.model || '(unknown)'} · round ${ev.iteration || '?'}`;
      const llmInput = ev.input ? formatLlmInput(ev.input) : '(未记录；设置 LITECRAB_TRACE_LOG_LLM_INPUT=1 后重启可记录)';
      const toolCalls = Array.isArray(ev.toolCalls) && ev.toolCalls.length ? prettyJson(ev.toolCalls) : '(无)';
      body = `== 发给LLM的内容 ==\n${llmInput}\n\n== LLM返回的文本 ==\n${ev.output || '(无文本)'}\n\n== LLM返回的工具调用 ==\n${toolCalls}`;
    } else {
      body = prettyJson(ev);
    }
    showModal(title, traceModel.redactSecrets(body));
  });

  return item;
}

function tryExtractShort(input) {
  if (!input) return '';
  const obj = tryParseJson(input);
  if (!obj) return input;
  // pick the most informative field
  if (obj.path) return obj.path;
  if (obj.script) return obj.script;
  if (obj.pattern) return obj.pattern;
  if (obj.skillPath) return obj.skillPath;
  if (obj.query) return obj.query;
  return input;
}

function extractSkillName(ev) {
  if (ev.skillName) return ev.skillName;
  const m = (ev.message || '').match(/(?:name|skill)=([^\s]+)/);
  return m ? m[1] : '';
}

function extractSkillTransition(ev) {
  const msg = ev.message || '';
  const from = msg.match(/from=([^\s]+)/);
  const to = msg.match(/to=([^\s]+)/);
  return from && to ? `${from[1]} → ${to[1]}` : '';
}

function extractSkillAction(ev) {
  if (ev.type && ev.type.startsWith('skill_')) return ev.type.slice('skill_'.length);
  const match = (ev.message || '').match(/\bskill_(start|resume|step|end)\b/);
  return match ? match[1] : '';
}

// ===== Event wiring =====
function on(id, evName, handler) {
  const n = $(id);
  if (n) n.addEventListener(evName, handler);
}
function wireEvents() {
  // File inputs (drop zone + header)
  const onFiles = (e) => { loadFiles(e.target.files); e.target.value = ''; };
  on('#fileInput', 'change', onFiles);
  on('#dirInput', 'change', onFiles);
  on('#fileInput2', 'change', onFiles);
  on('#dirInput2', 'change', onFiles);

  // Clear
  on('#clearBtn', 'click', () => {
    state.files = []; state.events = []; state.sessions.clear();
    state.selectedSessionId = null;
    state.expandedTraces.clear(); state.expandedEvents.clear();
    renderAll();
    toast('已清空');
  });

  // Filters
  on('#globalSearch', 'input', (e) => {
    state.filters.search = e.target.value;
    renderTracePanel();
  });
  on('#filterError', 'change', (e) => {
    state.filters.error = e.target.checked; renderTracePanel();
  });
  on('#filterTool', 'change', (e) => {
    state.filters.tool = e.target.checked; renderTracePanel();
  });
  on('#filterSlow', 'change', (e) => {
    state.filters.slow = e.target.checked; renderTracePanel();
  });
  on('#sessionSearch', 'input', (e) => {
    state.sessionFilter = e.target.value;
    renderSessions();
  });

  // Modal
  on('#modalClose', 'click', hideModal);
  const backdrop = $('.modal-backdrop');
  if (backdrop) backdrop.addEventListener('click', hideModal);
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') hideModal();
  });

  // Drag & drop on drop zone
  const drop = $('#dropZone');
  if (drop) {
    ['dragenter','dragover'].forEach(evt => {
      drop.addEventListener(evt, (e) => {
        e.preventDefault(); e.stopPropagation();
        drop.classList.add('dragover');
      });
    });
    ['dragleave','dragend','drop'].forEach(evt => {
      drop.addEventListener(evt, (e) => {
        e.preventDefault(); e.stopPropagation();
        drop.classList.remove('dragover');
      });
    });
    drop.addEventListener('drop', (e) => {
      const dt = e.dataTransfer;
      if (dt && dt.files && dt.files.length) loadFiles(dt.files);
    });
  }

  // Also allow drop on the app body (after data loaded) to add more files
  document.body.addEventListener('dragover', (e) => {
    if (state.events.length === 0) return;
    e.preventDefault();
  });
  document.body.addEventListener('drop', (e) => {
    if (state.events.length === 0) return;
    e.preventDefault();
    const dt = e.dataTransfer;
    if (dt && dt.files && dt.files.length) loadFiles(dt.files);
  });
}

// ===== Init =====
document.addEventListener('DOMContentLoaded', () => {
  wireEvents();
  renderAll();
});
