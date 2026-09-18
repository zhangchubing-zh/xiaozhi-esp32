#ifndef LITECRAB_ESP_BRIDGE_H
#define LITECRAB_ESP_BRIDGE_H

/*
 * Bridge between LiteCrab C agent runtime and the ESP32 firmware.
 *
 * LocalAgentProtocol (C++) calls these C functions to drive the LiteCrab
 * agent loop, replacing the old main/agent C++ Agent class.
 *
 * The bridge handles:
 *   - Initializing LiteCrab runtime with config from NVS
 *   - Feeding audio frames to the ASR provider
 *   - Running a turn (ASR → LLM tool loop → TTS) via the LiteCrab kernel
 *   - Emitting results back to LocalAgentProtocol via callbacks
 *   - Synchronizing device MCP tools into LiteCrab's tool registry
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize LiteCrab agent with the given configuration strings.
 * Returns 0 on success. Must be called once before any other bridge API. */
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
                int tts_timeout_ms);

/* Callback types for emitting results back to ESP32 firmware. */
typedef void (*lc_emit_stt_fn)(const char* text);
typedef void (*lc_emit_tts_start_fn)(void);
typedef void (*lc_emit_tts_sentence_fn)(const char* text);
typedef void (*lc_emit_tts_stop_fn)(void);
typedef void (*lc_emit_audio_fn)(const uint8_t* data, size_t len, uint32_t timestamp);
typedef void (*lc_emit_emotion_fn)(const char* emotion);
typedef bool (*lc_is_aborted_fn)(void);

/* Set output callbacks. */
void lc_esp_set_callbacks(lc_emit_stt_fn stt,
                          lc_emit_tts_start_fn tts_start,
                          lc_emit_tts_sentence_fn tts_sentence,
                          lc_emit_tts_stop_fn tts_stop,
                          lc_emit_audio_fn audio,
                          lc_emit_emotion_fn emotion,
                          lc_is_aborted_fn aborted);

/* Synchronize device MCP tools from McpServer into LiteCrab's registry.
 * Called once when the audio channel opens and device MCP tools are available. */
void lc_esp_sync_mcp_tools(void);

/* Start a new listening session (clear ASR buffer). */
void lc_esp_listen_start(void);

/* Feed an opus audio frame to the ASR provider. */
void lc_esp_feed_audio(const uint8_t* opus, size_t len);

/* Run a full turn: ASR → LLM tool loop → TTS.
 * Blocks until the turn completes or is aborted.
 * Returns 0 on success, -1 on error. */
int lc_esp_run_turn(void);

/* Abort the current turn. */
void lc_esp_abort(void);

/* Cleanup (close connections, free resources). */
void lc_esp_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LITECRAB_ESP_BRIDGE_H */
