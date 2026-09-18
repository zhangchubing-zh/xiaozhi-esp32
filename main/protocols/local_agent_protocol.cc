#include "local_agent_protocol.h"

#include <esp_log.h>
#include <esp_pthread.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

#define TAG "LocalAgent"

/* Stubs for McpServer C++ accessors (called from lc_esp_bridge.c) */
extern "C" {
const char* mcp_get_tools_list_json(void) {
    return McpServer::GetInstance().GetToolsListJson().c_str();
}
const char* mcp_call_tool_directly(const char* name, const char* arguments_json) {
    static std::string result;
    cJSON* args = cJSON_Parse(arguments_json);
    if (!args) args = cJSON_CreateObject();
    result = McpServer::GetInstance().CallToolDirectly(name, args);
    cJSON_Delete(args);
    return result.c_str();
}
}

LocalAgentProtocol::LocalAgentProtocol() {
    agent_events_ = xEventGroupCreate();
    server_sample_rate_ = 24000;
    server_frame_duration_ = 60;
}

LocalAgentProtocol::~LocalAgentProtocol() {
    CloseAudioChannel(false);
    if (agent_events_) {
        vEventGroupDelete(agent_events_);
        agent_events_ = nullptr;
    }
}

bool LocalAgentProtocol::Start() {
    return true;
}

bool LocalAgentProtocol::SendText(const std::string& text) {
    ESP_LOGD(TAG, "SendText (ignored): %s", text.c_str());
    return true;
}

bool LocalAgentProtocol::IsAudioChannelOpened() const {
    return channel_opened_.load();
}

void LocalAgentProtocol::LoadConfigAndInit() {
    Settings s("agent", false);
    std::string provider = s.GetString("provider", "siliconflow");
    std::string api_key = s.GetString("api_key", "");
    cfg_llm_model_ = s.GetString("llm_model", CONFIG_LOCAL_AGENT_DEFAULT_LLM_MODEL);
    cfg_asr_model_ = s.GetString("asr_model", CONFIG_LOCAL_AGENT_DEFAULT_ASR_MODEL);
    cfg_tts_model_ = s.GetString("tts_model", CONFIG_LOCAL_AGENT_DEFAULT_TTS_MODEL);
    cfg_tts_voice_ = s.GetString("tts_voice", "FunAudioLLM/CosyVoice2-0.5B:alex");
    cfg_llm_endpoint_ = s.GetString("llm_endpoint", "https://api.siliconflow.cn/v1/chat/completions");
    cfg_asr_endpoint_ = s.GetString("asr_endpoint", "https://api.siliconflow.cn/v1/audio/transcriptions");
    cfg_tts_endpoint_ = s.GetString("tts_endpoint", "https://api.siliconflow.cn/v1/audio/speech");
    cfg_asr_timeout_ms_ = s.GetInt("asr_timeout_ms", 90000);
    cfg_llm_timeout_ms_ = s.GetInt("llm_timeout_ms", CONFIG_LOCAL_AGENT_LLM_TIMEOUT_MS);
    cfg_tts_timeout_ms_ = s.GetInt("tts_timeout_ms", CONFIG_LOCAL_AGENT_TTS_TIMEOUT_MS);
    cfg_asr_max_audio_seconds_ = s.GetInt("max_audio_seconds", CONFIG_LOCAL_AGENT_MAX_AUDIO_SECONDS);
    if (api_key.empty()) {
        api_key = "sk-rgdwxvpekwdcrscrfzizqlajntczvdvpplfljpkhffmtlijw";
    }
    cfg_llm_api_key_ = api_key;
    cfg_asr_api_key_ = api_key;
    cfg_tts_api_key_ = api_key;
    cfg_asr_stub_ = s.GetString("stub_text", "你好");

    /* Set up callbacks */
    lc_esp_set_callbacks(
        [](const char* text) {  /* stt */
            /* Will be emitted by LocalAgentProtocol instance via Schedule */
        },
        []() { /* tts_start */ },
        [](const char* text) { /* tts_sentence */ },
        []() { /* tts_stop */ },
        [](const uint8_t* data, size_t len, uint32_t ts) { /* audio */ },
        [](const char* emotion) { /* emotion */ },
        []() -> bool { return false; }
    );

    /* Initialize LiteCrab */
    lc_esp_init(
        cfg_llm_endpoint_.c_str(), cfg_llm_api_key_.c_str(), cfg_llm_model_.c_str(),
        cfg_llm_max_tokens_, cfg_llm_temperature_, cfg_llm_stream_, cfg_llm_timeout_ms_,
        cfg_asr_endpoint_.c_str(), cfg_asr_api_key_.c_str(), cfg_asr_model_.c_str(),
        cfg_asr_timeout_ms_, cfg_asr_max_audio_seconds_, cfg_asr_stub_.c_str(), cfg_asr_fallback_,
        cfg_tts_enabled_, cfg_tts_endpoint_.c_str(), cfg_tts_api_key_.c_str(),
        cfg_tts_model_.c_str(), cfg_tts_voice_.c_str(), cfg_tts_sample_rate_,
        cfg_tts_frame_ms_, cfg_tts_timeout_ms_);

    /* Sync device MCP tools */
    lc_esp_sync_mcp_tools();
}

bool LocalAgentProtocol::OpenAudioChannel() {
    if (agent_thread_.joinable()) {
        ESP_LOGI(TAG, "Local-agent channel already open, reusing");
        aborted_.store(false);
        if (on_audio_channel_opened_) {
            on_audio_channel_opened_();
        }
        return true;
    }

    LoadConfigAndInit();
    aborted_.store(false);
    channel_opened_.store(true);

    session_id_ = "local-" + SystemInfo::GetMacAddress();
    std::replace(session_id_.begin(), session_id_.end(), ':', '-');

    esp_pthread_cfg_t thread_cfg = esp_pthread_get_default_config();
    thread_cfg.stack_size = 16 * 1024;
    thread_cfg.prio = 4;
    thread_cfg.thread_name = "agent_task";
    thread_cfg.inherit_cfg = false;
    esp_pthread_set_cfg(&thread_cfg);
    agent_thread_ = std::thread([this]() { AgentTask(); });
    esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&default_cfg);

    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void LocalAgentProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    if (!channel_opened_.exchange(false)) return;
    aborted_.store(true);
    lc_esp_abort();
    if (agent_events_) {
        xEventGroupSetBits(agent_events_, kEventAbort | kEventStopThread);
    }
    if (agent_thread_.joinable()) {
        agent_thread_.join();
    }
    lc_esp_deinit();
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool LocalAgentProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!channel_opened_.load()) return false;
    lc_esp_feed_audio(packet->payload.data(), packet->payload.size());
    return true;
}

void LocalAgentProtocol::SendWakeWordDetected(const std::string& wake_word) {
    (void)wake_word;
    aborted_.store(false);
    if (agent_events_) {
        xEventGroupSetBits(agent_events_, kEventListenStart);
    }
}

void LocalAgentProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    aborted_.store(false);
    if (agent_events_) {
        xEventGroupSetBits(agent_events_, kEventListenStart);
    }
}

void LocalAgentProtocol::SendStopListening() {
    if (agent_events_) {
        xEventGroupSetBits(agent_events_, kEventListenStop);
    }
}

void LocalAgentProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    aborted_.store(true);
    lc_esp_abort();
    if (agent_events_) {
        xEventGroupSetBits(agent_events_, kEventAbort);
    }
}

bool LocalAgentProtocol::IsAborted() {
    return aborted_.load();
}

void LocalAgentProtocol::EmitJson(const std::string& type,
                                   std::function<void(cJSON*)> body_builder) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", type.c_str());
    if (body_builder) body_builder(root);
    auto& app = Application::GetInstance();
    app.Schedule([this, root]() {
        if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
    });
}

void LocalAgentProtocol::EmitAudio(std::vector<uint8_t> opus_payload, uint32_t timestamp) {
    if (opus_payload.empty()) return;
    auto packet = std::make_shared<AudioStreamPacket>();
    packet->sample_rate = server_sample_rate_;
    packet->frame_duration = server_frame_duration_;
    packet->timestamp = timestamp;
    packet->payload = std::move(opus_payload);
    auto& app = Application::GetInstance();
    app.Schedule([this, packet]() {
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::make_unique<AudioStreamPacket>(*packet));
        }
    });
}

void LocalAgentProtocol::AgentTask() {
    ESP_LOGI(TAG, "Agent task started");
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            agent_events_,
            kEventListenStart | kEventListenStop | kEventAbort | kEventStopThread,
            pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & kEventStopThread) {
            ESP_LOGI(TAG, "Agent task: stop requested, exiting");
            break;
        }
        if (bits & kEventAbort) {
            ESP_LOGI(TAG, "Agent task: abort");
            lc_esp_abort();
        }
        if (bits & kEventListenStart) {
            lc_esp_listen_start();
        } else if (bits & kEventListenStop) {
            if ((bits & kEventAbort)) {
                ESP_LOGI(TAG, "Agent task: skipping aborted turn");
                continue;
            }
            try {
                lc_esp_run_turn();
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Agent turn exception: %s", e.what());
                EmitJson("tts", [](cJSON* root) { cJSON_AddStringToObject(root, "state", "start"); });
                EmitJson("tts", [](cJSON* root) {
                    cJSON_AddStringToObject(root, "state", "sentence_start");
                    cJSON_AddStringToObject(root, "text", "抱歉，处理出错了。");
                });
                EmitJson("tts", [](cJSON* root) { cJSON_AddStringToObject(root, "state", "stop"); });
            }
        }
    }
    ESP_LOGI(TAG, "Agent task exiting");
}
