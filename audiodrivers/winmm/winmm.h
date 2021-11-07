#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef void (StreamCallback_t)(int16_t *stream, int32_t numSamples);

void lockMixer(void);
void unlockMixer(void);
bool openMixer(int32_t mixingFrequency, int32_t mixingBufferSize, StreamCallback_t* cbRender);
void closeMixer(void);
