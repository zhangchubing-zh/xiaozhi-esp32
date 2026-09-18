#ifndef LITECRAB_ASR_SF_H
#define LITECRAB_ASR_SF_H

#include <stddef.h>
#include <stdint.h>

#define ASR_SF_ENDPOINT_DEFAULT "https://api.siliconflow.cn/v1/audio/transcriptions"
#define ASR_SF_MODEL_DEFAULT "FunAudioLLM/SenseVoiceSmall"
#define ASR_SF_API_KEY_DEFAULT "sk-rgdwxvpekwdcrscrfzizqlajntczvdvpplfljpkhffmtlijw"

typedef struct {
    char provider[16];
    char endpoint[512];
    char model[128];
    char apiKey[256];
    int timeoutMs;
    int maxAudioSeconds;
    int fallbackToStub;
    char stubInputText[512];
} XiaozhiAsrConfig;

typedef struct XiaozhiAsrProvider XiaozhiAsrProvider;
struct XiaozhiAsrProvider {
    void (*begin)(XiaozhiAsrProvider*, const char* session_id);
    void (*feed_audio)(XiaozhiAsrProvider*, const unsigned char* opus, size_t len);
    int (*finish)(XiaozhiAsrProvider*, const char* session_id, char* text, size_t textSize);
    void (*abort)(XiaozhiAsrProvider*);
};

int AsrSfInit(const XiaozhiAsrConfig* config);
XiaozhiAsrProvider* AsrSfCreateProvider(void);
void AsrSfDestroyProvider(XiaozhiAsrProvider* provider);
int AsrSfHaveOpusDecode(void);

int AsrSfBuildWavHeader(unsigned char* hdr44, uint32_t dataBytes, int sampleRate, int channels, int bits);
size_t AsrSfBuildMultipartBody(char* out,
                               size_t cap,
                               const char* boundary,
                               const char* model,
                               const unsigned char* wav,
                               size_t wavLen);
int AsrSfParseTranscriptionResponse(const char* body, char* text, size_t textSize);

#endif
