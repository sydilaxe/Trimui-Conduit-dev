#pragma once
/* Write stereo float frames as WAV. bits = 16 (PCM) or 32 (IEEE float).
 * Data is fsync'd before returning so it survives a sudden power-off. */
int wav_write(const char *path, const float *st, int frames, int rate, int bits);

/* Read a WAV (PCM 8/16/24/32, float 32/64, mono..N channels, extensible),
 * convert to stereo float at dst_rate. Returns malloc'd buffer or NULL. */
float *wav_read(const char *path, int dst_rate, int max_frames, int *out_frames);
