/*
Copyright 2019 Tomoaki Itoh
This software is released under the MIT License, see LICENSE.txt.
//*/

#include "LCWDelay.h"
#include "LCWFixedMath.h"
#include "LCWDelayFirParamTable.h"

#define LCW_DELAY_BUFFER_DEC(buf) (((buf)->pointer - 1) & (buf)->mask)
#define LCW_GRAIN_BLOCK_NUM (2)

typedef struct {
    SQ7_24 *buffer;
    uint32_t size;
    uint32_t mask;
    int32_t pointer;
} LCWDelayBuffer;

typedef struct {
    uint32_t counter;
    SQ3_28 t;
    SQ3_28 dt;
    uint32_t size;
    SQ7_24 step; // 未使用
    int32_t pointer;
} LCWGrainBlock;

typedef struct {
    int32_t originIndex;
    LCWGrainBlock grains[LCW_GRAIN_BLOCK_NUM];
    SQ7_24 step; // 未使用
    uint32_t delaySize;
    uint32_t sampling; // u.8
} LCWDelayBlock;

// hann-window
#define LCW_GRAIN_WND_BITS (6)
#define LCW_GRAIN_WND_SIZE (1 << LCW_GRAIN_WND_BITS)
#define LCW_GRAIN_WND_MASK (LCW_GRAIN_WND_SIZE - 1)
static const uint16_t windowTable[LCW_GRAIN_WND_SIZE] = {
    0x0000, 0x0027, 0x009D, 0x0161, 0x0270, 0x03C7, 0x0565, 0x0743,
    0x095F, 0x0BB3, 0x0E39, 0x10EA, 0x13C1, 0x16B6, 0x19C2, 0x1CDD,
    0x2000, 0x2323, 0x263E, 0x294A, 0x2C3F, 0x2F16, 0x31C7, 0x344D,
    0x36A1, 0x38BD, 0x3A9B, 0x3C39, 0x3D90, 0x3E9F, 0x3F63, 0x3FD9,
    0x4000, 0x3FD9, 0x3F63, 0x3E9F, 0x3D90, 0x3C39, 0x3A9B, 0x38BD,
    0x36A1, 0x344D, 0x31C7, 0x2F16, 0x2C3F, 0x294A, 0x263E, 0x2323,
    0x2000, 0x1CDD, 0x19C2, 0x16B6, 0x13C1, 0x10EA, 0x0E39, 0x0BB3,
    0x095F, 0x0743, 0x0565, 0x03C7, 0x0270, 0x0161, 0x009D, 0x0027
};

static LCWDelayBuffer delayInputBuffer = {
    (SQ7_24 *)0,
    LCW_DELAY_INPUT_SIZE,
    LCW_DELAY_INPUT_MASK,
    0
};

static LCWDelayBuffer delaySamplingBuffer = {
    (SQ7_24 *)0,
    LCW_DELAY_SAMPLING_SIZE,
    LCW_DELAY_SAMPLING_MASK,
    0
};

static LCWDelayBlock delayBlock;

static uint32_t convergeDelaySize(uint32_t src, uint32_t dst)
{
    const SQ7_24 param = LCW_SQ7_24( 0.9976 );
    if ( src < dst ) {
        const uint32_t diff = dst - src;
        const uint32_t tmp = (uint32_t)( ((uint64_t)diff * param) >> 24 );

        return dst - tmp;
    }
    else {
        const uint32_t diff = src - dst;
        const uint32_t tmp = (uint32_t)( ((uint64_t)diff * param) >> 24 );

        return dst + tmp;
    }
}

static SQ15_16 window(SQ15_16 t)
{
#if 0
    if ( LCW_SQ15_16(0.5) < t ) {
        return t << 1;
    }
    else {
        return (LCW_SQ15_16(1.0) - t) << 1;
    }
#else
    const int32_t i = t >> (16 - LCW_GRAIN_WND_BITS);
    const int32_t frac = t & (0xFFFF >> LCW_GRAIN_WND_BITS);

    // SQ1.14
    const int32_t val1 = windowTable[i & LCW_GRAIN_WND_MASK];
    const int32_t val2 = windowTable[(i+1) & LCW_GRAIN_WND_MASK];

    const SQ15_16 ret = val1 + (((val2 - val1) * frac) >> (16 - LCW_GRAIN_WND_BITS));
    return ret << 2;
#endif
}

static SQ7_24 resampling(const LCWDelayBuffer *p, int32_t i, const SQ3_12 *fir, int32_t n)
{
    const uint32_t mask = p->mask;
#if (1)
    int64_t ret = 0;
    for (int32_t j=0; j<n; j++) {
        ret += ( (int64_t)(p->buffer[(i + j) & mask]) * fir[j] );
    }

    return (SQ7_24)( ret >> LCW_DELAY_FIR_VALUE_BITS );
#else
    return p->buffer[(i + (n >> 1)) & mask];
#endif
}

static void initGrainBlock(LCWGrainBlock *grain, const LCWDelayBuffer *buf, SQ7_24 step, uint32_t size)
{
    grain->counter = 0;
    grain->t = LCW_SQ3_28( 1.0 );
    grain->dt = LCW_SQ3_28( 1.0 ) / size;
    grain->size = size;
    grain->step = step;
    grain->pointer = buf->pointer;

    // 切り上げ
    if ( (grain->dt * size) < LCW_SQ3_28(1.0) ) {
        grain->dt++;
    }
}

static SQ7_24 grainOut(const LCWDelayBuffer *input, const LCWGrainBlock *blocks, int32_t blockNum)
{
    SQ7_24 sample = 0;
    for (int32_t i=0; i<blockNum; i++) {
        const LCWGrainBlock *grain = &(blocks[i]);
        if ( (SQ3_28)0 < grain->t ) {
            // for Window func
            const SQ3_28 t = LCW_SQ3_28(1.0) - grain->t;

            const uint32_t offset = grain->counter;
            const int32_t j = grain->pointer + (int32_t)offset;
            const int64_t val = input->buffer[j & input->mask];

            sample += (SQ7_24)( (val * window(t >> (28 - 16))) >> 16 );
            //sample += (SQ7_24)(val >> 1);
        }
    }

    // grainが2つじゃない場合は、ここで正規化する
    return sample;
}

void LCWDelayInit(const LCWDelayNeededBuffer *buffer)
{
    delayInputBuffer.buffer = (SQ7_24 *)buffer->reverse;
    delaySamplingBuffer.buffer = (SQ7_24 *)buffer->sampling;
}

void LCWDelayReset(void)
{
    delayBlock.originIndex = 0;
    delayBlock.step = LCW_SQ7_24( 1.0 );
    delayBlock.delaySize = LCW_GRAIN_SIZE_MIN;
    delayBlock.sampling = LCW_GRAIN_SIZE_MIN << 8; // q8
}

void LCWDelayUpdate(uint32_t delaySamples)
{
    if ( LCW_GRAIN_SIZE_MIN <= delaySamples && delaySamples <= LCW_GRAIN_SIZE_MAX ) {
        delayBlock.delaySize = delaySamples;
    }

    delayBlock.sampling = convergeDelaySize(
        delayBlock.sampling, delayBlock.delaySize << 8 );
}

void LCWDelayInput(int32_t fxIn, int32_t fxIn2)
{
    LCWDelayBuffer *input = &delayInputBuffer;
    LCWDelayBuffer *sampling = &delaySamplingBuffer;

    input->pointer = LCW_DELAY_BUFFER_DEC(input);
    input->buffer[input->pointer] = (SQ7_24)fxIn;

    for (int32_t i=0; i<LCW_GRAIN_BLOCK_NUM; i++) {
        LCWGrainBlock *grain = &(delayBlock.grains[i]);
        grain->t -= grain->dt;
        grain->counter++;
    }

    LCWGrainBlock *origin = &(delayBlock.grains[delayBlock.originIndex]);
    if ( origin->t <= LCW_SQ3_28(1.0 - (1.0/LCW_GRAIN_BLOCK_NUM)) ) {
        delayBlock.originIndex++;
        if ( LCW_GRAIN_BLOCK_NUM <= delayBlock.originIndex ) {
            delayBlock.originIndex = 0;
        }

        // delayTime = grainSize / 2 にしたい
        const uint32_t grainSize = delayBlock.sampling >> (8+1); // q8 -> int
        LCWGrainBlock *newOrigin = &(delayBlock.grains[delayBlock.originIndex]);
        initGrainBlock( newOrigin, input, delayBlock.step, grainSize );

        // Window関数の周期を合わせる
        const SQ3_28 dt = newOrigin->dt;
        for (int32_t i=0; i<LCW_GRAIN_BLOCK_NUM; i++) {
            LCWGrainBlock *grain = &(delayBlock.grains[i]);
            grain->dt = dt;
        }
    }

    sampling->pointer = LCW_DELAY_BUFFER_DEC(sampling);
    sampling->buffer[sampling->pointer] = fxIn2; // = rev + feedback
}

int32_t LCWDelayOutput(void)
{
    const LCWDelayBuffer *sampling = &delaySamplingBuffer;

    const uint32_t offset = delayBlock.sampling; // u.8
    const int32_t i = sampling->pointer - (LCW_DELAY_FIR_TAP >> 1) + (int32_t)(offset >> 8);
    const SQ3_12 *fir = gLcwDelayFirTable[ (offset >> (8 - LCW_DELAY_FIR_TABLE_BITS)) & LCW_DELAY_FIR_TABLE_MASK ];
    return resampling( sampling, i, fir, LCW_DELAY_FIR_TAP );
}

int32_t LCWDelayReverse(void)
{
    const LCWDelayBuffer *input = &delayInputBuffer;
    return grainOut( input, delayBlock.grains, LCW_GRAIN_BLOCK_NUM );
}
