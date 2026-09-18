#include "siliconflow_provider.h"

#include <esp_log.h>
#include <http.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../provider.h"
#include "board.h"
#include "provider_common.h"

#define TAG "SF_Asr"

namespace {

// ~200 KB of opus per minute of speech (16 kHz mono, ~27 kbps).
constexpr size_t kMaxOpusBytesPerSecond = 200 * 1024 / 60;
constexpr const char* kFallbackText = "你好";

class SiliconFlowAsr : public AsrProvider {
public:
    explicit SiliconFlowAsr(const AgentConfig& cfg) : cfg_(cfg) {}

    void Begin(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mu_);
        frames_.clear();
        session_id_ = session_id;
        aborted_ = false;
    }

    void FeedAudio(const uint8_t* opus, size_t len) override {
        if (len == 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (aborted_) return;
        // Cap the buffer so a stuck-open mic cannot exhaust PSRAM.
        size_t max_total = (size_t)cfg_.max_audio_seconds * kMaxOpusBytesPerSecond;
        size_t cur = 0;
        for (const auto& f : frames_) cur += f.size();
        if (cur + len > max_total) {
            ESP_LOGW(TAG, "audio buffer cap reached, truncating");
            return;
        }
        frames_.emplace_back(opus, opus + len);
    }

    int Finish(std::string& text) override {
        std::vector<std::vector<uint8_t>> frames;
        {
            std::lock_guard<std::mutex> lock(mu_);
            frames.swap(frames_);
            if (aborted_) return -1;
        }
        if (frames.empty()) {
            text.clear();
            return 0;
        }
        // Decode opus -> PCM 16kHz mono
        auto pcm = DecodeOpusFrames(frames);
        if (pcm.empty()) {
            ESP_LOGE(TAG, "opus decode produced no PCM");
            text = kFallbackText;
            return 1;
        }
        // PCM -> WAV bytes
        size_t pcm_bytes = pcm.size() * sizeof(int16_t);
        auto header = BuildWavHeader(16000, 1, 16, (uint32_t)pcm_bytes);
        std::vector<uint8_t> wav;
        wav.reserve(header.size() + pcm_bytes);
        wav.insert(wav.end(), header.begin(), header.end());
        wav.insert(wav.end(), reinterpret_cast<uint8_t*>(pcm.data()),
                   reinterpret_cast<uint8_t*>(pcm.data()) + pcm_bytes);

        // POST multipart/form-data.
        // NOTE: this HttpClient sends the body via SetContent+Open (Open emits
        // Content-Length and transmits headers+body in one go). Calling Write()
        // after Open would switch the request into chunked mode and requires a
        // terminating zero-size chunk, so do NOT use Write here.
        std::string boundary = "----xiaozhi_local_agent_boundary_9f2c1";
        std::string body = BuildMultipartBody(boundary, cfg_.asr_model, wav);
        std::string ct = "multipart/form-data; boundary=" + boundary;

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (IsAborted()) return -1;
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
            if (!http) {
                ESP_LOGE(TAG, "CreateHttp failed");
                continue;
            }
            http->SetTimeout(cfg_.asr_timeout_ms);
            http->SetHeader("Authorization", "Bearer " + cfg_.api_key);
            http->SetHeader("Content-Type", ct);
            http->SetContent(std::string(body));
            if (!http->Open("POST", cfg_.asr_endpoint)) {
                ESP_LOGE(TAG, "HTTP open failed: %d", http->GetLastError());
                continue;
            }
            int status = http->GetStatusCode();
            if (status < 200 || status >= 300) {
                ESP_LOGE(TAG, "ASR HTTP %d", status);
                continue;
            }
            std::string resp = ReadFullBody(http.get());
            std::string decoded = DecodeChunkedIfNeeded(resp);
            std::string text_val;
            if (ParseAsrText(decoded, text_val)) {
                text = std::move(text_val);
                return 0;
            }
            ESP_LOGE(TAG, "ASR parse failed, body[0..200]: %.200s", decoded.c_str());
        }
        // Retry exhausted → fallback
        text = kFallbackText;
        return 1;
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

    static std::string BuildMultipartBody(const std::string& boundary,
                                          const std::string& model,
                                          const std::vector<uint8_t>& wav) {
        std::string b;
        b += "--" + boundary + "\r\n";
        b += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
        b += model + "\r\n";
        b += "--" + boundary + "\r\n";
        b += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
        b += "Content-Type: audio/wav\r\n\r\n";
        b.append(reinterpret_cast<const char*>(wav.data()), wav.size());
        b += "\r\n";
        b += "--" + boundary + "--\r\n";
        return b;
    }

    static std::string DecodeChunkedIfNeeded(const std::string& resp) {
        // Heuristic: if resp looks like chunked (starts with hex digits + CRLF), decode.
        if (resp.empty()) return resp;
        size_t crlf = resp.find("\r\n");
        if (crlf == std::string::npos) return resp;
        std::string first_line = resp.substr(0, crlf);
        // Strip extensions
        size_t semi = first_line.find(';');
        if (semi != std::string::npos) first_line = first_line.substr(0, semi);
        char* endp = nullptr;
        unsigned long sz = strtoul(first_line.c_str(), &endp, 16);
        if (endp != first_line.c_str() && sz > 0 && sz <= resp.size()) {
            // Looks chunked: attempt decode; on failure return original
            std::string decoded = DecodeChunked(resp);
            if (!decoded.empty()) return decoded;
        }
        return resp;
    }

    static bool ParseAsrText(const std::string& body, std::string& out) {
        cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
        if (!root) return false;
        bool ok = false;
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            out = text->valuestring;
            ok = true;
        }
        cJSON_Delete(root);
        return ok;
    }

    AgentConfig cfg_;
    std::mutex mu_;
    std::string session_id_;
    std::vector<std::vector<uint8_t>> frames_;
    bool aborted_ = false;
};

}  // namespace

std::unique_ptr<AsrProvider> MakeSiliconFlowAsr(const AgentConfig& cfg) {
    return std::make_unique<SiliconFlowAsr>(cfg);
}
