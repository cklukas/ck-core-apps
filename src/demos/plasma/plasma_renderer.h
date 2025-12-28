#ifndef CK_PLASMA_RENDERER_H
#define CK_PLASMA_RENDERER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CK_PLASMA_RENDERER_TIME_STEPS 240
#define CK_PLASMA_RENDERER_ITERATIONS 8
#define CK_PLASMA_RENDERER_EPSILON 1e-6f

/**
 * Generate a single plasma frame using the enhanced cxsa approach.
 * @param dst         RGBA destination buffer (width * height * 4 bytes).
 * @param width       Width of the target image in pixels.
 * @param height      Height of the target image in pixels.
 * @param frame_index Frame counter used to animate the effect.
 * @param sequence_id Arbitrary run identifier (e.g. frame_index / TIME_STEPS) that
 *                    introduces seam-safe variation between loops.
 */
void ck_plasma_render_frame(unsigned char *dst, int width, int height,
                            int frame_index, int sequence_id);

#ifdef __cplusplus
}
#endif

#endif /* CK_PLASMA_RENDERER_H */
