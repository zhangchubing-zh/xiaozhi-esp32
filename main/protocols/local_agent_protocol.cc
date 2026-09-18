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
#include "settings.h"
#include "system_info.h"

#define TAG "LocalAgent"

LocalAgentProtocol::LocalAgentProtocol() {
    agent_events_ = xEventGroupCreate();
    server_sample_rate_ = 24000;  // TTS provider output sample rate
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
    // Nothing to do until OpenAudioChannel is called.
    return true;
}

bool LocalAgentProtocol::SendText(const std::string& text) {
    // We don't send text anywhere - the agent loop is driven by SendAudio +
    // SendStartListening/SendStopListening/SendAbortSpeaking overrides below.
    ESP_LOGD(TAG, "SendText (ignored): %s", text.c_str());
    return true;
}

bool LocalAgentProtocol::IsAudioChannelOpened() const {
    // Note: a per-turn abort (wake word interruption) must NOT invalidate the
    // channel. Keeping it open lets the next wake reuse the agent task instead
    // of re-opening (which previously freed a live Agent -> use-after-free).
    return channel_opened_.load();
}

AgentConfig LocalAgentProtocol::LoadConfig() {
    AgentConfig cfg;
    Settings s("agent", false);
    cfg.asr_provider = s.GetString("provider", "siliconflow");
    cfg.llm_provider = s.GetString("provider", "siliconflow");
    cfg.tts_provider = s.GetString("provider", "siliconflow");
    cfg.api_key = s.GetString("api_key", "");
    cfg.llm_model = s.GetString("llm_model", CONFIG_LOCAL_AGENT_DEFAULT_LLM_MODEL);
    cfg.asr_model = s.GetString("asr_model", CONFIG_LOCAL_AGENT_DEFAULT_ASR_MODEL);
    cfg.tts_model = s.GetString("tts_model", CONFIG_LOCAL_AGENT_DEFAULT_TTS_MODEL);
    // The voice must be the full "<model>:<voice>" form (e.g.
    // "FunAudioLLM/CosyVoice2-0.5B:alex"); a bare voice name gets HTTP 400.
    // Hardcode the verified default rather than trusting a possibly-stale
    // Kconfig-derived sdkconfig value.
    cfg.tts_voice = s.GetString("tts_voice", "FunAudioLLM/CosyVoice2-0.5B:alex");
    cfg.system_prompt = s.GetString("system_prompt", "");
    cfg.asr_endpoint = s.GetString("asr_endpoint",
                                    "https://api.siliconflow.cn/v1/audio/transcriptions");
    cfg.llm_endpoint = s.GetString("llm_endpoint",
                                    "https://api.siliconflow.cn/v1/chat/completions");
    cfg.tts_endpoint = s.GetString("tts_endpoint",
                                    "https://api.siliconflow.cn/v1/audio/speech");
    // ASR free tier can queue 20-55s (measured with LiteCrab), so default the
    // timeout to 90s regardless of the Kconfig default.
    cfg.asr_timeout_ms = s.GetInt("asr_timeout_ms", 90000);
    cfg.llm_timeout_ms = s.GetInt("llm_timeout_ms", CONFIG_LOCAL_AGENT_LLM_TIMEOUT_MS);
    cfg.tts_timeout_ms = s.GetInt("tts_timeout_ms", CONFIG_LOCAL_AGENT_TTS_TIMEOUT_MS);
    cfg.max_audio_seconds = s.GetInt("max_audio_seconds", CONFIG_LOCAL_AGENT_MAX_AUDIO_SECONDS);
    cfg.max_tool_iterations = s.GetInt("max_tool_iterations", CONFIG_LOCAL_AGENT_MAX_TOOL_ITERATIONS);
    cfg.conversation_history_limit = s.GetInt("conversation_history_limit",
                                               CONFIG_LOCAL_AGENT_CONVERSATION_HISTORY_LIMIT);
    // TODO: remove this hardcoded key once NVS-based provisioning is in place.
    // Inlined for first-pass end-to-end testing only.
    if (cfg.api_key.empty()) {
        cfg.api_key = "sk-rgdwxvpekwdcrscrfzizqlajntczvdvpplfljpkhffmtlijw";
    }
    return cfg;
}

bool LocalAgentProtocol::OpenAudioChannel() {
    // The app may call OpenAudioChannel again after an aborted turn (wake word
    // interruption sets aborted_). In that case the agent task is still alive:
    // reuse it instead of creating a second Agent/thread. Replacing agent_ while
    // the old thread runs would free the old Agent (and its mutexes) out from
    // under the running thread -> use-after-free crash.
    if (agent_thread_.joinable()) {
        ESP_LOGI(TAG, "Local-agent channel already open, reusing");
        aborted_.store(false);
        if (on_audio_channel_opened_) {
            on_audio_channel_opened_();
        }
        return true;
    }

    cfg_ = LoadConfig();
    if (cfg_.api_key.empty()) {
        ESP_LOGE(TAG, "agent/api_key not configured");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    ESP_LOGI(TAG, "Opening local-agent channel: provider=%s llm=%s asr=%s tts=%s",
             cfg_.asr_provider.c_str(), cfg_.llm_model.c_str(),
             cfg_.asr_model.c_str(), cfg_.tts_model.c_str());

    aborted_.store(false);
    channel_opened_.store(true);

    // Derive a stable session id from the device MAC.
    session_id_ = "local-" + SystemInfo::GetMacAddress();
    std::replace(session_id_.begin(), session_id_.end(), ':', '-');

    // Construct the agent with emit helpers that route through Application::Schedule.
    auto emit_json = [this](const std::string& type, std::function<void(cJSON*)> body_builder) {
        EmitJson(type, std::move(body_builder));
    };
    auto emit_audio = [this](std::vector<uint8_t> opus, uint32_t timestamp) {
        EmitAudio(std::move(opus), timestamp);
    };
    auto is_aborted = [this]() { return IsAborted(); };
    agent_ = std::make_unique<Agent>(cfg_, emit_json, emit_audio, is_aborted);

    // Configure the agent thread BEFORE creating it. esp_pthread_set_cfg only
    // affects threads created by the *calling* task, so it must run here (main
    // task), not inside the thread body. The default pthread stack (3072 B) is
    // far too small: the agent turn performs mbedTLS handshakes which need
    // several KB of stack, and overflowing it corrupts adjacent heap blocks.
    esp_pthread_cfg_t thread_cfg = esp_pthread_get_default_config();
    thread_cfg.stack_size = 16 * 1024;
    thread_cfg.prio = 4;  // below audio tasks
    thread_cfg.thread_name = "agent_task";
    thread_cfg.inherit_cfg = false;
    esp_pthread_set_cfg(&thread_cfg);
    agent_thread_ = std::thread([this]() { AgentTask(); });
    // Restore the default pthread config so any other threads later created by
    // this task (e.g. camera encoder threads) keep their expected defaults.
    esp_pthread_cfg_t default_cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&default_cfg);

    // Notify Application that the channel is open (so it enters listening state).
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void LocalAgentProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    if (!channel_opened_.exchange(false)) return;
    aborted_.store(true);
    if (agent_) agent_->OnAbort();
    if (agent_events_) {
        xEventGroupSetBits(agent_events_, kEventAbort | kEventStopThread);
    }
    if (agent_thread_.joinable()) {
        agent_thread_.join();
    }
    agent_.reset();
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool LocalAgentProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!channel_opened_.load() || !agent_) return false;
    agent_->OnAudioFrame(packet->payload.data(), packet->payload.size());
    return true;
}

void LocalAgentProtocol::SendWakeWordDetected(const std::string& wake_word) {
    (void)wake_word;
    // A new turn begins: clear any stale abort flag from a previous turn and
    // arm the ASR buffer via the agent task (never call into the agent
    // directly from this task).
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
    if (agent_) agent_->OnAbort();
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
    // Route to Application via Schedule so UI/state mutations are serialized on the main task.
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
    // std::function requires copyable targets; wrap the shared packet (copyable)
    // instead of moving a unique_ptr into the lambda capture.
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
        // NOTE: xEventGroupWaitBits(xClearOnExit=pdTRUE) clears ALL retrieved
        // bits at once, so every set bit must be handled in this iteration --
        // an early `continue` would silently drop the others (e.g. a wake word
        // during listening raises ABORT and LISTEN_START together).
        if (bits & kEventStopThread) {
            ESP_LOGI(TAG, "Agent task: stop requested, exiting");
            break;
        }
        if (bits & kEventAbort) {
            ESP_LOGI(TAG, "Agent task: abort");
            if (agent_) agent_->OnAbort();
        }
        if (bits & kEventListenStart) {
            if (agent_) agent_->OnListenStart();
        } else if (bits & kEventListenStop) {
            // Skip a turn that was aborted in this same batch (wake word
            // interruption) -- its audio is stale.
            if ((bits & kEventAbort) || !agent_) {
                ESP_LOGI(TAG, "Agent task: skipping aborted turn");
                continue;
            }
            // Run the full turn synchronously. Abort requests arriving during
            // the turn are observed via the is_aborted_ callback.
            try {
                agent_->OnListenStop();
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
