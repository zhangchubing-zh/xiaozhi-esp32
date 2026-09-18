#include "conversation.h"

#include <cJSON.h>

void Conversation::AddUser(const std::string& text) {
    Msg m;
    m.role = "user";
    m.content = text;
    msgs_.push_back(std::move(m));
    Trim();
}

void Conversation::AddAssistant(const std::string& content,
                               const std::vector<ToolCall>& calls) {
    Msg m;
    m.role = "assistant";
    m.content = content;
    if (!calls.empty()) {
        // Build tool_calls JSON array
        cJSON* arr = cJSON_CreateArray();
        for (const auto& c : calls) {
            cJSON* tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", c.id.c_str());
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON* fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", c.name.c_str());
            // arguments is a JSON string per OpenAI spec
            cJSON_AddStringToObject(fn, "arguments", c.arguments.c_str());
            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(arr, tc);
        }
        char* s = cJSON_PrintUnformatted(arr);
        m.tool_calls = s;
        cJSON_free(s);
        cJSON_Delete(arr);
    }
    msgs_.push_back(std::move(m));
    Trim();
}

void Conversation::AddToolResult(const std::string& tool_call_id,
                                  const std::string& result) {
    Msg m;
    m.role = "tool";
    m.tool_call_id = tool_call_id;
    m.content = result;
    msgs_.push_back(std::move(m));
    Trim();
}

void Conversation::Clear() {
    msgs_.clear();
}

static void AppendJsonString(std::string& out, const char* field, const std::string& val) {
    cJSON* tmp = cJSON_CreateString(val.c_str());
    char* s = cJSON_PrintUnformatted(tmp);
    out += "\"" + std::string(field) + "\":";
    out += s;
    out += ",";
    cJSON_free(s);
    cJSON_Delete(tmp);
}

std::string Conversation::ToJson(const std::string& system_prompt) const {
    std::string out = "[";
    // System message first
    out += "{\"role\":\"system\",\"content\":";
    cJSON* sys = cJSON_CreateString(system_prompt.c_str());
    char* sys_s = cJSON_PrintUnformatted(sys);
    out += sys_s;
    out += "},";
    cJSON_free(sys_s);
    cJSON_Delete(sys);
    for (const auto& m : msgs_) {
        out += "{\"role\":\"";
        out += m.role;
        out += "\",";
        if (m.role == "tool") {
            // tool message: include tool_call_id
            AppendJsonString(out, "tool_call_id", m.tool_call_id);
            AppendJsonString(out, "content", m.content);
        } else {
            AppendJsonString(out, "content", m.content);
            if (!m.tool_calls.empty()) {
                out += "\"tool_calls\":";
                out += m.tool_calls;
                out += ",";
            }
        }
        if (out.back() == ',') out.pop_back();
        out += "},";
    }
    if (out.back() == ',') out.pop_back();
    out += "]";
    return out;
}

void Conversation::Trim() {
    // Keep last `limit_` user+assistant rounds. A round is user->assistant pair.
    // Simplest: count user messages; if more than limit_ + 1, drop oldest user/assistant/tool cluster.
    if (limit_ <= 0) return;
    int user_count = 0;
    for (const auto& m : msgs_) if (m.role == "user") ++user_count;
    while (user_count > limit_ && !msgs_.empty()) {
        // Drop from front until first user is removed (and preceding tool results handled)
        // We drop the first message; if it's an assistant with tool_calls, also drop trailing tool results.
        msgs_.erase(msgs_.begin());
        // If we just dropped a user, decrement count
        // Recompute (cheap, list is small)
        user_count = 0;
        for (const auto& m : msgs_) if (m.role == "user") ++user_count;
    }
}
