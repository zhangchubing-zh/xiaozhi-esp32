#ifndef AGENT_PROVIDER_H
#define AGENT_PROVIDER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AgentConfig {
    std::string asr_provider;   // "siliconflow"
    std::string llm_provider;
    std::string tts_provider;
    std::string asr_endpoint = "https://api.siliconflow.cn/v1/audio/transcriptions";
    std::string llm_endpoint = "https://api.siliconflow.cn/v1/chat/completions";
    std::string tts_endpoint = "https://api.siliconflow.cn/v1/audio/speech";
    std::string api_key;        // SiliconFlow: ASR/LLM/TTS shared key
    std::string llm_model = "Qwen/Qwen3.5-35B-A3B";
    std::string asr_model = "FunAudioLLM/SenseVoiceSmall";
    std::string tts_model = "FunAudioLLM/CosyVoice2-0.5B";
    std::string tts_voice = "alex";
    std::string system_prompt;
    int asr_timeout_ms = 30000;
    int llm_timeout_ms = 60000;
    int tts_timeout_ms = 30000;
    int max_audio_seconds = 60;
    int max_tool_iterations = 5;
    int conversation_history_limit = 10;
};

class AsrProvider {
public:
    virtual ~AsrProvider() = default;
    virtual void Begin(const std::string& session_id) = 0;
    virtual void FeedAudio(const uint8_t* opus, size_t len) = 0;
    // 0=success (text may be empty), 1=fallback, -1=hard failure
    virtual int Finish(std::string& text) = 0;
    virtual void Abort() = 0;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // JSON string
};

struct LlmResponse {
    std::string content;
    std::vector<ToolCall> tool_calls;
    bool ok = false;
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual LlmResponse Chat(const std::string& messages_json,
                             const std::string& tools_json) = 0;
    virtual void Abort() = 0;
};

// Each element is one opus frame (60ms) payload.
class TtsProvider {
public:
    virtual ~TtsProvider() = default;
    virtual bool Synthesize(const std::string& text,
                            std::vector<std::vector<uint8_t>>& opus_frames) = 0;
    virtual void Abort() = 0;
};

#endif  // AGENT_PROVIDER_H
