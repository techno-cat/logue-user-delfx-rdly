/*
Copyright 2019 Tomoaki Itoh
This software is released under the MIT License, see LICENSE.txt.
//*/

#ifndef LCWDelay_h
#define LCWDelay_h

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define LCW_DELAY_INPUT_BITS (18)
#define LCW_DELAY_INPUT_SIZE (1 << LCW_DELAY_INPUT_BITS)
#define LCW_DELAY_INPUT_MASK (LCW_DELAY_INPUT_SIZE - 1)

#define LCW_DELAY_SAMPLING_BITS (18)
#define LCW_DELAY_SAMPLING_SIZE (1 << LCW_DELAY_SAMPLING_BITS)
#define LCW_DELAY_SAMPLING_MASK (LCW_DELAY_SAMPLING_SIZE - 1)

#define LCW_GRAIN_SIZE_MIN (1 << 12)
#define LCW_GRAIN_SIZE_MAX (1 << 16)

typedef struct {
    int32_t *reverse;
    int32_t *sampling;
} LCWDelayNeededBuffer;

extern void LCWDelayInit(const LCWDelayNeededBuffer *buffer);
extern void LCWDelayReset(void);
extern void LCWDelayUpdate(uint32_t delaySamples);
extern void LCWDelayInput(int32_t fxIn, int32_t fxIn2);
extern int32_t LCWDelayOutput(void);
extern int32_t LCWDelayReverse(void);

#ifdef __cplusplus
}
#endif

#endif /* LCWDelay_h */
