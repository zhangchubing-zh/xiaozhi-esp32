#include "siliconflow_provider.h"

#include <esp_log.h>
#include <http.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "../provider.h"
#include "board.h"
#include "provider_common.h"

#define TAG "SF_Llm"

namespace {

class SiliconFlowLlm : public LlmProvider {
public:
    explicit SiliconFlowLlm(const AgentConfig& cfg) : cfg_(cfg) {}

    LlmResponse Chat(const std::string& messages_json,
                     const std::string& tools_json) override {
        LlmResponse resp;
        resp.ok = false;
        std::string body = BuildRequestBody(messages_json, tools_json);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (IsAborted()) return resp;
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
            if (!http) {
                ESP_LOGE(TAG, "CreateHttp failed");
                continue;
            }
            http->SetTimeout(cfg_.llm_timeout_ms);
            http->SetHeader("Authorization", "Bearer " + cfg_.api_key);
            http->SetHeader("Content-Type", "application/json");
            // SetContent+Open sends headers with Content-Length and the body in
            // one shot. Do NOT call Write() after Open (chunked mode).
            http->SetContent(std::string(body));
            if (!http->Open("POST", cfg_.llm_endpoint)) {
                ESP_LOGE(TAG, "HTTP open failed: %d", http->GetLastError());
                continue;
            }
            int status = http->GetStatusCode();
            if (status < 200 || status >= 300) {
                ESP_LOGE(TAG, "LLM HTTP %d", status);
                continue;
            }
            std::string r = ReadFullBody(http.get());
            std::string decoded = DecodeChunkedIfNeeded(r);
            if (ParseChatResponse(decoded, resp)) {
                resp.ok = true;
                return resp;
            }
            ESP_LOGE(TAG, "LLM parse failed, body[0..300]: %.300s", decoded.c_str());
        }
        return resp;
    }

    void Abort() override {
        std::lock_guard<std::mutex> lock(mu_);
        aborted_ = true;
    }

private:
    bool IsAborted() {
        std::lock_guard<std::mutex> lock(mu_);
        return aborted_;
    }

    std::string BuildRequestBody(const std::string& messages_json,
                                 const std::string& tools_json) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", cfg_.llm_model.c_str());
        cJSON* msgs = cJSON_Parse(messages_json.c_str());
        if (msgs) {
            cJSON_AddItemToObject(root, "messages", msgs);
        } else {
            // fallback empty
            cJSON_AddItemToObject(root, "messages", cJSON_CreateArray());
        }
        if (!tools_json.empty() && tools_json != "[]") {
            cJSON* tools = cJSON_Parse(tools_json.c_str());
            if (tools) {
                cJSON_AddItemToObject(root, "tools", tools);
                cJSON_AddStringToObject(root, "tool_choice", "auto");
            }
        }
        cJSON_AddBoolToObject(root, "stream", false);
        char* s = cJSON_PrintUnformatted(root);
        std::string out(s);
        cJSON_free(s);
        cJSON_Delete(root);
        return out;
    }

    static std::string DecodeChunkedIfNeeded(const std::string& resp) {
        if (resp.empty()) return resp;
        size_t crlf = resp.find("\r\n");
        if (crlf == std::string::npos) return resp;
        std::string first_line = resp.substr(0, crlf);
        size_t semi = first_line.find(';');
        if (semi != std::string::npos) first_line = first_line.substr(0, semi);
        char* endp = nullptr;
        unsigned long sz = strtoul(first_line.c_str(), &endp, 16);
        if (endp != first_line.c_str() && sz > 0 && sz <= resp.size()) {
            std::string decoded = DecodeChunked(resp);
            if (!decoded.empty()) return decoded;
        }
        return resp;
    }

    static bool ParseChatResponse(const std::string& body, LlmResponse& resp) {
        cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
        if (!root) return false;
        bool ok = false;
        cJSON* choices = cJSON_GetObjectItem(root, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON* choice = cJSON_GetArrayItem(choices, 0);
            cJSON* message = cJSON_GetObjectItem(choice, "message");
            if (cJSON_IsObject(message)) {
                cJSON* content = cJSON_GetObjectItem(message, "content");
                if (cJSON_IsString(content)) {
                    resp.content = content->valuestring;
                }
                cJSON* tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (cJSON_IsArray(tool_calls)) {
                    int n = cJSON_GetArraySize(tool_calls);
                    for (int i = 0; i < n; ++i) {
                        cJSON* tc = cJSON_GetArrayItem(tool_calls, i);
                        cJSON* id = cJSON_GetObjectItem(tc, "id");
                        cJSON* fn = cJSON_GetObjectItem(tc, "function");
                        if (!cJSON_IsObject(fn)) continue;
                        cJSON* name = cJSON_GetObjectItem(fn, "name");
                        cJSON* args = cJSON_GetObjectItem(fn, "arguments");
                        ToolCall call;
                        call.id = cJSON_IsString(id) ? id->valuestring : "";
                        call.name = cJSON_IsString(name) ? name->valuestring : "";
                        call.arguments = cJSON_IsString(args) ? args->valuestring
                                                              : (cJSON_IsObject(args) ? cJSON_PrintUnformatted(args) : "{}");
                        if (!call.name.empty()) resp.tool_calls.push_back(call);
                    }
                }
                ok = true;
            }
        }
        cJSON_Delete(root);
        return ok;
    }

    AgentConfig cfg_;
    std::mutex mu_;
    bool aborted_ = false;
};

}  // namespace

std::unique_ptr<LlmProvider> MakeSiliconFlowLlm(const AgentConfig& cfg) {
    return std::make_unique<SiliconFlowLlm>(cfg);
}
