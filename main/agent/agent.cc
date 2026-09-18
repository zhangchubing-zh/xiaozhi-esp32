#include "agent.h"

#include <esp_log.h>
#include <esp_pthread.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstring>
#include <ctime>
#include <sstream>
#include <thread>

#include <cJSON.h>

#include "mcp_server.h"
#include "providers/provider_common.h"
#include "providers/siliconflow_provider.h"

#define TAG "Agent"

namespace {
constexpr const char* kDefaultSystemPrompt =
    "你是一个运行在机器人上的语音助手。你可以通过工具控制机器人的动作（走路、转身等）和设备状态（音量、亮度等）。"
    "当用户要求执行动作时，先调用对应工具，再根据工具结果回答用户。"
    "回答简洁，使用中文，每句不超过60字。不要透露你的内部实现。";
}

Agent::Agent(AgentConfig cfg, EmitJsonFn emit_json, EmitAudioFn emit_audio, IsAbortedFn is_aborted)
    : cfg_(std::move(cfg)),
      emit_json_(std::move(emit_json)),
      emit_audio_(std::move(emit_audio)),
      is_aborted_(std::move(is_aborted)),
      conversation_(cfg_.conversation_history_limit) {
    if (cfg_.system_prompt.empty()) {
        cfg_.system_prompt = kDefaultSystemPrompt;
    }
    asr_ = MakeAsrProvider(cfg_);
    llm_ = MakeLlmProvider(cfg_);
    tts_ = MakeTtsProvider(cfg_);
    if (!asr_ || !llm_ || !tts_) {
        ESP_LOGE(TAG, "Failed to construct one or more providers");
    }
}

Agent::~Agent() = default;

void Agent::OnListenStart() {
    if (asr_) asr_->Begin("");
}

void Agent::OnAudioFrame(const uint8_t* opus, size_t len) {
    if (asr_) asr_->FeedAudio(opus, len);
}

void Agent::OnListenStop() {
    std::string asr_text = DoAsr();
    if (asr_text.empty()) {
        ESP_LOGI(TAG, "ASR returned empty text (user didn't speak), skipping turn");
        return;
    }
    RunTurn(asr_text);
}

void Agent::OnAbort() {
    if (asr_) asr_->Abort();
    if (llm_) llm_->Abort();
    if (tts_) tts_->Abort();
}

void Agent::RunTextTurn(const std::string& text) {
    if (text.empty()) return;
    RunTurn(text);
}

void Agent::RunTurn(const std::string& asr_text) {
    // 1. Emit stt (display user text)
    emit_json_("stt", [asr_text](cJSON* root) {
        cJSON_AddStringToObject(root, "text", asr_text.c_str());
    });
    // 2. Emit llm emotion
    emit_json_("llm", [](cJSON* root) {
        cJSON_AddStringToObject(root, "emotion", "happy");
        cJSON_AddStringToObject(root, "text", "");
    });
    // 3. LLM tool loop
    std::string reply = DoLlmToolLoop(asr_text);
    if (is_aborted_()) return;
    if (reply.empty()) {
        reply = "抱歉，处理出错了，请稍后再试。";
    }
    // 4. TTS
    DoTts(reply);
}

std::string Agent::DoAsr() {
    if (!asr_) return "";
    std::string text;
    int rc = asr_->Finish(text);
    ESP_LOGI(TAG, "ASR rc=%d text='%.80s'", rc, text.c_str());
    if (rc < 0) {
        // Hard failure - emit apology
        emit_json_("tts", [](cJSON* root) { cJSON_AddStringToObject(root, "state", "start"); });
        emit_json_("tts", [](cJSON* root) {
            cJSON_AddStringToObject(root, "state", "sentence_start");
            cJSON_AddStringToObject(root, "text", "抱歉，没听清，请再说一遍。");
        });
        emit_json_("tts", [](cJSON* root) { cJSON_AddStringToObject(root, "state", "stop"); });
        return "";
    }
    return text;
}

std::string Agent::DoLlmToolLoop(const std::string& user_text) {
    if (!llm_) return "抱歉，处理出错了。";
    conversation_.AddUser(user_text);
    std::string tools_json = McpServer::GetInstance().GetToolsListJson();
    std::string messages_json = conversation_.ToJson(cfg_.system_prompt);

    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        if (is_aborted_()) return "";
        // Re-serialize messages each iteration (history may have grown)
        messages_json = conversation_.ToJson(cfg_.system_prompt);
        LlmResponse resp = llm_->Chat(messages_json, tools_json);
        if (!resp.ok) {
            ESP_LOGE(TAG, "LLM Chat failed at iter %d", iter);
            return "";
        }
        if (resp.tool_calls.empty()) {
            conversation_.AddAssistant(resp.content, {});
            return resp.content;
        }
        // Has tool calls - record assistant message and execute each
        conversation_.AddAssistant(resp.content, resp.tool_calls);
        for (const auto& tc : resp.tool_calls) {
            if (is_aborted_()) return "";
            ESP_LOGI(TAG, "Invoking tool %s args=%.200s", tc.name.c_str(), tc.arguments.c_str());
            std::string result = InvokeMcpTool(tc.name, tc.arguments);
            conversation_.AddToolResult(tc.id, result);
        }
    }
    return "抱歉，处理步骤过多，请简化问题。";
}

std::string Agent::InvokeMcpTool(const std::string& name, const std::string& arguments) {
    cJSON* args = cJSON_Parse(arguments.c_str());
    if (!args) args = cJSON_CreateObject();
    std::string result = McpServer::GetInstance().CallToolDirectly(name, args);
    cJSON_Delete(args);
    // Extract the text content from the JSON-RPC result for LLM consumption.
    // Result shape: {"content":[{"type":"text","text":"..."}],"isError":false}
    // or {"error":{"code":..,"message":".."}}
    cJSON* root = cJSON_Parse(result.c_str());
    if (!root) return result;
    std::string out;
    cJSON* err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(err)) {
        cJSON* msg = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(msg)) out = std::string("工具执行失败: ") + msg->valuestring;
        else out = "工具执行失败";
        cJSON_Delete(root);
        return out;
    }
    cJSON* content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsArray(content)) {
        int n = cJSON_GetArraySize(content);
        for (int i = 0; i < n; ++i) {
            cJSON* item = cJSON_GetArrayItem(content, i);
            cJSON* text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(text)) {
                if (!out.empty()) out += "\n";
                out += text->valuestring;
            }
        }
    }
    if (out.empty()) out = result;
    cJSON_Delete(root);
    return out;
}

void Agent::DoTts(const std::string& text) {
    if (!tts_) return;
    auto sentences = SplitSentences(text);
    if (sentences.empty()) sentences.push_back(text);

    emit_json_("tts", [](cJSON* root) { cJSON_AddStringToObject(root, "state", "start"); });

    for (const auto& s : sentences) {
        if (is_aborted_()) break;
        // Emit sentence_start (screen display)
        emit_json_("tts", [&s](cJSON* root) {
            cJSON_AddStringToObject(root, "state", "sentence_start");
            cJSON_AddStringToObject(root, "text", s.c_str());
        });
        if (is_aborted_()) break;
        // Synthesize and emit audio frames
        std::vector<std::vector<uint8_t>> opus_frames;
        if (tts_->Synthesize(s, opus_frames)) {
            uint32_t ts = 0;
            for (auto& f : opus_frames) {
                if (is_aborted_()) break;
                emit_audio_(std::move(f), ts);
                ts += 60;  // 60ms per frame
            }
        } else {
            ESP_LOGW(TAG, "TTS failed for sentence, skipping audio: %.80s", s.c_str());
        }
    }

    emit_json_("tts", [](cJSON* root) { cJSON_AddStringToObject(root, "state", "stop"); });
}

std::vector<std::string> Agent::SplitSentences(const std::string& text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            // Trim whitespace
            size_t b = 0, e = cur.size();
            while (b < e && (cur[b] == ' ' || cur[b] == '\t' || cur[b] == '\r' || cur[b] == '\n')) ++b;
            while (e > b && (cur[e - 1] == ' ' || cur[e - 1] == '\t' || cur[e - 1] == '\r' || cur[e - 1] == '\n')) --e;
            if (b < e) out.push_back(cur.substr(b, e - b));
            cur.clear();
        }
    };

    const size_t kMaxLen = 120;  // bytes; UTF-8 safe boundary at codepoint end
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        cur += c;
        bool is_sep = (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n');
        // CJK punctuation is 3-byte UTF-8. Detect by leading byte sequences:
        // 。= E3 80 82, ！= EF BC 81, ？= EF BC 9F, ；= EF BC 9B
        if (!is_sep && i + 2 < text.size()) {
            unsigned char u0 = (unsigned char)text[i];
            unsigned char u1 = (unsigned char)text[i + 1];
            unsigned char u2 = (unsigned char)text[i + 2];
            if (u0 == 0xE3 && u1 == 0x80 && (u2 == 0x82)) {  // 。
                cur += text.substr(i + 1, 2);
                i += 2;
                is_sep = true;
            } else if (u0 == 0xEF && u1 == 0xBC && (u2 == 0x81 || u2 == 0x9F || u2 == 0x9B)) {
                // ！ ？ ；
                cur += text.substr(i + 1, 2);
                i += 2;
                is_sep = true;
            }
        }
        if (is_sep) {
            flush();
            continue;
        }
        // Hard cut at kMaxLen (UTF-8 safe: don't cut mid-codepoint)
        if (cur.size() >= kMaxLen) {
            // Walk back to last UTF-8 boundary
            size_t cut = cur.size();
            while (cut > 0) {
                unsigned char uc = (unsigned char)cur[cut - 1];
                if ((uc & 0xC0) != 0x80) break;  // start byte
                --cut;
            }
            if (cut == 0) cut = cur.size();  // all continuation bytes? shouldn't happen
            std::string kept = cur.substr(0, cut);
            cur.erase(0, cut);
            if (!kept.empty()) {
                size_t b = 0, e = kept.size();
                while (b < e && kept[b] == ' ') ++b;
                while (e > b && kept[e - 1] == ' ') --e;
                if (b < e) out.push_back(kept.substr(b, e - b));
            }
        }
    }
    flush();
    return out;
}

// Provider factories: route by cfg.asr_provider/llm_provider/tts_provider.
std::unique_ptr<AsrProvider> MakeAsrProvider(const AgentConfig& cfg) {
    if (cfg.asr_provider == "siliconflow" || cfg.asr_provider.empty()) {
        return MakeSiliconFlowAsr(cfg);
    }
    ESP_LOGE(TAG, "Unknown asr_provider '%s', falling back to siliconflow", cfg.asr_provider.c_str());
    return MakeSiliconFlowAsr(cfg);
}

std::unique_ptr<LlmProvider> MakeLlmProvider(const AgentConfig& cfg) {
    if (cfg.llm_provider == "siliconflow" || cfg.llm_provider.empty()) {
        return MakeSiliconFlowLlm(cfg);
    }
    ESP_LOGE(TAG, "Unknown llm_provider '%s', falling back to siliconflow", cfg.llm_provider.c_str());
    return MakeSiliconFlowLlm(cfg);
}

std::unique_ptr<TtsProvider> MakeTtsProvider(const AgentConfig& cfg) {
    if (cfg.tts_provider == "siliconflow" || cfg.tts_provider.empty()) {
        return MakeSiliconFlowTts(cfg);
    }
    ESP_LOGE(TAG, "Unknown tts_provider '%s', falling back to siliconflow", cfg.tts_provider.c_str());
    return MakeSiliconFlowTts(cfg);
}
