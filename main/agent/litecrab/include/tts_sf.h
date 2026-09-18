#ifndef LITECRAB_TTS_SF_H
#define LITECRAB_TTS_SF_H

#include <stddef.h>
#include <stdint.h>

#define TTS_SF_ENDPOINT_DEFAULT "https://api.siliconflow.cn/v1/audio/speech"
#define TTS_SF_MODEL_DEFAULT "FunAudioLLM/CosyVoice2-0.5B"
#define TTS_SF_VOICE_DEFAULT "FunAudioLLM/CosyVoice2-0.5B:alex"
#define TTS_SF_API_KEY_DEFAULT "sk-rgdwxvpekwdcrscrfzizqlajntczvdvpplfljpkhffmtlijw"

typedef struct {
    int enabled;
    char endpoint[512];
    char model[128];
    char voice[160];
    char apiKey[256];
    int timeoutMs;
    int sampleRate;
    int frameMs;
} XiaozhiTtsConfig;

int TtsSfInit(const XiaozhiTtsConfig* config);
int TtsSfEnabled(void);
int TtsSfSampleRate(void);
int TtsSfFrameMs(void);
/* Synthesizes text into length-prefixed raw opus frames (2-byte big-endian
 * length + packet, identical to the ASR provider buffer format). Returns 0 on
 * success; *framesOut must be freed by the caller. */
int TtsSfSynthesize(const char* text, unsigned char** framesOut, size_t* framesLen);
int TtsSfHaveOpusEncode(void);

/* exposed for unit tests */
int TtsSfParseWavPcm(const unsigned char* wav, size_t wavLen, int* sampleRate,
                     const unsigned char** pcmOut, size_t* pcmLen);
size_t TtsSfBuildSpeechRequestJson(char* out, size_t cap, const char* model,
                                   const char* voice, const char* text);

#endif
