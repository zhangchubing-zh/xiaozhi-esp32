#include "provider_common.h"

#include <esp_log.h>
#include <http.h>

#include <cstring>

#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#define TAG "AgentProvider"

namespace {

// Local frame-duration enum mapper (the audio_service versions are file-local).
esp_opus_enc_frame_duration_t FrameDurEnumEnc(int ms) {
    switch (ms) {
        case 5:   return ESP_OPUS_ENC_FRAME_DURATION_5_MS;
        case 10:  return ESP_OPUS_ENC_FRAME_DURATION_10_MS;
        case 20:  return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
        case 40:  return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
        case 60:  return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        case 80:  return ESP_OPUS_ENC_FRAME_DURATION_80_MS;
        case 100: return ESP_OPUS_ENC_FRAME_DURATION_100_MS;
        case 120: return ESP_OPUS_ENC_FRAME_DURATION_120_MS;
        default:  return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    }
}

esp_opus_dec_frame_duration_t FrameDurEnumDec(int ms) {
    switch (ms) {
        case 5:   return ESP_OPUS_DEC_FRAME_DURATION_5_MS;
        case 10:  return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
        case 20:  return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        case 40:  return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        case 60:  return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        case 80:  return ESP_OPUS_DEC_FRAME_DURATION_80_MS;
        case 100: return ESP_OPUS_DEC_FRAME_DURATION_100_MS;
        case 120: return ESP_OPUS_DEC_FRAME_DURATION_120_MS;
        default:  return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    }
}

}  // namespace

std::string ReadFullBody(Http* http) {
    std::string body;
    if (http == nullptr) return body;
    std::string all = http->ReadAll();
    if (!all.empty()) return all;
    constexpr size_t kBuf = 1024;
    std::vector<char> buf(kBuf);
    while (true) {
        int n = http->Read(buf.data(), buf.size());
        if (n <= 0) break;
        body.append(buf.data(), n);
    }
    return body;
}

std::string DecodeChunked(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        size_t eol = in.find("\r\n", i);
        if (eol == std::string::npos) break;
        std::string size_str = in.substr(i, eol - i);
        size_t semi = size_str.find(';');
        if (semi != std::string::npos) size_str = size_str.substr(0, semi);
        unsigned long sz = strtoul(size_str.c_str(), nullptr, 16);
        i = eol + 2;
        if (sz == 0) break;
        if (i + sz > in.size()) {
            out.append(in.data() + i, in.size() - i);
            break;
        }
        out.append(in.data() + i, sz);
        i += sz;
        if (i + 2 <= in.size() && in[i] == '\r' && in[i + 1] == '\n') i += 2;
    }
    return out;
}

std::vector<uint8_t> BuildWavHeader(uint32_t sample_rate, uint16_t channels,
                                     uint16_t bits_per_sample, uint32_t data_size) {
    std::vector<uint8_t> h(44, 0);
    auto put16 = [&h](size_t off, uint16_t v) {
        h[off] = v & 0xFF;
        h[off + 1] = (v >> 8) & 0xFF;
    };
    auto put32 = [&h](size_t off, uint32_t v) {
        h[off] = v & 0xFF;
        h[off + 1] = (v >> 8) & 0xFF;
        h[off + 2] = (v >> 16) & 0xFF;
        h[off + 3] = (v >> 24) & 0xFF;
    };
    uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    uint16_t block_align = channels * bits_per_sample / 8;
    memcpy(h.data() + 0, "RIFF", 4);
    put32(4, 36 + data_size);
    memcpy(h.data() + 8, "WAVE", 4);
    memcpy(h.data() + 12, "fmt ", 4);
    put32(16, 16);
    put16(20, 1);
    put16(22, channels);
    put32(24, sample_rate);
    put32(28, byte_rate);
    put16(32, block_align);
    put16(34, bits_per_sample);
    memcpy(h.data() + 36, "data", 4);
    put32(40, data_size);
    return h;
}

std::vector<int16_t> DecodeOpusFrames(const std::vector<std::vector<uint8_t>>& opus_frames) {
    std::vector<int16_t> pcm;
    if (opus_frames.empty()) return pcm;
    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate = 16000;
    cfg.channel = 1;  // ESP_AUDIO_MONO = 1
    cfg.frame_duration = FrameDurEnumDec(60);
    cfg.self_delimited = false;
    void* dec = nullptr;
    auto ret = esp_opus_dec_open(&cfg, sizeof(cfg), &dec);
    if (dec == nullptr) {
        ESP_LOGE(TAG, "esp_opus_dec_open failed: %d", (int)ret);
        return pcm;
    }
    // 60ms @ 16kHz mono = 960 samples = 1920 bytes. Leave headroom for resampling.
    const size_t kOutBytes = 1920 * 2;
    std::vector<uint8_t> out_buf(kOutBytes);
    for (const auto& f : opus_frames) {
        esp_audio_dec_in_raw_t raw{};
        raw.buffer = const_cast<uint8_t*>(f.data());
        raw.len = f.size();
        esp_audio_dec_out_frame_t out{};
        out.buffer = out_buf.data();
        out.len = out_buf.size();
        esp_audio_dec_info_t info{};
        ret = esp_opus_dec_decode(dec, &raw, &out, &info);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "opus decode failed: %d (frame %zu bytes)", (int)ret, f.size());
            continue;
        }
        size_t samples = out.decoded_size / sizeof(int16_t);
        const int16_t* p = reinterpret_cast<const int16_t*>(out_buf.data());
        pcm.insert(pcm.end(), p, p + samples);
    }
    esp_opus_dec_close(dec);
    return pcm;
}

std::vector<std::vector<uint8_t>> EncodePcmToOpus(const std::vector<int16_t>& pcm,
                                                  int sample_rate,
                                                  int channels,
                                                  int frame_duration_ms) {
    std::vector<std::vector<uint8_t>> out_frames;
    if (pcm.empty()) return out_frames;
    esp_opus_enc_config_t cfg = {};
    cfg.sample_rate = sample_rate;
    cfg.channel = (channels == 2) ? 2 : 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    cfg.frame_duration = FrameDurEnumEnc(frame_duration_ms);
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    cfg.complexity = 5;
    cfg.enable_fec = false;
    cfg.enable_dtx = false;
    cfg.enable_vbr = true;
    void* enc = nullptr;
    auto ret = esp_opus_enc_open(&cfg, sizeof(cfg), &enc);
    if (enc == nullptr) {
        ESP_LOGE(TAG, "esp_opus_enc_open failed: %d", (int)ret);
        return out_frames;
    }
    int in_size = 0, out_size = 0;
    esp_opus_enc_get_frame_size(enc, &in_size, &out_size);
    if (in_size <= 0 || out_size <= 0) {
        esp_opus_enc_close(enc);
        return out_frames;
    }
    int frame_samples = in_size / sizeof(int16_t);
    std::vector<uint8_t> out_buf(out_size);
    size_t total_samples = pcm.size();
    for (size_t pos = 0; pos + frame_samples <= total_samples; pos += frame_samples) {
        esp_audio_enc_in_frame_t in{};
        in.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm.data() + pos));
        in.len = in_size;
        esp_audio_enc_out_frame_t out{};
        out.buffer = out_buf.data();
        out.len = out_buf.size();
        ret = esp_opus_enc_process(enc, &in, &out);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "opus encode failed: %d", (int)ret);
            continue;
        }
        out_frames.emplace_back(out_buf.begin(), out_buf.begin() + out.encoded_bytes);
    }
    esp_opus_enc_close(enc);
    return out_frames;
}

std::vector<int16_t> ParseWav(const std::vector<uint8_t>& wav,
                              int& out_sample_rate,
                              int& out_channels) {
    std::vector<int16_t> pcm;
    out_sample_rate = 0;
    out_channels = 0;
    if (wav.size() < 44) return pcm;
    if (memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        return pcm;
    }
    size_t pos = 12;
    uint16_t bits_per_sample = 0;
    while (pos + 8 <= wav.size()) {
        char id[5] = {0};
        memcpy(id, wav.data() + pos, 4);
        uint32_t chunk_size = wav[pos + 4] | (wav[pos + 5] << 8) |
                              (wav[pos + 6] << 16) | ((uint32_t)wav[pos + 7] << 24);
        pos += 8;
        if (memcmp(id, "fmt ", 4) == 0) {
            if (chunk_size < 16 || pos + 16 > wav.size()) return pcm;
            uint16_t fmt = wav[pos] | (wav[pos + 1] << 8);
            out_channels = wav[pos + 2] | (wav[pos + 3] << 8);
            out_sample_rate = wav[pos + 4] | (wav[pos + 5] << 8) |
                              (wav[pos + 6] << 16) | ((uint32_t)wav[pos + 7] << 24);
            bits_per_sample = wav[pos + 14] | (wav[pos + 15] << 8);
            if (fmt != 1 || bits_per_sample != 16) return pcm;
        } else if (memcmp(id, "data", 4) == 0) {
            size_t data_end = pos + chunk_size;
            if (data_end > wav.size()) data_end = wav.size();
            size_t n_samples = (data_end - pos) / sizeof(int16_t);
            const int16_t* p = reinterpret_cast<const int16_t*>(wav.data() + pos);
            pcm.assign(p, p + n_samples);
            return pcm;
        }
        pos += chunk_size;
        if (chunk_size & 1) ++pos;
    }
    return pcm;
}
