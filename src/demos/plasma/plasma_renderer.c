/*
 * MIT License
 *
 * Copyright (c) 2025 Manuel Capel
 * Copyright (c) 2025 Christian Klukas (adaptation for ck-plasma for dynamic changes)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Adapted to the ck-plasma demo to keep GUI and image generation in separate files.
 * Original algorithm: https://github.com/mancap314/cxsa/blob/main/plasma.c
 *
 * Update (ck-plasma):
 *  - Adds a "sequence_id" parameter to vary the plasma pattern/colors per 0..239 loop.
 *  - Changes are seam-safe: at time==0 (and conceptually 2π) the added variation is zero,
 *    so switching to the next sequence does not introduce a visible jump at the loop point.
 *  - Variation is injected into the *field evolution* (phases, coupling, diff response),
 *    not only a trivial palette remap.
 */

#include "plasma_renderer.h"

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ------------------------------ helpers ------------------------------ */

static inline float clamp_unit(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static int wrap_time_step(int frame_index)
{
    if (CK_PLASMA_RENDERER_TIME_STEPS <= 0) return 0;
    int wrapped = frame_index % CK_PLASMA_RENDERER_TIME_STEPS;
    if (wrapped < 0) {
        wrapped += CK_PLASMA_RENDERER_TIME_STEPS;
    }
    return wrapped;
}

static inline uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline float u32_to_unit_float(uint32_t v)
{
    return (float)((v >> 8) & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

static inline float rand_signed(uint32_t *state)
{
    return u32_to_unit_float(xorshift32(state)) * 2.0f - 1.0f;
}

static inline float rand_range(uint32_t *state, float lo, float hi)
{
    return lo + (hi - lo) * u32_to_unit_float(xorshift32(state));
}

/* ------------------------------ per-sequence params ------------------------------ */

typedef struct ck_plasma_seq_params
{
    float phase_a;
    float phase_b;
    float freq_warp;
    float couple;

    float bias;
    float diff_gain;
    float diff_gamma;

    float mix_rg;
    float mix_rb;
    float mix_gr;
    float mix_gb;
    float mix_br;
    float mix_bg;
} ck_plasma_seq_params;

static ck_plasma_seq_params make_seq_params(int sequence_id)
{
    uint32_t s = (uint32_t)sequence_id;
    s ^= 0x9E3779B9u;
    s *= 0x85EBCA6Bu;
    s ^= s >> 13;
    s *= 0xC2B2AE35u;
    s ^= s >> 16;
    if (s == 0) s = 1u;

    ck_plasma_seq_params p;
    p.phase_a = rand_range(&s, -2.0f * (float)M_PI, 2.0f * (float)M_PI);
    p.phase_b = rand_range(&s, -2.0f * (float)M_PI, 2.0f * (float)M_PI);
    p.freq_warp = rand_range(&s, -0.03f, 0.03f);
    p.couple = rand_range(&s, -0.02f, 0.02f);
    p.bias = 0.7f + rand_range(&s, -0.12f, 0.12f);
    p.diff_gain = 1.0f + rand_range(&s, -0.20f, 0.20f);
    p.diff_gamma = 1.0f + rand_range(&s, -0.25f, 0.25f);
    p.mix_rg = rand_range(&s, -0.18f, 0.18f);
    p.mix_rb = rand_range(&s, -0.18f, 0.18f);
    p.mix_gr = rand_range(&s, -0.18f, 0.18f);
    p.mix_gb = rand_range(&s, -0.18f, 0.18f);
    p.mix_br = rand_range(&s, -0.18f, 0.18f);
    p.mix_bg = rand_range(&s, -0.18f, 0.18f);
    return p;
}

void ck_plasma_render_frame(unsigned char *dst, int width, int height,
                            int frame_index, int sequence_id)
{
    if (!dst || width <= 0 || height <= 0) return;

    const float two_pi = 2.0f * (float)M_PI;
    const int wrapped = wrap_time_step(frame_index);
    const float time = (float)wrapped / (float)CK_PLASMA_RENDERER_TIME_STEPS * two_pi;
    const float envelopescale = 0.5f * (1.0f - cosf(time));

    const ck_plasma_seq_params sp = make_seq_params(sequence_id);
    const float phase_a = time + envelopescale * sp.phase_a;
    const float phase_b = time + envelopescale * sp.phase_b;
    const float k = 1.0f + envelopescale * sp.freq_warp;
    const float cpl = envelopescale * sp.couple;
    const float bias = 0.7f + envelopescale * (sp.bias - 0.7f);
    const float diff_gain = 1.0f + envelopescale * (sp.diff_gain - 1.0f);
    const float diff_gamma = 1.0f + envelopescale * (sp.diff_gamma - 1.0f);
    const float mix_rg = envelopescale * sp.mix_rg;
    const float mix_rb = envelopescale * sp.mix_rb;
    const float mix_gr = envelopescale * sp.mix_gr;
    const float mix_gb = envelopescale * sp.mix_gb;
    const float mix_br = envelopescale * sp.mix_br;
    const float mix_bg = envelopescale * sp.mix_bg;

    const float inv_height = 1.0f / (float)height;
    const float r_x = (float)width;
    const float r_y = (float)height;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float p_x = (float)x * 2.0f - r_x;
            float p_y = (float)y * 2.0f - r_y;
            p_x *= inv_height;
            p_y *= inv_height;

            const float dot = p_x * p_x + p_y * p_y;
            const float l_val = 4.0f - 4.0f * fabsf(0.7f - dot);
            float v_x = p_x * l_val;
            float v_y = p_y * l_val;

            float o[4] = {0.0f, 0.0f, 0.0f, 0.0f};

            for (uint8_t iy = 1; iy <= CK_PLASMA_RENDERER_ITERATIONS; ++iy) {
                const float fiy = (float)iy;
                float tmp0_x = (v_y * fiy) * k + phase_a;
                float tmp0_y = (v_x * fiy) * k + fiy + phase_b;

                tmp0_x = cosf(tmp0_x);
                tmp0_y = cosf(tmp0_y);

                tmp0_x = tmp0_x / fiy + bias;
                tmp0_y = tmp0_y / fiy + bias;

                v_x += tmp0_x;
                v_y += tmp0_y;

                if (fabsf(cpl) > 0.0f) {
                    const float vx = v_x;
                    const float vy = v_y;
                    v_x = vx + cpl * vy;
                    v_y = vy - cpl * vx;
                }

                float diff = fabsf(v_x - v_y);
                diff *= diff_gain;
                diff = fmaxf(diff, 1e-6f);
                diff = powf(diff, diff_gamma);

                float tmp1[4] = {v_x, v_y, v_y, v_x};
                tmp1[0] = (sinf(tmp1[0]) + 1.0f) * diff;
                tmp1[1] = (sinf(tmp1[1]) + 1.0f) * diff;
                tmp1[2] = (sinf(tmp1[2]) + 1.0f) * diff;
                tmp1[3] = (sinf(tmp1[3]) + 1.0f) * diff;

                o[0] += tmp1[0];
                o[1] += tmp1[1];
                o[2] += tmp1[2];
                o[3] += tmp1[3];
            }

            float tmp3[4];
            tmp3[0] = p_y;
            tmp3[1] = -p_y;
            tmp3[2] = -2.0f * p_y;
            tmp3[3] = 0.0f;

            const float l_adjust = l_val - 4.0f;
            tmp3[0] += l_adjust;
            tmp3[1] += l_adjust;
            tmp3[2] += l_adjust;
            tmp3[3] += l_adjust;

            tmp3[0] = expf(tmp3[0]) * 5.0f;
            tmp3[1] = expf(tmp3[1]) * 5.0f;
            tmp3[2] = expf(tmp3[2]) * 5.0f;
            tmp3[3] = expf(tmp3[3]) * 5.0f;

            for (int i = 0; i < 4; ++i) {
                const float denom = o[i];
                if (fabsf(denom) > CK_PLASMA_RENDERER_EPSILON) {
                    o[i] = tmp3[i] / denom;
                } else {
                    o[i] = 0.0f;
                }
            }

            float r = tanhf(o[0]);
            float g = tanhf(o[1]);
            float b = tanhf(o[2]);

            float r2 = clamp_unit(r + mix_rg * g + mix_rb * b);
            float g2 = clamp_unit(g + mix_gr * r + mix_gb * b);
            float b2 = clamp_unit(b + mix_br * r + mix_bg * g);

            unsigned char *pixel = dst + ((y * width + x) * 4);
            pixel[0] = (unsigned char)(r2 * 255.0f);
            pixel[1] = (unsigned char)(g2 * 255.0f);
            pixel[2] = (unsigned char)(b2 * 255.0f);
            pixel[3] = 0xFF;
        }
    }
}
