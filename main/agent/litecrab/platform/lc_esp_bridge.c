#include "lc_esp_bridge.h"
#include "litecrab/kernel.h"
#include "litecrab/runtime.h"
#include "litecrab/session.h"
#include "litecrab/observability.h"
#include "litecrab/hub.h"
#include "litecrab/working_memory.h"

#include <esp_log.h>
#include <cJSON.h>

#define TAG "LiteCrabBridge"

/* McpServer is C++ — forward declare the accessor we need. */
#ifdef __cplusplus
extern "C" {
#endif
/* Implemented in mcp_server.cc — returns the tool list as OpenAI JSON array string. */
const char* mcp_get_tools_list_json(void);
/* Implemented in mcp_server.cc — calls a tool directly, returns result string. */
const char* mcp_call_tool_directly(const char* name, const char* arguments_json);
#ifdef __cplusplus
}
#endif

/* ---- Callbacks ---- */
static lc_emit_stt_fn g_stt_cb;
static lc_emit_tts_start_fn g_tts_start_cb;
static lc_emit_tts_sentence_fn g_tts_sentence_cb;
static lc_emit_tts_stop_fn g_tts_stop_cb;
static lc_emit_audio_fn g_audio_cb;
static lc_emit_emotion_fn g_emotion_cb;
static lc_is_aborted_fn g_aborted_cb;

/* ---- State ---- */
static int g_initialized = 0;

/* ---- Sentence splitter (reuse from agent.cc) ---- */
static void emit_tts_text(const char* text) {
    if (!text || !*text) return;
    if (g_tts_start_cb) g_tts_start_cb();
    /* Simple sentence split: 。！？!?；;\n */
    const char* start = text;
    const char* p = text;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        int is_sep = 0;
        if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n') {
            is_sep = 1;
        } else if (c == 0xE3 && p[1] == 0x80) {
            /* 。= E3 80 82 */
            if (p[2] == 0x82) is_sep = 1;
        } else if (c == 0xEF && p[1] == 0xBC) {
            /* ！= EF BC 81, ？= EF BC 9F, ；= EF BC 9B */
            if (p[2] == 0x81 || p[2] == 0x9F || p[2] == 0x9B) is_sep = 1;
        }
        if (is_sep) {
            size_t len = (size_t)(p - start) + 1;
            /* Skip multi-byte continuation if applicable */
            if (c >= 0x80) {
                int extra = 1;
                if ((c & 0xE0) == 0xC0) extra = 2;
                else if ((c & 0xF0) == 0xE0) extra = 3;
                len = (size_t)(p - start) + extra;
            }
            if (len > 0 && len < 200) {
                char buf[256];
                if (len < sizeof(buf)) {
                    memcpy(buf, start, len);
                    buf[len] = 0;
                    if (g_tts_sentence_cb) g_tts_sentence_cb(buf);
                    /* TTS audio is handled by the TTS provider in LiteCrab's
                     * gateway layer — here we only emit text for display.
                     * Audio frames would come from tts_sf.c's TtsSfSynthesize
                     * but for now we rely on the existing C++ TTS provider. */
                }
            }
            p = start + len;
            start = p;
            continue;
        }
        p++;
    }
    if (*start && g_tts_sentence_cb) {
        g_tts_sentence_cb(start);
    }
    if (g_tts_stop_cb) g_tts_stop_cb();
}

/* ---- Bridge API ---- */

int lc_esp_init(const char* llm_endpoint,
                const char* llm_api_key,
                const char* llm_model,
                int llm_max_tokens,
                double llm_temperature,
                int llm_stream,
                int llm_timeout_ms,
                const char* asr_endpoint,
                const char* asr_api_key,
                const char* asr_model,
                int asr_timeout_ms,
                int asr_max_audio_seconds,
                const char* asr_stub_text,
                int asr_fallback,
                int tts_enabled,
                const char* tts_endpoint,
                const char* tts_api_key,
                const char* tts_model,
                const char* tts_voice,
                int tts_sample_rate,
                int tts_frame_ms,
                int tts_timeout_ms) {
    (void)tts_enabled; (void)tts_endpoint; (void)tts_api_key;
    (void)tts_model; (void)tts_voice; (void)tts_sample_rate;
    (void)tts_frame_ms; (void)tts_timeout_ms;

    if (g_initialized) return 0;

    /* Initialize LLM config */
    LlmConfig llmCfg = {};
    llmCfg.baseUrl = llm_endpoint;
    llmCfg.apiKey = llm_api_key;
    llmCfg.model = llm_model;
    llmCfg.maxTokens = llm_max_tokens;
    llmCfg.temperature = llm_temperature;
    llmCfg.stream = llm_stream;
    llmCfg.timeoutMs = llm_timeout_ms;
    if (LlmInit(&llmCfg)) {
        ESP_LOGE(TAG, "LlmInit failed");
        return -1;
    }

    /* Initialize runtime */
    CrabRuntimeConfig rtCfg = {};
    rtCfg.workspaceRoot = "/spiffs/litecrab";
    rtCfg.readonlyMode = 0;
    rtCfg.maxResponseBytes = 16384;
    rtCfg.maxRawBytes = 0;
    rtCfg.maxWarnings = 4;
    rtCfg.emitMessage = NULL;
    rtCfg.now = NULL;
    if (CrabRuntimeInit(AgentLoopRuntime(), &rtCfg)) {
        ESP_LOGE(TAG, "CrabRuntimeInit failed");
        return -1;
    }
    CrabRuntimeRegisterBuiltinTools(AgentLoopRuntime());

    /* Load embedded robot skills */
    SkillLoadAllFrom(NULL);

    /* Initialize session store */
    AgentSessionStoreConfigure("/spiffs/litecrab");
    AgentSessionStateInit();

    /* Initialize the agent loop */
    if (AgentLoopInit(&rtCfg, &llmCfg) || AgentLoopStart()) {
        ESP_LOGE(TAG, "AgentLoopInit/Start failed");
        return -1;
    }

    ESP_LOGI(TAG, "LiteCrab bridge initialized: llm=%s model=%s asr=%s",
             llm_endpoint, llm_model, asr_model);
    g_initialized = 1;
    return 0;
}

void lc_esp_set_callbacks(lc_emit_stt_fn stt,
                          lc_emit_tts_start_fn tts_start,
                          lc_emit_tts_sentence_fn tts_sentence,
                          lc_emit_tts_stop_fn tts_stop,
                          lc_emit_audio_fn audio,
                          lc_emit_emotion_fn emotion,
                          lc_is_aborted_fn aborted) {
    g_stt_cb = stt;
    g_tts_start_cb = tts_start;
    g_tts_sentence_cb = tts_sentence;
    g_tts_stop_cb = tts_stop;
    g_audio_cb = audio;
    g_emotion_cb = emotion;
    g_aborted_cb = aborted;
}

void lc_esp_sync_mcp_tools(void) {
    /* Get the device tool list from McpServer and register them into
     * LiteCrab's runtime registry so the LLM can see and call them. */
    const char* toolsJson = mcp_get_tools_list_json();
    if (!toolsJson) {
        ESP_LOGW(TAG, "mcp_get_tools_list_json returned null");
        return;
    }
    /* The tool list is already in OpenAI tools format. LiteCrab's
     * AgentLoopRegisterTool expects CrabToolSpec structs, but the
     * gateway's device tool registration path already handles this
     * via xiaozhi_ws.c's AgentLoopRegisterTool call. Here we just
     * trigger a tools refresh so the LLM sees them. */
    AgentLoopRefreshTools();
    ESP_LOGI(TAG, "MCP tools synced");
}

void lc_esp_listen_start(void) {
    /* The ASR provider's begin is called by the xiaozhi gateway when
     * it receives listen/start. For the bridge, we rely on the gateway
     * layer (xiaozhi_ws.c) to handle the full protocol. */
}

void lc_esp_feed_audio(const uint8_t* opus, size_t len) {
    (void)opus; (void)len;
    /* Audio feeding is handled by the xiaozhi gateway's ASR provider
     * via AsrSfFeedAudio. The bridge doesn't duplicate this. */
}

int lc_esp_run_turn(void) {
    if (!g_initialized) return -1;
    if (g_aborted_cb && g_aborted_cb()) return -1;

    /* Open a session */
    if (AgentSessionStateOpen()) {
        ESP_LOGE(TAG, "AgentSessionStateOpen failed");
        return -1;
    }

    /* The actual turn execution is handled by LiteCrab's kernel run_round,
     * which is called by the xiaozhi gateway when listen/stop arrives.
     * The gateway emits stt/tts messages back through the xiaozhi protocol,
     * which LocalAgentProtocol translates to OnIncomingJson callbacks.
     *
     * For the bridge, we rely on the existing xiaozhi_ws gateway to drive
     * the full turn. This function is a no-op placeholder that returns
     * success when the turn has been initiated by the gateway. */
    AgentSessionStateClose();
    return 0;
}

void lc_esp_abort(void) {
    LlmCancelActiveRequests();
}

void lc_esp_deinit(void) {
    if (!g_initialized) return;
    AgentLoopStop();
    AgentSessionStateClose();
    g_initialized = 0;
}
