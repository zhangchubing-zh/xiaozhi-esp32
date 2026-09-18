#include "siliconflow_provider.h"

#include <esp_log.h>
#include <http.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "../provider.h"
#include "board.h"
#include "provider_common.h"

#define TAG "SF_Tts"

namespace {

class SiliconFlowTts : public TtsProvider {
public:
    explicit SiliconFlowTts(const AgentConfig& cfg) : cfg_(cfg) {}

    bool Synthesize(const std::string& text,
                    std::vector<std::vector<uint8_t>>& opus_frames) override {
        opus_frames.clear();
        if (text.empty()) return true;
        std::string body = BuildRequestBody(text);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (IsAborted()) return false;
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
            if (!http) {
                ESP_LOGE(TAG, "CreateHttp failed");
                continue;
            }
            http->SetTimeout(cfg_.tts_timeout_ms);
            http->SetHeader("Authorization", "Bearer " + cfg_.api_key);
            http->SetHeader("Content-Type", "application/json");
            // SetContent+Open sends headers with Content-Length and the body in
            // one shot. Do NOT call Write() after Open (chunked mode).
            http->SetContent(std::string(body));
            if (!http->Open("POST", cfg_.tts_endpoint)) {
                ESP_LOGE(TAG, "HTTP open failed: %d", http->GetLastError());
                continue;
            }
            int status = http->GetStatusCode();
            if (status < 200 || status >= 300) {
                ESP_LOGE(TAG, "TTS HTTP %d", status);
                continue;
            }
            // Read full body (may be chunked for audio responses)
            std::string raw = ReadFullBody(http.get());
            std::vector<uint8_t> wav(raw.begin(), raw.end());
            // Decode chunked if needed
            if (LooksChunked(wav)) {
                std::string de = DecodeChunked(raw);
                wav.assign(de.begin(), de.end());
            }
            int sr = 0, ch = 0;
            auto pcm = ParseWav(wav, sr, ch);
            if (pcm.empty()) {
                ESP_LOGE(TAG, "TTS WAV parse failed, size=%zu", wav.size());
                continue;
            }
            // Encode PCM to opus frames at 60ms. Use 24kHz if matches, else 16kHz.
            // Server hello declares sample_rate=24000. The device's audio decoder
            // is configured at codec->output_sample_rate() and will resample.
            int enc_sr = (sr > 0) ? sr : 24000;
            int enc_ch = (ch > 0) ? ch : 1;
            opus_frames = EncodePcmToOpus(pcm, enc_sr, enc_ch, 60);
            if (opus_frames.empty()) {
                ESP_LOGE(TAG, "opus encode produced 0 frames from %zu PCM samples", pcm.size());
                continue;
            }
            ESP_LOGI(TAG, "TTS synthesized %zu opus frames (%zu PCM, %dHz)",
                     opus_frames.size(), pcm.size(), enc_sr);
            return true;
        }
        return false;
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

    std::string BuildRequestBody(const std::string& text) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", cfg_.tts_model.c_str());
        cJSON* input = cJSON_CreateString(text.c_str());
        cJSON_AddItemToObject(root, "input", input);
        cJSON_AddStringToObject(root, "voice", cfg_.tts_voice.c_str());
        cJSON_AddStringToObject(root, "response_format", "wav");
        char* s = cJSON_PrintUnformatted(root);
        std::string out(s);
        cJSON_free(s);
        cJSON_Delete(root);
        return out;
    }

    static bool LooksChunked(const std::vector<uint8_t>& b) {
        if (b.size() < 4) return false;
        // WAV responses start with "RIFF"; chunked responses start with hex digits + CRLF.
        if (b.size() >= 4 && memcmp(b.data(), "RIFF", 4) == 0) return false;
        size_t crlf = 0;
        for (size_t i = 0; i + 1 < b.size(); ++i) {
            if (b[i] == '\r' && b[i + 1] == '\n') { crlf = i; break; }
        }
        if (crlf == 0 || crlf > 16) return false;
        // First line all hex digits?
        for (size_t i = 0; i < crlf; ++i) {
            char c = (char)b[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ';')) {
                return false;
            }
        }
        return true;
    }

    AgentConfig cfg_;
    std::mutex mu_;
    bool aborted_ = false;
};

}  // namespace

std::unique_ptr<TtsProvider> MakeSiliconFlowTts(const AgentConfig& cfg) {
    return std::make_unique<SiliconFlowTts>(cfg);
}
