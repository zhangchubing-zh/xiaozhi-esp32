(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  root.TraceModel = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const KEY_MAP = { session_id: 'sessionId', skill_run_id: 'skillRunId',
    interruption_id: 'interruptionId', correlation_token: 'correlationToken',
    reason_code: 'reasonCode' };
  const ROUTE_ACTIONS = ['none', 'select', 'resume', 'clarify', 'error'];

  function parseStructuredMessage(message) {
    const result = {}, text = String(message || '');
    const re = /([A-Za-z_][A-Za-z0-9_.]*)=("(?:\\.|[^"])*"|[^\s]+)/g;
    let match;
    while ((match = re.exec(text))) {
      let value = match[2];
      if (value.startsWith('"') && value.endsWith('"')) {
        try { value = JSON.parse(value); } catch { value = value.slice(1, -1); }
      }
      result[KEY_MAP[match[1]] || match[1]] = value;
    }
    if (result.confidence != null) result.confidence = Number(result.confidence);
    if (result.action != null && /^-?\d+$/.test(result.action)) result.action = Number(result.action);
    return result;
  }

  function normalizeEvent(input) {
    const event = { ...input };
    if (event.type === 'log' && (event.component === 'skill' || event.component === 'skill_router')) {
      event.structured = parseStructuredMessage(event.message);
      for (const key of ['sessionId', 'skillRunId', 'interruptionId', 'correlationToken'])
        if (!event[key] && event.structured[key]) event[key] = event.structured[key];
      if (!event.skillName && event.structured.skill) event.skillName = event.structured.skill;
    }
    return event;
  }

  function routeActionLabel(action) {
    const index = typeof action === 'number' ? action : Number(action);
    return Number.isInteger(index) && ROUTE_ACTIONS[index] ? ROUTE_ACTIONS[index] : String(action || 'unknown');
  }

  function redactSecrets(value) {
    let text = value == null ? '' : String(value);
    text = text.replace(/("(?:token|apiKey|api_key|password|cookie|authorization)"\s*:\s*")([^"\\]*(?:\\.[^"\\]*)*)(")/gi,
      '$1***REDACTED***$3');
    text = text.replace(/\b(token|api[_-]?key|password|cookie|authorization)=([^\s,;]+)/gi,
      '$1=***REDACTED***');
    text = text.replace(/\bBearer\s+[A-Za-z0-9._~+\/-]+/gi, 'Bearer ***REDACTED***');
    return text;
  }
  return { normalizeEvent, parseStructuredMessage, routeActionLabel, redactSecrets };
});
