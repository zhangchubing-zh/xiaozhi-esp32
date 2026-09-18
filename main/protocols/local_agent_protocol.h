#ifndef LOCAL_AGENT_PROTOCOL_H
#define LOCAL_AGENT_PROTOCOL_H

#include "protocol.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <memory>
#include <thread>

#include "agent/agent.h"

// Implements the Protocol interface by running an on-device agent loop that
// directly HTTPS-connects to cloud ASR/LLM/TTS providers. To Application, the
// LocalAgentProtocol looks indistinguishable from a remote server: it feeds
// `OnIncomingJson(stt/llm/tts/mcp)` and `OnIncomingAudio(opus)` events.
//
// Configuration is read from NVS namespace "agent" once at OpenAudioChannel.
class LocalAgentProtocol : public Protocol {
public:
    LocalAgentProtocol();
    ~LocalAgentProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    // Override the protocol-text-based control messages: route them to agent
    // events instead of sending JSON over the wire.
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;

private:
    // Event bits for the agent task
    static constexpr EventBits_t kEventListenStart = 1 << 0;
    static constexpr EventBits_t kEventListenStop  = 1 << 1;
    static constexpr EventBits_t kEventAbort       = 1 << 2;
    static constexpr EventBits_t kEventStopThread  = 1 << 3;

    bool SendText(const std::string& text) override;

    // Agent task entry
    void AgentTask();
    // Emit helpers: build cJSON, fire callback on the protocol's main thread
    // via Application::Schedule. cJSON is created and consumed by the scheduled
    // callback which owns (deletes) it.
    void EmitJson(const std::string& type, std::function<void(cJSON*)> body_builder);
    void EmitAudio(std::vector<uint8_t> opus_payload, uint32_t timestamp);
    bool IsAborted();

    // Configuration loader
    AgentConfig LoadConfig();

    // Agent task sync primitives
    EventGroupHandle_t agent_events_ = nullptr;
    std::unique_ptr<Agent> agent_;
    std::thread agent_thread_;
    std::atomic<bool> channel_opened_{false};
    std::atomic<bool> aborted_{false};
    AgentConfig cfg_;
};

#endif  // LOCAL_AGENT_PROTOCOL_H
