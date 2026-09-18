#ifndef AGENT_PROVIDER_COMMON_H
#define AGENT_PROVIDER_COMMON_H

#include <cstdint>
#include <string>
#include <vector>

class Http;

// Read an HTTP response body fully. Handles chunked transfer-encoding by
// accumulating decoded bytes. Returns empty string on failure.
std::string ReadFullBody(Http* http);

// Decode raw chunked-transfer-encoded bytes into the underlying body.
// `in` is raw bytes possibly containing "size\r\n<data>\r\n" chunks.
// Returns the de-chunked body.
std::string DecodeChunked(const std::string& in);

// Build a 44-byte WAV header for 16-bit PCM mono.
// `sample_rate`, `channels`, `bits_per_sample`, `data_size` describe the payload.
std::vector<uint8_t> BuildWavHeader(uint32_t sample_rate, uint16_t channels,
                                    uint16_t bits_per_sample, uint32_t data_size);

// Decode opus frames (each frame's full payload, e.g. 60ms each, 16kHz mono)
// into PCM 16-bit samples. The opus frames are the raw device-uplink frames
// collected by AsrProvider. Returns interleaved int16_t samples as bytes (LE).
// Returns empty on failure.
std::vector<int16_t> DecodeOpusFrames(const std::vector<std::vector<uint8_t>>& opus_frames);

// Encode PCM samples (16-bit, `sample_rate`, `channels`) into opus frames of
// `frame_duration_ms`. Returns a vector of opus frame payloads (no container).
// Uses the Espressif esp_opus_enc API which is already linked by audio_service.
std::vector<std::vector<uint8_t>> EncodePcmToOpus(const std::vector<int16_t>& pcm,
                                                  int sample_rate,
                                                  int channels,
                                                  int frame_duration_ms);

// Parse a WAV buffer into PCM samples. Returns empty on failure.
// `out_sample_rate`, `out_channels` receive the parsed header info.
std::vector<int16_t> ParseWav(const std::vector<uint8_t>& wav,
                              int& out_sample_rate,
                              int& out_channels);

#endif  // AGENT_PROVIDER_COMMON_H
