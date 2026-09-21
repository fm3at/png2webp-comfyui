/* csp_shim.c - software ARGB<->YUV420 conversions (replaces picture_csp_enc.c,
 * which requires the unavailable sharpyuv submodule).
 * Provides the symbols referenced by the rest of the encoder.
 */
#include <stdlib.h>
#include <string.h>
#include "webp/encode.h"
#include "src/enc/vp8i_enc.h"
#include "src/enc/vp8li_enc.h"
#include "src/dsp/dsp.h"
#include "src/dsp/lossless.h"
#include "src/dsp/yuv.h"

static int CheckNonOpaque(const uint8_t *a, int width, int height,
                          int step, int rgb_stride)
{
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++)
            if (a[i * step] != 255) return 1;
        a += rgb_stride;
    }
    return 0;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* libwebp reference RGB->YUV fixed point (see src/dsp/yuv.h, YUV_FIX=16) */
static int clip_uv(int uv, int rounding)
{
    uv = (uv + rounding + (128 << 18)) >> 18;
    return uv < 0 ? 0 : (uv > 255 ? 255 : uv);
}

static int rgb_to_y(int r, int g, int b)
{
    return (16839 * r + 33059 * g + 6420 * b + 32768 + (16 << 16)) >> 16;
}

static int rgb_to_u(int r, int g, int b)
{
    return clip_uv(-9719 * r - 19081 * g + 28800 * b, 131072);
}

static int rgb_to_v(int r, int g, int b)
{
    return clip_uv(28800 * r - 24116 * g - 4684 * b, 131072);
}

static int ConvertRGBAtoYUVA(const uint8_t *r, const uint8_t *g,
                             const uint8_t *b, const uint8_t *a,
                             int step, int rgb_stride, WebPPicture *picture)
{
    const int width = picture->width;
    const int height = picture->height;
    const int has_alpha = a ? CheckNonOpaque(a, width, height, step, rgb_stride) : 0;

    picture->colorspace = has_alpha ? WEBP_YUV420A : WEBP_YUV420;
    picture->use_argb = 0;
    if (!WebPPictureAllocYUVA(picture)) return 0;

    const int uv_w = (width + 1) >> 1;
    const int uv_h = (height + 1) >> 1;

    for (int j = 0; j < height; j++) {
        const uint8_t *row = r + (size_t)j * rgb_stride;
        const uint8_t *grow = g + (size_t)j * rgb_stride;
        const uint8_t *brow = b + (size_t)j * rgb_stride;
        uint8_t *dy = picture->y + (size_t)j * picture->y_stride;
        for (int i = 0; i < width; i++) {
            dy[i] = (uint8_t)rgb_to_y(row[i * step], grow[i * step],
                                      brow[i * step]);
        }
    }

    for (int j = 0; j < uv_h; j++) {
        const int r0 = 2 * j;
        const int r1 = (2 * j + 1 < height) ? 2 * j + 1 : 2 * j;
        const uint8_t *rr0 = r + (size_t)r0 * rgb_stride;
        const uint8_t *rr1 = r + (size_t)r1 * rgb_stride;
        const uint8_t *gg0 = g + (size_t)r0 * rgb_stride;
        const uint8_t *gg1 = g + (size_t)r1 * rgb_stride;
        const uint8_t *bb0 = b + (size_t)r0 * rgb_stride;
        const uint8_t *bb1 = b + (size_t)r1 * rgb_stride;
        uint8_t *du = picture->u + (size_t)j * picture->uv_stride;
        uint8_t *dv = picture->v + (size_t)j * picture->uv_stride;
        for (int i = 0; i < uv_w; i++) {
            const int c0 = 2 * i;
            const int c1 = (2 * i + 1 < width) ? 2 * i + 1 : 2 * i;
            int rs = rr0[c0 * step] + rr0[c1 * step] +
                     rr1[c0 * step] + rr1[c1 * step];
            int gs = gg0[c0 * step] + gg0[c1 * step] +
                     gg1[c0 * step] + gg1[c1 * step];
            int bs = bb0[c0 * step] + bb0[c1 * step] +
                     bb1[c0 * step] + bb1[c1 * step];
            du[i] = (uint8_t)rgb_to_u(rs, gs, bs);
            dv[i] = (uint8_t)rgb_to_v(rs, gs, bs);
        }
    }

    if (has_alpha) {
        for (int j = 0; j < height; j++) {
            const uint8_t *row = a + (size_t)j * rgb_stride;
            uint8_t *da = picture->a + (size_t)j * picture->a_stride;
            for (int i = 0; i < width; i++) da[i] = row[i * step];
        }
    }
    return 1;
}

static int PictureARGBToYUVA(WebPPicture *picture, WebPEncCSP colorspace,
                             float dithering, int iterative)
{
    (void)dithering;
    (void)iterative;
    if (picture == NULL) return 0;
    if (picture->argb == NULL)
        return WebPEncodingSetError(picture, VP8_ENC_ERROR_NULL_PARAMETER);
    if ((colorspace & WEBP_CSP_UV_MASK) != WEBP_YUV420)
        return WebPEncodingSetError(picture, VP8_ENC_ERROR_INVALID_CONFIGURATION);

    const uint8_t *const argb = (const uint8_t *)picture->argb;
    /* little-endian ARGB: byte order in memory is B,G,R,A */
    const uint8_t *const a = argb + 3;
    const uint8_t *const r = argb + 2;
    const uint8_t *const g = argb + 1;
    const uint8_t *const b = argb + 0;
    return ConvertRGBAtoYUVA(r, g, b, a, 4, 4 * picture->argb_stride, picture);
}

int WebPPictureSharpARGBToYUVA(WebPPicture *picture)
{
    return PictureARGBToYUVA(picture, WEBP_YUV420, 0.f, 1);
}

int WebPPictureARGBToYUVADithered(WebPPicture *picture, WebPEncCSP colorspace,
                                 float dithering)
{
    return PictureARGBToYUVA(picture, colorspace, dithering, 0);
}

int WebPPictureARGBToYUVA(WebPPicture *picture, WebPEncCSP colorspace)
{
    return PictureARGBToYUVA(picture, colorspace, 0.f, 0);
}

int WebPPictureSmartARGBToYUVA(WebPPicture *picture)
{
    return WebPPictureSharpARGBToYUVA(picture);
}

int WebPPictureYUVAToARGB(WebPPicture *picture)
{
    if (picture == NULL) return 0;
    if (picture->y == NULL || picture->u == NULL || picture->v == NULL)
        return WebPEncodingSetError(picture, VP8_ENC_ERROR_NULL_PARAMETER);
    if ((picture->colorspace & WEBP_CSP_ALPHA_BIT) && picture->a == NULL)
        return WebPEncodingSetError(picture, VP8_ENC_ERROR_NULL_PARAMETER);
    if ((picture->colorspace & WEBP_CSP_UV_MASK) != WEBP_YUV420)
        return WebPEncodingSetError(picture, VP8_ENC_ERROR_INVALID_CONFIGURATION);

    if (!WebPPictureAllocARGB(picture)) return 0;
    picture->use_argb = 1;

    const int w = picture->width;
    const int h = picture->height;
    const int y_stride = picture->y_stride;
    const int uv_stride = picture->uv_stride;
    const int a_stride = picture->a_stride;
    const int has_alpha = (picture->colorspace & WEBP_CSP_ALPHA_BIT) != 0;

    for (int j = 0; j < h; j++) {
        uint32_t *d = picture->argb + (size_t)j * picture->argb_stride;
        const uint8_t *dy = picture->y + (size_t)j * y_stride;
        const uint8_t *du = picture->u + (size_t)(j >> 1) * uv_stride;
        const uint8_t *dv = picture->v + (size_t)(j >> 1) * uv_stride;
        const uint8_t *da = has_alpha ? picture->a + (size_t)j * a_stride : NULL;
        for (int i = 0; i < w; i++) {
            uint8_t rgb[3];
            VP8YuvToRgb(dy[i], du[i >> 1] - 128, dv[i >> 1] - 128, rgb);
            uint32_t al = da ? (uint32_t)da[i] : 255;
            d[i] = (al << 24) | ((uint32_t)rgb[0] << 16) |
                   ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
        }
    }
    return 1;
}

int WebPPictureHasTransparency(const WebPPicture *picture)
{
    const int w = picture->width;
    const int h = picture->height;
    if (picture->use_argb) {
        for (int j = 0; j < h; j++) {
            const uint8_t *row = (const uint8_t *)picture->argb +
                                (size_t)j * 4 * picture->argb_stride + 3;
            for (int i = 0; i < w; i++)
                if (row[i * 4] != 255) return 1;
        }
        return 0;
    }
    if (picture->a != NULL) {
        for (int j = 0; j < h; j++) {
            const uint8_t *row = picture->a + (size_t)j * picture->a_stride;
            for (int i = 0; i < w; i++)
                if (row[i] != 255) return 1;
        }
    }
    return 0;
}

/* ---------------- Import functions ---------------- */

static int Import(WebPPicture *picture, const uint8_t *rgb, int rgb_stride,
                  int step, int swap_rb, int import_alpha)
{
    int y;
    const uint8_t *r_ptr = rgb + (swap_rb ? 2 : 0);
    const uint8_t *g_ptr = rgb + 1;
    const uint8_t *b_ptr = rgb + (swap_rb ? 0 : 2);
    const int width = picture->width;
    const int height = picture->height;

    if (abs(rgb_stride) < (import_alpha ? 4 : 3) * width) return 0;

    if (!picture->use_argb) {
        const uint8_t *a_ptr = import_alpha ? rgb + 3 : NULL;
        return ConvertRGBAtoYUVA(r_ptr, g_ptr, b_ptr, a_ptr, step, rgb_stride,
                                 picture);
    }
    if (!WebPPictureAlloc(picture)) return 0;

    VP8LDspInit();
    WebPInitAlphaProcessing();

    if (import_alpha) {
        uint32_t *dst = picture->argb;
        if (swap_rb) {
            /* B(G)RA input: memory order b,g,r,a matches little-endian dst */
            for (y = 0; y < height; ++y) {
                memcpy(dst, rgb, (size_t)width * 4);
                rgb += rgb_stride;
                dst += picture->argb_stride;
            }
        } else {
            for (y = 0; y < height; ++y) {
                VP8LConvertBGRAToRGBA((const uint32_t *)rgb, width,
                                      (uint8_t *)dst);
                rgb += rgb_stride;
                dst += picture->argb_stride;
            }
        }
    } else {
        uint32_t *dst = picture->argb;
        for (y = 0; y < height; ++y) {
            WebPPackRGB(r_ptr, g_ptr, b_ptr, width, step, dst);
            r_ptr += rgb_stride;
            g_ptr += rgb_stride;
            b_ptr += rgb_stride;
            dst += picture->argb_stride;
        }
    }
    return 1;
}

int WebPPictureImportBGR(WebPPicture *picture, const uint8_t *bgr, int bgr_stride)
{
    return (picture != NULL && bgr != NULL)
               ? Import(picture, bgr, bgr_stride, 3, 1, 0)
               : 0;
}

int WebPPictureImportBGRA(WebPPicture *picture, const uint8_t *bgra, int bgra_stride)
{
    return (picture != NULL && bgra != NULL)
               ? Import(picture, bgra, bgra_stride, 4, 1, 1)
               : 0;
}

int WebPPictureImportBGRX(WebPPicture *picture, const uint8_t *bgrx, int bgrx_stride)
{
    return (picture != NULL && bgrx != NULL)
               ? Import(picture, bgrx, bgrx_stride, 4, 1, 0)
               : 0;
}

int WebPPictureImportRGB(WebPPicture *picture, const uint8_t *rgb, int rgb_stride)
{
    return (picture != NULL && rgb != NULL)
               ? Import(picture, rgb, rgb_stride, 3, 0, 0)
               : 0;
}

int WebPPictureImportRGBA(WebPPicture *picture, const uint8_t *rgba, int rgba_stride)
{
    return (picture != NULL && rgba != NULL)
               ? Import(picture, rgba, rgba_stride, 4, 0, 1)
               : 0;
}

int WebPPictureImportRGBX(WebPPicture *picture, const uint8_t *rgbx, int rgbx_stride)
{
    return (picture != NULL && rgbx != NULL)
               ? Import(picture, rgbx, rgbx_stride, 4, 0, 0)
               : 0;
}
