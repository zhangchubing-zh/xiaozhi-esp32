/* Encodes a 16 kHz mono 16-bit PCM WAV into length-prefixed raw Opus frames
 * (2-byte big-endian length + packet), the exact format the xiaozhi gateway
 * ASR buffer accepts. Used by integration tests to fabricate device audio. */
#include <opus/opus.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s in.wav out.frames\n", argv[0]);
        return 2;
    }
    FILE* in = fopen(argv[1], "rb");
    if (!in) {
        perror("open input");
        return 1;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    rewind(in);
    if (size < 44) {
        fprintf(stderr, "input too small\n");
        fclose(in);
        return 1;
    }
    unsigned char* data = malloc((size_t) size);
    if (!data || fread(data, 1, (size_t) size, in) != (size_t) size) {
        fprintf(stderr, "read failed\n");
        fclose(in);
        return 1;
    }
    fclose(in);
    if (memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) {
        fprintf(stderr, "not a RIFF/WAVE file\n");
        return 1;
    }
    unsigned char* pcm = NULL;
    unsigned int dataLen = 0;
    long off = 12;
    while (off + 8 <= size) {
        unsigned int chunkSize = (unsigned int) data[off + 4] | ((unsigned int) data[off + 5] << 8) |
                                 ((unsigned int) data[off + 6] << 16) | ((unsigned int) data[off + 7] << 24);
        if (!memcmp(data + off, "data", 4)) {
            pcm = data + off + 8;
            dataLen = chunkSize;
            break;
        }
        off += 8 + (long) chunkSize + (chunkSize & 1);
    }
    if (!pcm || !dataLen) {
        fprintf(stderr, "no data chunk\n");
        return 1;
    }
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != OPUS_OK) {
        fprintf(stderr, "opus_encoder_create failed: %d\n", err);
        return 1;
    }
    FILE* out = fopen(argv[2], "wb");
    if (!out) {
        perror("open output");
        return 1;
    }
    unsigned char packet[4000];
    size_t pos = 0;
    int frames = 0;
    while (pos + 1920 <= dataLen) {
        int n = opus_encode(enc, (const opus_int16*) (void*) (pcm + pos), 960, packet, sizeof packet);
        if (n < 0) {
            fprintf(stderr, "opus_encode failed: %d\n", n);
            fclose(out);
            return 1;
        }
        unsigned char prefix[2] = {(unsigned char) (n >> 8), (unsigned char) (n & 0xFF)};
        fwrite(prefix, 1, 2, out);
        fwrite(packet, 1, (size_t) n, out);
        pos += 1920;
        frames++;
    }
    fclose(out);
    free(data);
    printf("encoded %d frames from %zu pcm bytes\n", frames, pos);
    return 0;
}
