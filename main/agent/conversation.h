#ifndef AGENT_CONVERSATION_H
#define AGENT_CONVERSATION_H

#include <string>
#include <vector>

#include "provider.h"

class Conversation {
public:
    explicit Conversation(int limit) : limit_(limit) {}

    void AddUser(const std::string& text);
    void AddAssistant(const std::string& content, const std::vector<ToolCall>& calls);
    void AddToolResult(const std::string& tool_call_id, const std::string& result);
    void Clear();

    // Serialize to OpenAI messages JSON array string (including system prompt prepended).
    std::string ToJson(const std::string& system_prompt) const;

private:
    void Trim();
    struct Msg {
        std::string role;          // user | assistant | tool
        std::string content;
        std::string tool_call_id;   // role=="tool"
        std::string tool_calls;     // role=="assistant" with calls, JSON array string
    };
    std::vector<Msg> msgs_;
    int limit_;  // rounds (user+assistant pairs) to retain
};

#endif  // AGENT_CONVERSATION_H
