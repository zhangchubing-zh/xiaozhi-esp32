#ifndef AGENT_AGENT_H
#define AGENT_AGENT_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "conversation.h"
#include "provider.h"

// Forward declaration: cJSON is a C struct from cJSON.h. We only use it as an
// opaque pointer in callback signatures; concrete agent code includes cJSON.h.
struct cJSON;

class Agent {
public:
    using EmitJsonFn = std::function<void(const std::string& type,
                                          std::function<void(cJSON*)> body_builder)>;
    using EmitAudioFn = std::function<void(std::vector<uint8_t> opus_payload,
                                            uint32_t timestamp)>;
    using IsAbortedFn = std::function<bool()>;

    Agent(AgentConfig cfg,
          EmitJsonFn emit_json,
          EmitAudioFn emit_audio,
          IsAbortedFn is_aborted);
    ~Agent();

    // Called from audio task (high priority). Non-blocking, just buffers.
    void OnListenStart();
    void OnAudioFrame(const uint8_t* opus, size_t len);
    // Triggers a turn: ASR -> LLM tool loop -> TTS. Runs synchronously.
    void OnListenStop();
    void OnAbort();

    // Test helper: directly inject text instead of ASR (e.g. chat_mode).
    void RunTextTurn(const std::string& text);

private:
    void RunTurn(const std::string& asr_text);
    std::string DoAsr();
    std::string DoLlmToolLoop(const std::string& user_text);
    void DoTts(const std::string& text);
    std::string InvokeMcpTool(const std::string& name, const std::string& arguments);
    std::vector<std::string> SplitSentences(const std::string& text);

    AgentConfig cfg_;
    EmitJsonFn emit_json_;
    EmitAudioFn emit_audio_;
    IsAbortedFn is_aborted_;

    std::unique_ptr<AsrProvider> asr_;
    std::unique_ptr<LlmProvider> llm_;
    std::unique_ptr<TtsProvider> tts_;
    Conversation conversation_;
    int next_tool_id_ = 1;
};

// Factory: build the concrete provider implementations for a given config.
std::unique_ptr<AsrProvider> MakeAsrProvider(const AgentConfig& cfg);
std::unique_ptr<LlmProvider> MakeLlmProvider(const AgentConfig& cfg);
std::unique_ptr<TtsProvider> MakeTtsProvider(const AgentConfig& cfg);

#endif  // AGENT_AGENT_H
