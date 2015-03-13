/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include <libavutil/common.h>

#include "gl_video.h"

#include "misc/bstr.h"
#include "gl_common.h"
#include "gl_utils.h"
#include "gl_hwdec.h"
#include "gl_osd.h"
#include "filter_kernels.h"
#include "aspect.h"
#include "video/memcpy_pic.h"
#include "bitmap_packer.h"
#include "dither.h"

// Pixel width of 1D lookup textures.
#define LOOKUP_TEXTURE_SIZE 256

// Texture units 0-3 are used by the video, and for free use by the passes
// Units 4-5 are used for scaler LUTs.
#define TEXUNIT_SCALERS 4
#define TEXUNIT_3DLUT 6
#define TEXUNIT_DITHER 7

// scale/cscale arguments that map directly to shader filter routines.
// Note that the convolution filters are not included in this list.
static const char *const fixed_scale_filters[] = {
    "bilinear",
    "bicubic_fast",
    "sharpen3",
    "sharpen5",
    NULL
};

// must be sorted, and terminated with 0
// 2 & 6 are special-cased, the rest can be generated with WEIGHTS_N().
int filter_sizes[] =
    {2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 0};

struct vertex_pt {
    float x, y;
};

struct vertex {
    struct vertex_pt position;
    struct vertex_pt texcoord[4];
};

static const struct gl_vao_entry vertex_vao[] = {
    {"position", 2, GL_FLOAT, false, offsetof(struct vertex, position)},
    {"texcoord0", 2, GL_FLOAT, false, offsetof(struct vertex, texcoord[0])},
    {"texcoord1", 2, GL_FLOAT, false, offsetof(struct vertex, texcoord[1])},
    {"texcoord2", 2, GL_FLOAT, false, offsetof(struct vertex, texcoord[2])},
    {"texcoord3", 2, GL_FLOAT, false, offsetof(struct vertex, texcoord[3])},
    {0}
};

struct texplane {
    int w, h;
    int tex_w, tex_h;
    GLint gl_internal_format;
    GLenum gl_target;
    GLenum gl_format;
    GLenum gl_type;
    GLuint gl_texture;
    int gl_buffer;
    int buffer_size;
    void *buffer_ptr;
};

struct video_image {
    struct texplane planes[4];
    bool image_flipped;
    struct mp_image *mpi;       // original input image
};

struct scaler {
    int index;
    const char *name;
    double scale_factor;
    float params[2];
    float antiring;

    bool initialized;
    struct filter_kernel *kernel;
    GLuint gl_lut;
    GLenum gl_target;
    struct fbotex sep_fbo;
    bool insufficient;

    // kernel points here
    struct filter_kernel kernel_storage;
};

struct fbosurface {
    struct fbotex fbotex;
    int64_t pts;
};

#define FBOSURFACES_MAX 4

struct src_tex {
    GLuint gl_tex;
    GLenum gl_target;
    int tex_w, tex_h;
    struct mp_rect_f src;
};

struct gl_video {
    GL *gl;

    struct mp_log *log;
    struct gl_video_opts opts;
    bool gl_debug;

    int depth_g;
    int texture_16bit_depth;    // actual bits available in 16 bit textures

    struct gl_shader_cache *sc;

    GLenum gl_target; // texture target (GL_TEXTURE_2D, ...) for video and FBOs

    struct gl_vao vao;

    struct osd_state *osd_state;
    struct mpgl_osd *osd;
    double osd_pts;

    GLuint lut_3d_texture;
    bool use_lut_3d;

    GLuint dither_texture;
    int dither_size;

    struct mp_image_params real_image_params;   // configured format
    struct mp_image_params image_params;        // texture format (mind hwdec case)
    struct mp_imgfmt_desc image_desc;
    int plane_count;
    int image_w, image_h;

    bool is_yuv, is_rgb, is_packed_yuv;
    bool has_alpha;
    char color_swizzle[5];

    bool user_gamma_enabled;

    struct video_image image;

    struct fbotex indirect_fbo;         // RGB target
    struct fbotex chroma_merge_fbo;
    struct fbosurface surfaces[FBOSURFACES_MAX];

    size_t surface_idx;
    size_t surface_now;
    bool is_interpolated;

    // state for luma (0) and chroma (1) scalers
    struct scaler scalers[2];

    struct mp_csp_equalizer video_eq;

    struct mp_rect src_rect;    // displayed part of the source video
    struct mp_rect dst_rect;    // video rectangle on output window
    struct mp_osd_res osd_rect; // OSD size/margins
    int vp_w, vp_h;

    // temporary during rendering
    struct src_tex pass_tex[4];
    bool use_indirect;

    int frames_rendered;

    // Cached because computing it can take relatively long
    int last_dither_matrix_size;
    float *last_dither_matrix;

    struct gl_hwdec *hwdec;
    bool hwdec_active;
};

struct fmt_entry {
    int mp_format;
    GLint internal_format;
    GLenum format;
    GLenum type;
};

// Very special formats, for which OpenGL happens to have direct support
static const struct fmt_entry mp_to_gl_formats[] = {
    {IMGFMT_BGR555,  GL_RGBA,  GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    {IMGFMT_BGR565,  GL_RGB,   GL_RGB,  GL_UNSIGNED_SHORT_5_6_5_REV},
    {IMGFMT_RGB555,  GL_RGBA,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV},
    {IMGFMT_RGB565,  GL_RGB,   GL_RGB,  GL_UNSIGNED_SHORT_5_6_5},
    {0},
};

static const struct fmt_entry gl_byte_formats[] = {
    {0, GL_RED,     GL_RED,     GL_UNSIGNED_BYTE},      // 1 x 8
    {0, GL_RG,      GL_RG,      GL_UNSIGNED_BYTE},      // 2 x 8
    {0, GL_RGB,     GL_RGB,     GL_UNSIGNED_BYTE},      // 3 x 8
    {0, GL_RGBA,    GL_RGBA,    GL_UNSIGNED_BYTE},      // 4 x 8
    {0, GL_R16,     GL_RED,     GL_UNSIGNED_SHORT},     // 1 x 16
    {0, GL_RG16,    GL_RG,      GL_UNSIGNED_SHORT},     // 2 x 16
    {0, GL_RGB16,   GL_RGB,     GL_UNSIGNED_SHORT},     // 3 x 16
    {0, GL_RGBA16,  GL_RGBA,    GL_UNSIGNED_SHORT},     // 4 x 16
};

static const struct fmt_entry gl_byte_formats_gles3[] = {
    {0, GL_R8,       GL_RED,    GL_UNSIGNED_BYTE},      // 1 x 8
    {0, GL_RG8,      GL_RG,     GL_UNSIGNED_BYTE},      // 2 x 8
    {0, GL_RGB8,     GL_RGB,    GL_UNSIGNED_BYTE},      // 3 x 8
    {0, GL_RGBA8,    GL_RGBA,   GL_UNSIGNED_BYTE},      // 4 x 8
    // There are no filterable texture formats that can be uploaded as
    // GL_UNSIGNED_SHORT, so apparently we're out of luck.
    {0, 0,           0,         0},                     // 1 x 16
    {0, 0,           0,         0},                     // 2 x 16
    {0, 0,           0,         0},                     // 3 x 16
    {0, 0,           0,         0},                     // 4 x 16
};

static const struct fmt_entry gl_byte_formats_gles2[] = {
    {0, GL_LUMINANCE,           GL_LUMINANCE,       GL_UNSIGNED_BYTE}, // 1 x 8
    {0, GL_LUMINANCE_ALPHA,     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE}, // 2 x 8
    {0, GL_RGB,                 GL_RGB,             GL_UNSIGNED_BYTE}, // 3 x 8
    {0, GL_RGBA,                GL_RGBA,            GL_UNSIGNED_BYTE}, // 4 x 8
    {0, 0,                      0,                  0},                // 1 x 16
    {0, 0,                      0,                  0},                // 2 x 16
    {0, 0,                      0,                  0},                // 3 x 16
    {0, 0,                      0,                  0},                // 4 x 16
};

static const struct fmt_entry gl_byte_formats_legacy[] = {
    {0, GL_LUMINANCE,           GL_LUMINANCE,       GL_UNSIGNED_BYTE}, // 1 x 8
    {0, GL_LUMINANCE_ALPHA,     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE}, // 2 x 8
    {0, GL_RGB,                 GL_RGB,             GL_UNSIGNED_BYTE}, // 3 x 8
    {0, GL_RGBA,                GL_RGBA,            GL_UNSIGNED_BYTE}, // 4 x 8
    {0, GL_LUMINANCE16,         GL_LUMINANCE,       GL_UNSIGNED_SHORT},// 1 x 16
    {0, GL_LUMINANCE16_ALPHA16, GL_LUMINANCE_ALPHA, GL_UNSIGNED_SHORT},// 2 x 16
    {0, GL_RGB16,               GL_RGB,             GL_UNSIGNED_SHORT},// 3 x 16
    {0, GL_RGBA16,              GL_RGBA,            GL_UNSIGNED_SHORT},// 4 x 16
};

static const struct fmt_entry gl_float16_formats[] = {
    {0, GL_R16F,    GL_RED,     GL_FLOAT},              // 1 x f
    {0, GL_RG16F,   GL_RG,      GL_FLOAT},              // 2 x f
    {0, GL_RGB16F,  GL_RGB,     GL_FLOAT},              // 3 x f
    {0, GL_RGBA16F, GL_RGBA,    GL_FLOAT},              // 4 x f
};

static const struct fmt_entry gl_apple_formats[] = {
    {IMGFMT_UYVY, GL_RGB, GL_RGB_422_APPLE, GL_UNSIGNED_SHORT_8_8_APPLE},
    {IMGFMT_YUYV, GL_RGB, GL_RGB_422_APPLE, GL_UNSIGNED_SHORT_8_8_REV_APPLE},
    {0}
};

struct packed_fmt_entry {
    int fmt;
    int8_t component_size;
    int8_t components[4]; // source component - 0 means unmapped
};

static const struct packed_fmt_entry mp_packed_formats[] = {
    //                  w   R  G  B  A
    {IMGFMT_Y8,         1, {1, 0, 0, 0}},
    {IMGFMT_Y16,        2, {1, 0, 0, 0}},
    {IMGFMT_YA8,        1, {1, 0, 0, 2}},
    {IMGFMT_YA16,       2, {1, 0, 0, 2}},
    {IMGFMT_ARGB,       1, {2, 3, 4, 1}},
    {IMGFMT_0RGB,       1, {2, 3, 4, 0}},
    {IMGFMT_BGRA,       1, {3, 2, 1, 4}},
    {IMGFMT_BGR0,       1, {3, 2, 1, 0}},
    {IMGFMT_ABGR,       1, {4, 3, 2, 1}},
    {IMGFMT_0BGR,       1, {4, 3, 2, 0}},
    {IMGFMT_RGBA,       1, {1, 2, 3, 4}},
    {IMGFMT_RGB0,       1, {1, 2, 3, 0}},
    {IMGFMT_BGR24,      1, {3, 2, 1, 0}},
    {IMGFMT_RGB24,      1, {1, 2, 3, 0}},
    {IMGFMT_RGB48,      2, {1, 2, 3, 0}},
    {IMGFMT_RGBA64,     2, {1, 2, 3, 4}},
    {IMGFMT_BGRA64,     2, {3, 2, 1, 4}},
    {0},
};

static const char *const osd_shaders[SUBBITMAP_COUNT] = {
    [SUBBITMAP_LIBASS] = "frag_osd_libass",
    [SUBBITMAP_RGBA] =   "frag_osd_rgba",
};

const struct gl_video_opts gl_video_opts_def = {
    .npot = 1,
    .dither_depth = -1,
    .dither_size = 6,
    .fbo_format = GL_RGBA,
    .sigmoid_center = 0.75,
    .sigmoid_slope = 6.5,
    .scalers = { "bilinear", "bilinear" },
    .dscaler = "bilinear",
    .scaler_params = {{NAN, NAN}, {NAN, NAN}},
    .scaler_radius = {3, 3},
    .alpha_mode = 2,
    .background = {0, 0, 0, 255},
    .gamma = 1.0f,
};

const struct gl_video_opts gl_video_opts_hq_def = {
    .npot = 1,
    .dither_depth = 0,
    .dither_size = 6,
    .fbo_format = GL_RGBA16,
    .fancy_downscaling = 1,
    .sigmoid_center = 0.75,
    .sigmoid_slope = 6.5,
    .sigmoid_upscaling = 1,
    .scalers = { "spline36", "bilinear" },
    .dscaler = "mitchell",
    .scaler_params = {{NAN, NAN}, {NAN, NAN}},
    .scaler_radius = {3, 3},
    .alpha_mode = 2,
    .background = {0, 0, 0, 255},
    .gamma = 1.0f,
};

static int validate_scaler_opt(struct mp_log *log, const m_option_t *opt,
                               struct bstr name, struct bstr param);

#define OPT_BASE_STRUCT struct gl_video_opts
const struct m_sub_options gl_video_conf = {
    .opts = (const m_option_t[]) {
        OPT_FLOATRANGE("gamma", gamma, 0, 0.1, 2.0),
        OPT_FLAG("gamma-auto", gamma_auto, 0),
        OPT_CHOICE("target-prim", target_prim, 0,
                   ({"auto",      MP_CSP_PRIM_AUTO},
                    {"bt601-525", MP_CSP_PRIM_BT_601_525},
                    {"bt601-625", MP_CSP_PRIM_BT_601_625},
                    {"bt709",     MP_CSP_PRIM_BT_709},
                    {"bt2020",    MP_CSP_PRIM_BT_2020},
                    {"bt470m",    MP_CSP_PRIM_BT_470M})),
        OPT_CHOICE("target-trc", target_trc, 0,
                   ({"auto",    MP_CSP_TRC_AUTO},
                    {"bt1886",  MP_CSP_TRC_BT_1886},
                    {"srgb",    MP_CSP_TRC_SRGB},
                    {"linear",  MP_CSP_TRC_LINEAR},
                    {"gamma22", MP_CSP_TRC_GAMMA22})),
        OPT_FLAG("npot", npot, 0),
        OPT_FLAG("pbo", pbo, 0),
        OPT_STRING_VALIDATE("scale", scalers[0], 0, validate_scaler_opt),
        OPT_STRING_VALIDATE("cscale", scalers[1], 0, validate_scaler_opt),
        OPT_STRING_VALIDATE("scale-down", dscaler, 0, validate_scaler_opt),
        OPT_FLOAT("scale-param1", scaler_params[0][0], 0),
        OPT_FLOAT("scale-param2", scaler_params[0][1], 0),
        OPT_FLOAT("cscale-param1", scaler_params[1][0], 0),
        OPT_FLOAT("cscale-param2", scaler_params[1][1], 0),
        OPT_FLOATRANGE("scale-radius", scaler_radius[0], 0, 1.0, 16.0),
        OPT_FLOATRANGE("cscale-radius", scaler_radius[1], 0, 1.0, 16.0),
        OPT_FLOATRANGE("scale-antiring", scaler_antiring[0], 0, 0.0, 1.0),
        OPT_FLOATRANGE("cscale-antiring", scaler_antiring[1], 0, 0.0, 1.0),
        OPT_FLAG("scaler-resizes-only", scaler_resizes_only, 0),
        OPT_FLAG("linear-scaling", linear_scaling, 0),
        OPT_FLAG("fancy-downscaling", fancy_downscaling, 0),
        OPT_FLAG("sigmoid-upscaling", sigmoid_upscaling, 0),
        OPT_FLOATRANGE("sigmoid-center", sigmoid_center, 0, 0.0, 1.0),
        OPT_FLOATRANGE("sigmoid-slope", sigmoid_slope, 0, 1.0, 20.0),
        OPT_CHOICE("fbo-format", fbo_format, 0,
                   ({"rgb",    GL_RGB},
                    {"rgba",   GL_RGBA},
                    {"rgb8",   GL_RGB8},
                    {"rgb10",  GL_RGB10},
                    {"rgb10_a2", GL_RGB10_A2},
                    {"rgb16",  GL_RGB16},
                    {"rgb16f", GL_RGB16F},
                    {"rgb32f", GL_RGB32F},
                    {"rgba12", GL_RGBA12},
                    {"rgba16", GL_RGBA16},
                    {"rgba16f", GL_RGBA16F},
                    {"rgba32f", GL_RGBA32F})),
        OPT_CHOICE_OR_INT("dither-depth", dither_depth, 0, -1, 16,
                          ({"no", -1}, {"auto", 0})),
        OPT_CHOICE("dither", dither_algo, 0,
                   ({"fruit", 0}, {"ordered", 1}, {"no", -1})),
        OPT_INTRANGE("dither-size-fruit", dither_size, 0, 2, 8),
        OPT_FLAG("temporal-dither", temporal_dither, 0),
        OPT_CHOICE("chroma-location", chroma_location, 0,
                   ({"auto",   MP_CHROMA_AUTO},
                    {"center", MP_CHROMA_CENTER},
                    {"left",   MP_CHROMA_LEFT})),
        OPT_CHOICE("alpha", alpha_mode, 0,
                   ({"no", 0},
                    {"yes", 1},
                    {"blend", 2})),
        OPT_FLAG("rectangle-textures", use_rectangle, 0),
        OPT_COLOR("background", background, 0),
        OPT_FLAG("smoothmotion", smoothmotion, 0),
        OPT_FLOAT("smoothmotion-threshold", smoothmotion_threshold,
                   CONF_RANGE, .min = 0, .max = 0.5),
        OPT_REMOVED("approx-gamma", "this is always enabled now"),
        OPT_REMOVED("cscale-down", "chroma is never downscaled"),
        OPT_REMOVED("scale-sep", "this is set automatically whenever sane"),
        OPT_REMOVED("indirect", "this is set automatically whenever sane"),

        OPT_REPLACED("lscale", "scale"),
        OPT_REPLACED("lscale-down", "scale-down"),
        OPT_REPLACED("lparam1", "scale-param1"),
        OPT_REPLACED("lparam2", "scale-param2"),
        OPT_REPLACED("lradius", "scale-radius"),
        OPT_REPLACED("lantiring", "scale-antiring"),
        OPT_REPLACED("cparam1", "cscale-param1"),
        OPT_REPLACED("cparam2", "cscale-param2"),
        OPT_REPLACED("cradius", "cscale-radius"),
        OPT_REPLACED("cantiring", "cscale-antiring"),
        OPT_REPLACED("srgb", "target-prim=srgb:target-trc=srgb"),

        {0}
    },
    .size = sizeof(struct gl_video_opts),
    .defaults = &gl_video_opts_def,
};

static void uninit_rendering(struct gl_video *p);
static void uninit_scaler(struct gl_video *p, int scaler_unit);
static void check_gl_features(struct gl_video *p);
static bool init_format(int fmt, struct gl_video *init);

#define GLSL(x) gl_sc_add(p->sc, #x "\n");
#define GLSLF(...) gl_sc_addf(p->sc, __VA_ARGS__)

static const struct fmt_entry *find_tex_format(GL *gl, int bytes_per_comp,
                                               int n_channels)
{
    assert(bytes_per_comp == 1 || bytes_per_comp == 2);
    assert(n_channels >= 1 && n_channels <= 4);
    const struct fmt_entry *fmts = gl_byte_formats;
    if (gl->es >= 300) {
        fmts = gl_byte_formats_gles3;
    } else if (gl->es) {
        fmts = gl_byte_formats_gles2;
    } else if (!(gl->mpgl_caps & MPGL_CAP_TEX_RG)) {
        fmts = gl_byte_formats_legacy;
    }
    return &fmts[n_channels - 1 + (bytes_per_comp - 1) * 4];
}

static void debug_check_gl(struct gl_video *p, const char *msg)
{
    if (p->gl_debug)
        glCheckError(p->gl, p->log, msg);
}

void gl_video_set_debug(struct gl_video *p, bool enable)
{
    GL *gl = p->gl;

    p->gl_debug = enable;
    if (p->gl->debug_context)
        gl_set_debug_logger(gl, enable ? p->log : NULL);
}

static void gl_video_reset_surfaces(struct gl_video *p)
{
    for (int i = 0; i < FBOSURFACES_MAX; i++)
        p->surfaces[i].pts = 0;
    p->surface_idx = 0;
    p->surface_now = 0;
}

static size_t fbosurface_next(size_t id)
{
    return (id+1) % FBOSURFACES_MAX;
}

static void recreate_osd(struct gl_video *p)
{
    if (p->osd)
        mpgl_osd_destroy(p->osd);
    p->osd = mpgl_osd_init(p->gl, p->log, p->osd_state);
    mpgl_osd_set_options(p->osd, p->opts.pbo);
}

static void reinit_rendering(struct gl_video *p)
{
    MP_VERBOSE(p, "Reinit rendering.\n");

    debug_check_gl(p, "before scaler initialization");

    uninit_rendering(p);

    recreate_osd(p);
}

static void uninit_rendering(struct gl_video *p)
{
    GL *gl = p->gl;

    for (int n = 0; n < 2; n++)
        uninit_scaler(p, n);

    gl->DeleteTextures(1, &p->dither_texture);
    p->dither_texture = 0;

    gl_video_reset_surfaces(p);
}

void gl_video_set_lut3d(struct gl_video *p, struct lut3d *lut3d)
{
    GL *gl = p->gl;

    if (!lut3d) {
        if (p->use_lut_3d) {
            p->use_lut_3d = false;
            reinit_rendering(p);
        }
        return;
    }

    if (!(gl->mpgl_caps & MPGL_CAP_3D_TEX))
        return;

    if (!p->lut_3d_texture)
        gl->GenTextures(1, &p->lut_3d_texture);

    gl->ActiveTexture(GL_TEXTURE0 + TEXUNIT_3DLUT);
    gl->BindTexture(GL_TEXTURE_3D, p->lut_3d_texture);
    gl->TexImage3D(GL_TEXTURE_3D, 0, GL_RGB16, lut3d->size[0], lut3d->size[1],
                   lut3d->size[2], 0, GL_RGB, GL_UNSIGNED_SHORT, lut3d->data);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl->ActiveTexture(GL_TEXTURE0);

    p->use_lut_3d = true;
    check_gl_features(p);

    debug_check_gl(p, "after 3d lut creation");

    reinit_rendering(p);
}

static void pass_load_fbotex(struct gl_video *p, struct fbotex *src_fbo, int id,
                             int w, int h)
{
    p->pass_tex[id] = (struct src_tex){
        .gl_tex = src_fbo->texture,
        .gl_target = GL_TEXTURE_2D,
        .tex_w = src_fbo->tex_w,
        .tex_h = src_fbo->tex_h,
        .src = {0, 0, w, h},
    };
}

static void pass_set_image_textures(struct gl_video *p, struct video_image *vimg,
                                    float chroma[3][2])
{
    GLuint imgtex[4] = {0};

    assert(vimg->mpi);

    float ls_w = 1.0 / (1 << p->image_desc.chroma_xs);
    float ls_h = 1.0 / (1 << p->image_desc.chroma_ys);

    int chroma_loc = p->opts.chroma_location;
    if (!chroma_loc)
        chroma_loc = p->image_params.chroma_location;
    if (chroma_loc != MP_CHROMA_CENTER) {
        int cx, cy;
        mp_get_chroma_location(chroma_loc, &cx, &cy);
        // By default texture coordinates are such that chroma is centered with
        // any chroma subsampling. If a specific direction is given, make it
        // so that the luma and chroma sample line up exactly.
        // For 4:4:4, setting chroma location should have no effect at all.
        // luma sample size (in chroma coord. space)
        chroma[2][0] = ls_w < 1 ? ls_w * -cx / 2 : 0;
        chroma[2][1] = ls_h < 1 ? ls_h * -cy / 2 : 0;
    } else {
        chroma[2][0] = chroma[2][1] = 0.0;
    }

    // Make sure luma/chroma sizes are aligned.
    // Example: For 4:2:0 with size 3x3, the subsampled chroma plane is 2x2
    // so luma (3,3) has to align with chroma (2,2).
    chroma[0][0] = ls_w * (float)vimg->planes[0].tex_w
                               / vimg->planes[1].tex_w;
    chroma[1][1] = ls_h * (float)vimg->planes[0].tex_h
                               / vimg->planes[1].tex_h;
    chroma[0][1] = chroma[1][0] = 0.0; // No rotation etc.

    if (p->hwdec_active) {
        p->hwdec->driver->map_image(p->hwdec, vimg->mpi, imgtex);
    } else {
        for (int n = 0; n < p->plane_count; n++)
            imgtex[n] = vimg->planes[n].gl_texture;
    }

    for (int n = 0; n < 4; n++) {
        struct texplane *t = &vimg->planes[n];
        p->pass_tex[n] = (struct src_tex){
            .gl_tex = imgtex[n],
            .gl_target = t->gl_target,
            .tex_w = t->tex_w,
            .tex_h = t->tex_h,
            .src = {0, 0, t->w, t->h},
        };
    }
}

static int align_pow2(int s)
{
    int r = 1;
    while (r < s)
        r *= 2;
    return r;
}

static void init_video(struct gl_video *p)
{
    GL *gl = p->gl;

    check_gl_features(p);

    init_format(p->image_params.imgfmt, p);
    p->gl_target = p->opts.use_rectangle ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D;

    if (p->hwdec_active) {
        if (p->hwdec->driver->reinit(p->hwdec, &p->image_params) < 0)
            MP_ERR(p, "Initializing texture for hardware decoding failed.\n");
        init_format(p->image_params.imgfmt, p);
        p->gl_target = p->hwdec->gl_texture_target;
    }

    mp_image_params_guess_csp(&p->image_params);

    p->image_w = p->image_params.w;
    p->image_h = p->image_params.h;

    int eq_caps = MP_CSP_EQ_CAPS_GAMMA;
    if (p->is_yuv && p->image_params.colorspace != MP_CSP_BT_2020_C)
        eq_caps |= MP_CSP_EQ_CAPS_COLORMATRIX;
    if (p->image_desc.flags & MP_IMGFLAG_XYZ)
        eq_caps |= MP_CSP_EQ_CAPS_BRIGHTNESS;
    p->video_eq.capabilities = eq_caps;

    debug_check_gl(p, "before video texture creation");

    struct video_image *vimg = &p->image;

    for (int n = 0; n < p->plane_count; n++) {
        struct texplane *plane = &vimg->planes[n];

        plane->gl_target = p->gl_target;

        plane->w = mp_chroma_div_up(p->image_w, p->image_desc.xs[n]);
        plane->h = mp_chroma_div_up(p->image_h, p->image_desc.ys[n]);

        plane->tex_w = plane->w;
        plane->tex_h = plane->h;

        if (!p->hwdec_active) {
            if (!p->opts.npot) {
                plane->tex_w = align_pow2(plane->tex_w);
                plane->tex_h = align_pow2(plane->tex_h);
            }

            gl->ActiveTexture(GL_TEXTURE0 + n);
            gl->GenTextures(1, &plane->gl_texture);
            gl->BindTexture(p->gl_target, plane->gl_texture);

            gl->TexImage2D(p->gl_target, 0, plane->gl_internal_format,
                           plane->tex_w, plane->tex_h, 0,
                           plane->gl_format, plane->gl_type, NULL);

            gl->TexParameteri(p->gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->TexParameteri(p->gl_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->TexParameteri(p->gl_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->TexParameteri(p->gl_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        MP_VERBOSE(p, "Texture for plane %d: %dx%d\n",
                   n, plane->tex_w, plane->tex_h);
    }
    gl->ActiveTexture(GL_TEXTURE0);

    debug_check_gl(p, "after video texture creation");

    reinit_rendering(p);
}

static void uninit_video(struct gl_video *p)
{
    GL *gl = p->gl;

    uninit_rendering(p);

    struct video_image *vimg = &p->image;

    for (int n = 0; n < 3; n++) {
        struct texplane *plane = &vimg->planes[n];

        gl->DeleteTextures(1, &plane->gl_texture);
        plane->gl_texture = 0;
        gl->DeleteBuffers(1, &plane->gl_buffer);
        plane->gl_buffer = 0;
        plane->buffer_ptr = NULL;
        plane->buffer_size = 0;
    }
    mp_image_unrefp(&vimg->mpi);

    // Invalidate image_params to ensure that gl_video_config() will call
    // init_video() on uninitialized gl_video.
    p->real_image_params = (struct mp_image_params){0};
    p->image_params = p->real_image_params;
}

static void pass_prepare_src_tex(struct gl_video *p)
{
    GL *gl = p->gl;
    struct gl_shader_cache *sc = p->sc;

    for (int n = 0; n < 4; n++) {
        struct src_tex *s = &p->pass_tex[n];
        if (!s->gl_tex)
            continue;

        char texture_name[32];
        char texture_size[32];
        snprintf(texture_name, sizeof(texture_name), "texture%d", n);
        snprintf(texture_size, sizeof(texture_size), "texture_size%d", n);

        gl_sc_uniform_sampler(sc, texture_name, s->gl_target, n);
        float f[2] = {1, 1};
        if (s->gl_target != GL_TEXTURE_RECTANGLE) {
            f[0] = s->tex_w;
            f[1] = s->tex_h;
        }
        gl_sc_uniform_vec2(sc, texture_size, f);

        gl->ActiveTexture(GL_TEXTURE0 + n);
        gl->BindTexture(s->gl_target, s->gl_tex);
    }
    gl->ActiveTexture(GL_TEXTURE0);
}

// flags = bits 0-1: rotate, bit 2: flip vertically
static void render_pass_quad(struct gl_video *p, int vp_w, int vp_h,
                             const struct mp_rect *dst, int flags)
{
    struct vertex va[4];

    float matrix[3][2];
    gl_matrix_ortho2d(matrix, 0, vp_w, 0, vp_h);

    float x[2] = {dst->x0, dst->x1};
    float y[2] = {dst->y0, dst->y1};
    gl_matrix_mul_vec(matrix, &x[0], &y[0]);
    gl_matrix_mul_vec(matrix, &x[1], &y[1]);

    for (int n = 0; n < 4; n++) {
        struct vertex *v = &va[n];
        v->position.x = x[n / 2];
        v->position.y = y[n % 2];
        for (int i = 0; i < 4; i++) {
            struct src_tex *s = &p->pass_tex[i];
            if (s->gl_tex) {
                float tx[2] = {s->src.x0, s->src.x1};
                float ty[2] = {s->src.y0, s->src.y1};
                if (flags & 4)
                    MPSWAP(float, ty[0], ty[1]);
                bool rect = s->gl_target == GL_TEXTURE_RECTANGLE;
                v->texcoord[i].x = tx[n / 2] / (rect ? 1 : s->tex_w);
                v->texcoord[i].y = ty[n % 2] / (rect ? 1 : s->tex_h);
            }
        }
    }

    int rot = flags & 3;
    while (rot--) {
        static const int perm[4] = {1, 3, 0, 2};
        struct vertex vb[4];
        memcpy(vb, va, sizeof(vb));
        for (int n = 0; n < 4; n++)
            memcpy(va[n].texcoord, vb[perm[n]].texcoord,
                   sizeof(struct vertex_pt[4]));
    }

    gl_vao_draw_data(&p->vao, GL_TRIANGLE_STRIP, va, 4);

    debug_check_gl(p, "after rendering");
}

// flags: see render_pass_quad
static void finish_pass_direct(struct gl_video *p, GLint fbo, int vp_w, int vp_h,
                               const struct mp_rect *dst, int flags)
{
    GL *gl = p->gl;
    pass_prepare_src_tex(p);
    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->Viewport(0, 0, vp_w, vp_h < 0 ? -vp_h : vp_h);
    gl_sc_gen_shader_and_reset(p->sc);
    render_pass_quad(p, vp_w, vp_h, dst, flags);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
    memset(&p->pass_tex, 0, sizeof(p->pass_tex));
}

// dst_fbo: this will be used for rendering; possibly reallocating the whole
//          FBO, if the required parameters have changed
// w, h: required FBO target dimension, and also defines the target rectangle
//       used for rasterization
// tex: the texture unit to load the result back into
// flags: 0 or combination of FBOTEX_FUZZY_W/FBOTEX_FUZZY_H (setting the fuzzy
//        flags allows the FBO to be larger than the w/h parameters)
static void finish_pass_fbo(struct gl_video *p, struct fbotex *dst_fbo,
                            int w, int h, int tex, int flags)
{
    fbotex_change(dst_fbo, p->gl, p->log, w, h, p->opts.fbo_format, flags);

    finish_pass_direct(p, dst_fbo->fbo, dst_fbo->tex_w, dst_fbo->tex_h,
                       &(struct mp_rect){0, 0, w, h}, 0);
    pass_load_fbotex(p, dst_fbo, tex, w, h);
}

static void uninit_scaler(struct gl_video *p, int scaler_unit)
{
    GL *gl = p->gl;
    struct scaler *scaler = &p->scalers[scaler_unit];

    gl->DeleteTextures(1, &scaler->gl_lut);
    scaler->gl_lut = 0;
    scaler->kernel = NULL;
    scaler->initialized = false;
}

static void reinit_scaler(struct gl_video *p, int scaler_unit, const char *name,
                          double scale_factor)
{
    GL *gl = p->gl;
    struct scaler *scaler = &p->scalers[scaler_unit];

    if (scaler->name && strcmp(scaler->name, name) == 0 &&
        scaler->scale_factor == scale_factor &&
        scaler->initialized)
        return;

    uninit_scaler(p, scaler_unit);

    scaler->name = name;
    scaler->scale_factor = scale_factor;
    scaler->insufficient = false;
    scaler->initialized = true;

    for (int n = 0; n < 2; n++)
        scaler->params[n] = p->opts.scaler_params[scaler->index][n];

    const struct filter_kernel *t_kernel = mp_find_filter_kernel(scaler->name);
    if (!t_kernel)
        return;

    scaler->kernel_storage = *t_kernel;
    scaler->kernel = &scaler->kernel_storage;

    for (int n = 0; n < 2; n++) {
        if (!isnan(scaler->params[n]))
            scaler->kernel->params[n] = scaler->params[n];
    }

    scaler->antiring = p->opts.scaler_antiring[scaler->index];

    if (scaler->kernel->radius < 0)
        scaler->kernel->radius = p->opts.scaler_radius[scaler->index];

    scaler->insufficient = !mp_init_filter(scaler->kernel, filter_sizes,
                                           scale_factor);

    if (scaler->kernel->polar) {
        scaler->gl_target = GL_TEXTURE_1D;
    } else {
        scaler->gl_target = GL_TEXTURE_2D;
    }

    int size = scaler->kernel->size;
    int elems_per_pixel = 4;
    if (size == 1) {
        elems_per_pixel = 1;
    } else if (size == 2) {
        elems_per_pixel = 2;
    } else if (size == 6) {
        elems_per_pixel = 3;
    }
    int width = size / elems_per_pixel;
    assert(size == width * elems_per_pixel);
    const struct fmt_entry *fmt = &gl_float16_formats[elems_per_pixel - 1];
    GLenum target = scaler->gl_target;

    gl->ActiveTexture(GL_TEXTURE0 + TEXUNIT_SCALERS + scaler->index);

    if (!scaler->gl_lut)
        gl->GenTextures(1, &scaler->gl_lut);

    gl->BindTexture(target, scaler->gl_lut);

    float *weights = talloc_array(NULL, float, LOOKUP_TEXTURE_SIZE * size);
    mp_compute_lut(scaler->kernel, LOOKUP_TEXTURE_SIZE, weights);

    if (target == GL_TEXTURE_1D) {
        gl->TexImage1D(target, 0, fmt->internal_format, LOOKUP_TEXTURE_SIZE,
                       0, fmt->format, GL_FLOAT, weights);
    } else {
        gl->TexImage2D(target, 0, fmt->internal_format, width, LOOKUP_TEXTURE_SIZE,
                       0, fmt->format, GL_FLOAT, weights);
    }

    talloc_free(weights);

    gl->TexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    if (target != GL_TEXTURE_1D)
        gl->TexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl->ActiveTexture(GL_TEXTURE0);

    debug_check_gl(p, "after initializing scaler");
}

static void pass_sample_separated_get_weights(struct gl_video *p,
                                              struct scaler *scaler)
{
    gl_sc_uniform_sampler(p->sc, "lut", scaler->gl_target,
                          TEXUNIT_SCALERS + scaler->index);

    int N = scaler->kernel->size;
    if (N == 2) {
        GLSL(vec2 c1 = texture(lut, vec2(0.5, fcoord)).RG;)
        GLSL(float weights[2] = float[](c1.r, c1.g);)
    } else if (N == 6) {
        GLSL(vec4 c1 = texture(lut, vec2(0.25, fcoord));)
        GLSL(vec4 c2 = texture(lut, vec2(0.75, fcoord));)
        GLSL(float weights[6] = float[](c1.r, c1.g, c1.b, c2.r, c2.g, c2.b);)
    } else {
        GLSLF("float weights[%d];\n", N);
        for (int n = 0; n < N / 4; n++) {
            GLSLF("c = texture(lut, vec2(1.0 / %d + %d / float(%d), fcoord));\n",
                    N / 2, n, N / 4);
            GLSLF("weights[%d] = c.r;\n", n * 4 + 0);
            GLSLF("weights[%d] = c.g;\n", n * 4 + 1);
            GLSLF("weights[%d] = c.b;\n", n * 4 + 2);
            GLSLF("weights[%d] = c.a;\n", n * 4 + 3);
        }
    }
}

// Handle a single pass (either vertical or horizontal). The direction is given
// by the vector (d_x, d_y)
static void pass_sample_separated_gen(struct gl_video *p, struct scaler *scaler,
                                      int d_x, int d_y)
{
    int N = scaler->kernel->size;
    bool use_ar = scaler->antiring > 0;
    GLSL(vec4 color = vec4(0.0);)
    GLSLF("{\n");
    GLSLF("vec2 dir = vec2(%d, %d);\n", d_x, d_y);
    GLSL(vec2 pt = (vec2(1.0) / sample_size) * dir;)
    GLSL(float fcoord = dot(fract(sample_pos * sample_size - vec2(0.5)), dir);)
    GLSLF("vec2 base = sample_pos - fcoord * pt - pt * vec2(%d);\n", N / 2 - 1);
    GLSL(vec4 c;)
    if (use_ar) {
        GLSL(vec4 hi = vec4(0.0);)
        GLSL(vec4 lo = vec4(1.0);)
    }
    pass_sample_separated_get_weights(p, scaler);
    GLSLF("// scaler samples\n");
    for (int n = 0; n < N; n++) {
        GLSLF("c = texture(sample_tex, base + pt * vec2(%d));\n", n);
        GLSLF("color += vec4(weights[%d]) * c;\n", n);
        if (use_ar && (n == N/2-1 || n == N/2)) {
            GLSL(lo = min(lo, c);)
            GLSL(hi = max(hi, c);)
        }
    }
    if (use_ar)
        GLSLF("color = mix(color, clamp(color, lo, hi), %f);\n", scaler->antiring);
    GLSLF("}\n");
}

static void pass_sample_separated(struct gl_video *p, int src_tex,
                                  struct scaler *scaler, int w, int h,
                                  float transform[3][2])
{
    // Keep the x components untouched for the first pass
    struct mp_rect_f src_new = p->pass_tex[src_tex].src;
    gl_matrix_mul_rect(transform, &src_new);
    GLSLF("// pass 1\n");
    p->pass_tex[src_tex].src.y0 = src_new.y0;
    p->pass_tex[src_tex].src.y1 = src_new.y1;
    pass_sample_separated_gen(p, scaler, 0, 1);
    int src_w = p->pass_tex[src_tex].src.x1 - p->pass_tex[src_tex].src.x0;
    finish_pass_fbo(p, &scaler->sep_fbo, src_w, h, src_tex, FBOTEX_FUZZY_H);
    // Restore the sample source for the second pass
    GLSLF("#define sample_tex  texture%d\n", src_tex);
    GLSLF("#define sample_pos  texcoord%d\n", src_tex);
    GLSLF("#define sample_size texture_size%d\n", src_tex);
    GLSLF("// pass 2\n");
    p->pass_tex[src_tex].src.x0 = src_new.x0;
    p->pass_tex[src_tex].src.x1 = src_new.x1;
    pass_sample_separated_gen(p, scaler, 1, 0);
}

static void pass_sample_polar(struct gl_video *p, struct scaler *scaler)
{
    double radius = scaler->kernel->radius;
    int bound = (int)ceil(radius);
    bool use_ar = scaler->antiring > 0;
    GLSL(vec4 color = vec4(0.0);)
    GLSLF("{\n");
    GLSL(vec2 pt = vec2(1.0) / sample_size;)
    GLSL(vec2 fcoord = fract(sample_pos * sample_size - vec2(0.5));)
    GLSL(vec2 base = sample_pos - fcoord * pt;)
    GLSL(vec4 c;)
    GLSLF("float w, d, wsum = 0.0;\n");
    if (use_ar) {
        GLSL(vec4 lo = vec4(1.0);)
        GLSL(vec4 hi = vec4(0.0);)
    }
    gl_sc_uniform_sampler(p->sc, "lut", scaler->gl_target,
                          TEXUNIT_SCALERS + scaler->index);
    GLSLF("// scaler samples\n");
    for (int y = 1-bound; y <= bound; y++) {
        for (int x = 1-bound; x <= bound; x++) {
            // Since we can't know the subpixel position in advance, assume a
            // worst case scenario
            int yy = y > 0 ? y-1 : y;
            int xx = x > 0 ? x-1 : x;
            double dmax = sqrt(xx*xx + yy*yy);
            // Skip samples definitely outside the radius
            if (dmax >= radius)
                continue;
            GLSLF("d = length(vec2(%d, %d) - fcoord)/%f;\n", x, y, radius);
            // Check for samples that might be skippable
            if (dmax >= radius - 1)
                GLSLF("if (d < 1.0) {\n");
            GLSL(w = texture1D(lut, d).r;)
            GLSL(wsum += w;)
            GLSLF("c = texture(sample_tex, base + pt * vec2(%d, %d));\n", x, y);
            GLSL(color += vec4(w) * c;)
            if (use_ar && x >= 0 && y >= 0 && x <= 1 && y <= 1) {
                GLSL(lo = min(lo, c);)
                GLSL(hi = max(hi, c);)
            }
            if (dmax >= radius -1)
                GLSLF("}\n");
        }
    }
    GLSL(color = color / vec4(wsum);)
    if (use_ar)
        GLSLF("color = mix(color, clamp(color, lo, hi), %f);\n", scaler->antiring);
    GLSLF("}\n");
}

static void bicubic_calcweights(struct gl_video *p, const char *t, const char *s)
{
    // Explanation of how bicubic scaling with only 4 texel fetches is done:
    //   http://www.mate.tue.nl/mate/pdfs/10318.pdf
    //   'Efficient GPU-Based Texture Interpolation using Uniform B-Splines'
    // Explanation why this algorithm normally always blurs, even with unit
    // scaling:
    //   http://bigwww.epfl.ch/preprints/ruijters1001p.pdf
    //   'GPU Prefilter for Accurate Cubic B-spline Interpolation'
    GLSLF("vec4 %s = vec4(-0.5, 0.1666, 0.3333, -0.3333) * %s"
                " + vec4(1, 0, -0.5, 0.5);\n", t, s);
    GLSLF("%s = %s * %s + vec4(0, 0, -0.5, 0.5);\n", t, t, s);
    GLSLF("%s = %s * %s + vec4(-0.6666, 0, 0.8333, 0.1666);\n", t, t, s);
    GLSLF("%s.xy *= vec2(1, 1) / vec2(%s.z, %s.w);\n", t, t, t);
    GLSLF("%s.xy += vec2(1 + %s, 1 - %s);\n", t, s, s);
}

static void pass_sample_bicubic_fast(struct gl_video *p)
{
    GLSL(vec4 color;)
    GLSLF("{\n");
    GLSL(vec2 pt = 1.0 / sample_size;)
    GLSL(vec2 fcoord = fract(sample_tex * sample_size + vec2(0.5, 0.5));)
    bicubic_calcweights(p, "parmx", "fcoord.x");
    bicubic_calcweights(p, "parmy", "fcoord.y");
    GLSL(vec4 cdelta;)
    GLSL(cdelta.xz = parmx.RG * vec2(-pt.x, pt.x);)
    GLSL(cdelta.yw = parmy.RG * vec2(-pt.y, pt.y);)
    // first y-interpolation
    GLSL(vec4 ar = texture(sample_tex, sample_pos + cdelta.xy);)
    GLSL(vec4 ag = texture(sample_tex, sample_pos + cdelta.xw);)
    GLSL(vec4 ab = mix(ag, ar, parmy.b);)
    // second y-interpolation
    GLSL(vec4 br = texture(sample_tex, sample_pos + cdelta.zy);)
    GLSL(vec4 bg = texture(sample_tex, sample_pos + cdelta.zw);)
    GLSL(vec4 aa = mix(bg, br, parmy.b);)
    // x-interpolation
    GLSL(color = mix(aa, ab, parmx.b);)
    GLSLF("}\n");
}

static void pass_sample_sharpen3(struct gl_video *p, struct scaler *scaler)
{
    GLSL(vec4 color;)
    GLSLF("{\n");
    GLSL(vec2 pt = 1.0 / sample_size;)
    GLSL(vec2 st = pt * 0.5;)
    GLSL(vec4 p = texture(sample_tex, sample_pos);)
    GLSL(vec4 sum = texture(sample_tex, sample_pos + st * vec2(+1, +1))
                  + texture(sample_tex, sample_pos + st * vec2(+1, -1))
                  + texture(sample_tex, sample_pos + st * vec2(-1, +1))
                  + texture(sample_tex, sample_pos + st * vec2(-1, -1));)
    double param = isnan(scaler->params[0]) ? 0.5 : scaler->params[0];
    GLSLF("color = p + (p - 0.25 * sum) * %f;\n", param);
    GLSLF("}\n");
}

static void pass_sample_sharpen5(struct gl_video *p, struct scaler *scaler)
{
    GLSL(vec4 color;)
    GLSLF("{\n");
    GLSL(vec2 pt = 1.0 / sample_size;)
    GLSL(vec2 st1 = pt * 1.2;)
    GLSL(vec4 p = texture(sample_tex, sample_pos);)
    GLSL(vec4 sum1 = texture(sample_tex, sample_pos + st1 * vec2(+1, +1))
                   + texture(sample_tex, sample_pos + st1 * vec2(+1, -1))
                   + texture(sample_tex, sample_pos + st1 * vec2(-1, +1))
                   + texture(sample_tex, sample_pos + st1 * vec2(-1, -1));)
    GLSL(vec2 st2 = pt * 1.5;)
    GLSL(vec4 sum2 = texture(sample_tex, sample_pos + st2 * vec2(+1,  0))
                   + texture(sample_tex, sample_pos + st2 * vec2( 0, +1))
                   + texture(sample_tex, sample_pos + st2 * vec2(-1,  0))
                   + texture(sample_tex, sample_pos + st2 * vec2( 0, -1));)
    GLSL(vec4 t = p * 0.859375 + sum2 * -0.1171875 + sum1 * -0.09765625;)
    double param = isnan(scaler->params[0]) ? 0.5 : scaler->params[0];
    GLSLF("color = p + t * %f;\n", param);
    GLSLF("}\n");

}

// Sample. This samples from the texture ID given by src_tex. It's hardcoded to
// use all variables and values associated with it (which includes textureN,
// texcoordN and texture_sizeN).
// The src rectangle is implicit in p->pass_tex + transform.
// The dst rectangle is implicit by what the caller will do next, but w and h
// must still be what is going to be used (to dimension FBOs correctly).
// This will declare "vec4 color;", which contains the scaled contents.
// The scaler unit is initialized by this function; in order to avoid cache
// thrashing, the scaler unit should usually use the same parameters.
static void pass_sample(struct gl_video *p, int src_tex,
                        int scaler_unit, const char *name, double scale_factor,
                        int w, int h, float transform[3][2])
{
    struct scaler *scaler = &p->scalers[scaler_unit];
    reinit_scaler(p, scaler_unit, name, scale_factor);

    // Set up the sample parameters appropriately
    GLSLF("#define sample_tex  texture%d\n", src_tex);
    GLSLF("#define sample_pos  texcoord%d\n", src_tex);
    GLSLF("#define sample_size texture_size%d\n", src_tex);

    // Set up the transformation for everything other than separated scaling
    if (!scaler->kernel || scaler->kernel->polar)
        gl_matrix_mul_rect(transform, &p->pass_tex[src_tex].src);

    // Dispatch the scaler. They're all wildly different.
    if (strcmp(scaler->name, "bilinear") == 0) {
        GLSL(vec4 color = texture(sample_tex, sample_pos);)
    } else if (strcmp(scaler->name, "bicubic_fast") == 0) {
        pass_sample_bicubic_fast(p);
    } else if (strcmp(scaler->name, "sharpen3") == 0) {
        pass_sample_sharpen3(p, scaler);
    } else if (strcmp(scaler->name, "sharpen5") == 0) {
        pass_sample_sharpen5(p, scaler);
    } else if (scaler->kernel && scaler->kernel->polar) {
        pass_sample_polar(p, scaler);
    } else if (scaler->kernel) {
        pass_sample_separated(p, src_tex, scaler, w, h, transform);
    } else {
        // Should never happen
        abort();
    }

    // Micro-optimization: Avoid scaling unneeded channels
    if (!p->has_alpha || p->opts.alpha_mode != 1)
        GLSL(color.a = 1.0;)
}

// sample from video textures, set "color" variable to yuv value
static void pass_read_video(struct gl_video *p)
{
    float chromafix[3][2];
    pass_set_image_textures(p, &p->image, chromafix);

    if (p->plane_count == 1) {
        GLSL(vec4 color = texture(texture0, texcoord0);)
        goto fixalpha;
    }

    const char *cscale = p->opts.scalers[1];
    if (p->image_desc.flags & MP_IMGFLAG_SUBSAMPLED &&
            strcmp(cscale, "bilinear") != 0) {
        struct src_tex luma = p->pass_tex[0];
        if (p->plane_count > 2) {
            // For simplicity and performance, we merge the chroma planes
            // into a single texture before scaling, so the scaler doesn't
            // need to run multiple times.
            GLSLF("// chroma merging\n");
            GLSL(vec4 color = vec4(texture(texture1, texcoord1).r,
                                   texture(texture2, texcoord2).r,
                                   0.0, 1.0);)
            int c_w = p->pass_tex[1].src.x1 - p->pass_tex[1].src.x0;
            int c_h = p->pass_tex[1].src.y1 - p->pass_tex[1].src.y0;
            assert(c_w == p->pass_tex[2].src.x1 - p->pass_tex[2].src.x0);
            assert(c_h == p->pass_tex[2].src.y1 - p->pass_tex[2].src.y0);
            finish_pass_fbo(p, &p->chroma_merge_fbo, c_w, c_h, 1, 0);
        }
        GLSLF("// chroma scaling\n");
        pass_sample(p, 1, 1, cscale, 1.0, p->image_w, p->image_h, chromafix);
        GLSL(vec2 chroma = color.rg;)
        // Always force rendering to a FBO before main scaling, or we would
        // scale chroma incorrectly.
        p->use_indirect = true;
        p->pass_tex[0] = luma; // Restore luma after scaling
    } else {
        GLSL(vec4 color;)
        if (p->plane_count == 2) {
            gl_matrix_mul_rect(chromafix, &p->pass_tex[1].src);
            GLSL(vec2 chroma = texture(texture1, texcoord0).rg;) // NV formats
        } else {
            gl_matrix_mul_rect(chromafix, &p->pass_tex[1].src);
            gl_matrix_mul_rect(chromafix, &p->pass_tex[2].src);
            GLSL(vec2 chroma = vec2(texture(texture1, texcoord1).r,
                                    texture(texture2, texcoord2).r);)
        }
    }

    GLSL(color = vec4(texture(texture0, texcoord0).r, chroma, 1.0);)

fixalpha:
    if (p->has_alpha) {
        if (p->plane_count >= 4)
            GLSL(color.a = texture(texture3, texcoord3).r;)
        if (p->opts.alpha_mode == 0) // none
            GLSL(color.a = 1.0;)
        if (p->opts.alpha_mode == 2) // blend
            GLSL(color = vec4(color.rgb * color.a, 1.0);)
    }
}

// yuv conversion, and any other conversions before main up/down-scaling
static void pass_convert_yuv(struct gl_video *p)
{
    struct gl_shader_cache *sc = p->sc;

    struct mp_csp_params cparams = MP_CSP_PARAMS_DEFAULTS;
    cparams.gray = p->is_yuv && !p->is_packed_yuv && p->plane_count == 1;
    cparams.input_bits = p->image_desc.component_bits;
    cparams.texture_bits = (cparams.input_bits + 7) & ~7;
    mp_csp_set_image_params(&cparams, &p->image_params);
    mp_csp_copy_equalizer_values(&cparams, &p->video_eq);

    float user_gamma = cparams.gamma * p->opts.gamma;
    p->user_gamma_enabled |= user_gamma != 1.0;

    GLSLF("// color conversion\n");

    if (p->color_swizzle[0])
        GLSLF("color = color.%s;\n", p->color_swizzle);

    // Pre-colormatrix input gamma correction
    if (p->image_desc.flags & MP_IMGFLAG_XYZ) {
        cparams.colorspace = MP_CSP_XYZ;
        cparams.input_bits = 8;
        cparams.texture_bits = 8;

        // Pre-colormatrix input gamma correction. Note that this results in
        // linear light
        GLSL(color.rgb *= vec3(2.6);)
    }

    // Conversion from Y'CbCr or other linear spaces to RGB
    if (!p->is_rgb) {
        struct mp_cmat m = {{{0}}};
        if (p->image_desc.flags & MP_IMGFLAG_XYZ) {
            struct mp_csp_primaries csp = mp_get_csp_primaries(p->image_params.primaries);
            mp_get_xyz2rgb_coeffs(&cparams, csp, MP_INTENT_RELATIVE_COLORIMETRIC, &m);
        } else {
            mp_get_yuv2rgb_coeffs(&cparams, &m);
        }
        gl_sc_uniform_mat3(sc, "colormatrix", true, &m.m[0][0]);
        gl_sc_uniform_vec3(sc, "colormatrix_c", m.c);

        GLSL(color.rgb = mat3(colormatrix) * color.rgb + colormatrix_c;)
    }

    if (p->image_params.colorspace == MP_CSP_BT_2020_C) {
        p->use_indirect = true;
        // Conversion for C'rcY'cC'bc via the BT.2020 CL system:
        // C'bc = (B'-Y'c) / 1.9404  | C'bc <= 0
        //      = (B'-Y'c) / 1.5816  | C'bc >  0
        //
        // C'rc = (R'-Y'c) / 1.7184  | C'rc <= 0
        //      = (R'-Y'c) / 0.9936  | C'rc >  0
        //
        // as per the BT.2020 specification, table 4. This is a non-linear
        // transformation because (constant) luminance receives non-equal
        // contributions from the three different channels.
        GLSLF("// constant luminance conversion\n");
        GLSL(color.br = color.br * mix(vec2(1.5816, 0.9936),
                                       vec2(1.9404, 1.7184),
                                       lessThanEqual(color.br, vec2(0)))
                        + color.gg;)
        // Expand channels to camera-linear light. This shader currently just
        // assumes everything uses the BT.2020 12-bit gamma function, since the
        // difference between 10 and 12-bit is negligible for anything other
        // than 12-bit content.
        GLSL(color.rgb = mix(color.rgb / vec3(4.5),
                             pow((color.rgb + vec3(0.0993))/vec3(1.0993), vec3(1.0/0.45)),
                             lessThanEqual(vec3(0.08145), color.rgb));)
        // Calculate the green channel from the expanded RYcB
        // The BT.2020 specification says Yc = 0.2627*R + 0.6780*G + 0.0593*B
        GLSL(color.g = (color.g - 0.2627*color.r - 0.0593*color.b)/0.6780;)
        // Re-compand to receive the R'G'B' result, same as other systems
        GLSL(color.rgb = mix(color.rgb * vec3(4.5),
                             vec3(1.0993) * pow(color.rgb, vec3(0.45)) - vec3(0.0993),
                             lessThanEqual(vec3(0.0181), color.rgb));)
    }

    GLSL(color.rgb = clamp(color.rgb, 0.0, 1.0);)

    if (p->user_gamma_enabled) {
        p->use_indirect = true;
        gl_sc_uniform_f(sc, "user_gamma", user_gamma);
        GLSL(color.rgb = pow(color.rgb, vec3(1.0 / user_gamma));)
    }

    if (!p->has_alpha)
        GLSL(color.a = 1.0;)
}

static void get_scale_factors(struct gl_video *p, double xy[2])
{
    xy[0] = (p->dst_rect.x1 - p->dst_rect.x0) /
            (double)(p->src_rect.x1 - p->src_rect.x0);
    xy[1] = (p->dst_rect.y1 - p->dst_rect.y0) /
            (double)(p->src_rect.y1 - p->src_rect.y0);
}

// Takes care of the main scaling and post-conversions such as gamut/gamma
// mapping or color management.
static void pass_render_main(struct gl_video *p)
{
    // Figure out the main scaler.
    double xy[2];
    get_scale_factors(p, xy);
    bool downscaling = xy[0] < 1.0 || xy[1] < 1.0;
    bool upscaling = !downscaling && (xy[0] > 1.0 || xy[1] > 1.0);
    double scale_factor = 1.0;

    char *scaler = p->opts.scalers[0];
    if (p->opts.scaler_resizes_only && !downscaling && !upscaling)
        scaler = "bilinear";
    if (downscaling)
        scaler = p->opts.dscaler;

    double f = MPMIN(xy[0], xy[1]);
    if (p->opts.fancy_downscaling && f < 1.0 &&
        fabs(xy[0] - f) < 0.01 && fabs(xy[1] - f) < 0.01)
    {
        scale_factor = FFMAX(1.0, 1.0 / f);
    }

    bool use_cms = p->use_lut_3d || p->opts.target_prim != MP_CSP_PRIM_AUTO
                                 || p->opts.target_trc != MP_CSP_TRC_AUTO;

    // Pre-conversion, like linear light/sigmoidization
    GLSLF("// scaler pre-conversion\n");
    bool use_linear = p->opts.linear_scaling || p->opts.sigmoid_upscaling
                      || use_cms || p->image_params.gamma == MP_CSP_TRC_LINEAR;
    if (use_linear) {
        p->use_indirect = true;
        switch (p->image_params.gamma) {
            case MP_CSP_TRC_SRGB:
                GLSL(color.rgb = mix(color.rgb / vec3(12.92),
                                     pow((color.rgb + vec3(0.055))/vec3(1.055),
                                         vec3(2.4)),
                                     lessThanEqual(vec3(0.04045), color.rgb));)
                break;
            case MP_CSP_TRC_BT_1886:
                GLSL(color.rgb = pow(color.rgb, vec3(1.961));)
                break;
            case MP_CSP_TRC_GAMMA22:
                GLSL(color.rgb = pow(color.rgb, vec3(2.2));)
                break;
        }
    }

    bool use_sigmoid = use_linear && p->opts.sigmoid_upscaling && upscaling;
    float sig_center, sig_slope, sig_offset, sig_scale;
    if (use_sigmoid) {
        p->use_indirect = true;
        // Coefficients for the sigmoidal transform are taken from the
        // formula here: http://www.imagemagick.org/Usage/color_mods/#sigmoidal
        sig_center = p->opts.sigmoid_center;
        sig_slope  = p->opts.sigmoid_slope;
        // This function needs to go through (0,0) and (1,1) so we compute the
        // values at 1 and 0, and then scale/shift them, respectively.
        sig_offset = 1.0/(1+expf(sig_slope * sig_center));
        sig_scale  = 1.0/(1+expf(sig_slope * (sig_center-1))) - sig_offset;
        GLSLF("color.rgb = %f - log(1.0/(color.rgb * %f + %f) - 1.0)/%f;\n",
                sig_center, sig_scale, sig_offset, sig_slope);
    }

    // Compute the cropped and rotated transformation
    float sx = (p->src_rect.x1 - p->src_rect.x0) / (float)p->image_w,
          sy = (p->src_rect.y1 - p->src_rect.y0) / (float)p->image_h,
          ox = p->src_rect.x0,
          oy = p->src_rect.y0;
    float transform[3][2] = {{sx,0.0}, {0.0,sy}, {ox,oy}};

    int xc = 0, yc = 1,
        vp_w = p->dst_rect.x1 - p->dst_rect.x0,
        vp_h = p->dst_rect.y1 - p->dst_rect.y0;

    if ((p->image_params.rotate % 180) == 90) {
        for (int n = 0; n < 3; n++)
            MPSWAP(float, transform[n][xc], transform[n][yc]);
        MPSWAP(int, xc, yc);
        MPSWAP(int, vp_w, vp_h);
    }

    GLSLF("// main scaling\n");
    if (!p->use_indirect && strcmp(scaler, "bilinear") == 0) {
        // implicitly scale in pass_video_to_screen, but set up the textures
        // manually (for cropping etc.). Special care has to be taken for the
        // chroma planes (everything except luma=tex0), to make sure the offset
        // is scaled to the correct reference frame (in the case of subsampled
        // input)
        float tchroma[3][2];
        memcpy(tchroma, transform, sizeof(float[3][2]));
        tchroma[2][xc] /= 1 << p->image_desc.chroma_xs;
        tchroma[2][yc] /= 1 << p->image_desc.chroma_ys;

        for (int n = 0; n < p->plane_count; n++)
            gl_matrix_mul_rect(n>0 ? tchroma : transform, &p->pass_tex[n].src);
    } else {
        finish_pass_fbo(p, &p->indirect_fbo, p->image_w, p->image_h, 0, 0);
        pass_sample(p, 0, 0, scaler, scale_factor, vp_w, vp_h, transform);
    }

    GLSLF("// scaler post-conversion\n");
    if (use_sigmoid) {
        // Inverse of the transformation above
        GLSLF("color.rgb = (1.0/(1.0 + exp(%f * (%f - color.rgb))) - %f) / %f;\n",
                sig_slope, sig_center, sig_offset, sig_scale);
    }

    GLSLF("// color management\n");
    enum mp_csp_trc trc_dst = p->opts.target_trc;
    enum mp_csp_prim prim_src = p->image_params.primaries,
                     prim_dst = p->opts.target_prim;

    if (p->use_lut_3d) {
        // The 3DLUT is hard-coded against BT.2020's gamut during creation, and
        // we never want to adjust its output (so treat it as linear)
        prim_dst = MP_CSP_PRIM_BT_2020;
        trc_dst = MP_CSP_TRC_LINEAR;
    }

    if (prim_dst == MP_CSP_PRIM_AUTO)
        prim_dst = prim_src;
    if (trc_dst == MP_CSP_TRC_AUTO) {
        trc_dst = p->image_params.gamma;
        // Pick something more reasonable for linear light inputs
        if (p->image_params.gamma == MP_CSP_TRC_LINEAR)
            trc_dst = MP_CSP_TRC_GAMMA22;
    }

    // Adapt to the right colorspace if necessary
    if (prim_src != prim_dst) {
        struct mp_csp_primaries csp_src = mp_get_csp_primaries(prim_src),
                                csp_dst = mp_get_csp_primaries(prim_dst);
        float m[3][3] = {{0}};
        mp_get_cms_matrix(csp_src, csp_dst, MP_INTENT_RELATIVE_COLORIMETRIC, m);
        gl_sc_uniform_mat3(p->sc, "cms_matrix", true, &m[0][0]);
        GLSL(color.rgb = cms_matrix * color.rgb;)
    }

    if (p->use_lut_3d) {
        gl_sc_uniform_sampler(p->sc, "lut_3d", GL_TEXTURE_3D, TEXUNIT_3DLUT);
        // For the 3DLUT we are arbitrarily using 2.4 as input gamma to reduce
        // the severity of quantization errors.
        GLSL(color.rgb = clamp(color.rgb, 0.0, 1.0);)
        GLSL(color.rgb = pow(color.rgb, vec3(1.0/2.4));)
        GLSL(color.rgb = texture3D(lut_3d, color.rgb).rgb;)
    }

    // Don't perform any gamut mapping unless linear light input is present to
    // begin with
    if (use_linear && trc_dst != MP_CSP_TRC_LINEAR) {
        GLSL(color.rgb = clamp(color.rgb, 0.0, 1.0);)
        switch (trc_dst) {
            case MP_CSP_TRC_SRGB:
                GLSL(color.rgb = mix(color.rgb * vec3(12.92),
                                     vec3(1.055) * pow(color.rgb,
                                                       vec3(1.0/2.4))
                                         - vec3(0.055),
                                     lessThanEqual(vec3(0.0031308), color.rgb));)
                break;
            case MP_CSP_TRC_BT_1886:
                GLSL(color.rgb = pow(color.rgb, vec3(1.0/1.961));)
                break;
            case MP_CSP_TRC_GAMMA22:
                GLSL(color.rgb = pow(color.rgb, vec3(1.0/2.2));)
                break;
        }
    }
}

static void pass_dither(struct gl_video *p)
{
    GL *gl = p->gl;

    // Assume 8 bits per component if unknown.
    int dst_depth = p->depth_g ? p->depth_g : 8;
    if (p->opts.dither_depth > 0)
        dst_depth = p->opts.dither_depth;

    if (p->opts.dither_depth < 0 || p->opts.dither_algo < 0)
        return;

    if (!p->dither_texture) {
        MP_VERBOSE(p, "Dither to %d.\n", dst_depth);

        int tex_size;
        void *tex_data;
        GLint tex_iformat;
        GLint tex_format;
        GLenum tex_type;
        unsigned char temp[256];

        if (p->opts.dither_algo == 0) {
            int sizeb = p->opts.dither_size;
            int size = 1 << sizeb;

            if (p->last_dither_matrix_size != size) {
                p->last_dither_matrix = talloc_realloc(p, p->last_dither_matrix,
                                                       float, size * size);
                mp_make_fruit_dither_matrix(p->last_dither_matrix, sizeb);
                p->last_dither_matrix_size = size;
            }

            tex_size = size;
            tex_iformat = gl_float16_formats[0].internal_format;
            tex_format = gl_float16_formats[0].format;
            tex_type = GL_FLOAT;
            tex_data = p->last_dither_matrix;
        } else {
            assert(sizeof(temp) >= 8 * 8);
            mp_make_ordered_dither_matrix(temp, 8);

            const struct fmt_entry *fmt = find_tex_format(gl, 1, 1);
            tex_size = 8;
            tex_iformat = fmt->internal_format;
            tex_format = fmt->format;
            tex_type = fmt->type;
            tex_data = temp;
        }

        p->dither_size = tex_size;

        gl->ActiveTexture(GL_TEXTURE0 + TEXUNIT_DITHER);
        gl->GenTextures(1, &p->dither_texture);
        gl->BindTexture(GL_TEXTURE_2D, p->dither_texture);
        gl->PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->TexImage2D(GL_TEXTURE_2D, 0, tex_iformat, tex_size, tex_size, 0,
                    tex_format, tex_type, tex_data);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->ActiveTexture(GL_TEXTURE0);

        debug_check_gl(p, "dither setup");
    }

    GLSLF("// dithering\n");

    // This defines how many bits are considered significant for output on
    // screen. The superfluous bits will be used for rounding according to the
    // dither matrix. The precision of the source implicitly decides how many
    // dither patterns can be visible.
    float dither_quantization = (1 << dst_depth) - 1;
    float dither_center = 0.5 / (p->dither_size * p->dither_size);

    gl_sc_uniform_f(p->sc, "dither_size", p->dither_size);
    gl_sc_uniform_f(p->sc, "dither_quantization", dither_quantization);
    gl_sc_uniform_f(p->sc, "dither_center", dither_center);
    gl_sc_uniform_sampler(p->sc, "dither", GL_TEXTURE_2D, TEXUNIT_DITHER);

    GLSL(vec2 dither_pos = gl_FragCoord.xy / dither_size;)

    if (p->opts.temporal_dither) {
        int phase = p->frames_rendered % 8u;
        float r = phase * (M_PI / 2); // rotate
        float m = phase < 4 ? 1 : -1; // mirror

        float matrix[2][2] = {{cos(r),     -sin(r)    },
                              {sin(r) * m,  cos(r) * m}};
        gl_sc_uniform_mat2(p->sc, "dither_trafo", true, &matrix[0][0]);

        GLSL(dither_pos = dither_trafo * dither_pos;)
    }

    GLSL(float dither_value = texture(dither, dither_pos).r;)
    GLSL(color = floor(color * dither_quantization + dither_value + dither_center) /
                       dither_quantization;)
}

// The main rendering function, takes care of everything up to and including
// color management
static void pass_draw_frame(struct gl_video *p)
{
    p->use_indirect = false; // set to true as needed by pass_*
    pass_read_video(p);
    pass_convert_yuv(p);
    pass_render_main(p);
}

static void pass_draw_to_screen(struct gl_video *p, int fbo)
{
    pass_dither(p);
    int flags = (p->image_params.rotate % 90 ? 0 : p->image_params.rotate / 90)
              | (p->image.image_flipped ? 4 : 0);
    finish_pass_direct(p, fbo, p->vp_w, p->vp_h, &p->dst_rect, flags);
}

// Draws an interpolate frame to fbo, based on the frame timing in t
static void gl_video_interpolate_frame(struct gl_video *p, int fbo,
                                       struct frame_timing *t)
{
    int vp_w = p->dst_rect.x1 - p->dst_rect.x0,
        vp_h = p->dst_rect.y1 - p->dst_rect.y0,
        fuzz = FBOTEX_FUZZY_W | FBOTEX_FUZZY_H;
    size_t surface_nxt = fbosurface_next(p->surface_now);

    // First of all, figure out if we have a frame availble at all, and draw
    // it manually + reset the queue if not
    if (!p->surfaces[p->surface_now].pts) {
        pass_draw_frame(p);
        finish_pass_fbo(p, &p->surfaces[p->surface_now].fbotex,
                        vp_w, vp_h, 0, fuzz);
        p->surfaces[p->surface_now].pts = t ? t->pts : 0;
        p->surface_idx = p->surface_now;
    }

    // Render a new frame if it came in and there's room in the queue
    size_t surface_dst = fbosurface_next(p->surface_idx);
    if (t && surface_dst != p->surface_now &&
             p->surfaces[p->surface_idx].pts < t->pts) {
        MP_STATS(p, "new-pts");
        pass_draw_frame(p);
        finish_pass_fbo(p, &p->surfaces[surface_dst].fbotex,
                        vp_w, vp_h, 0, fuzz);
        p->surfaces[surface_dst].pts = t->pts;
        p->surface_idx = surface_dst;
    }

    // Finally, draw the right mix of frames to the screen.
    pass_load_fbotex(p, &p->surfaces[p->surface_now].fbotex, 0, vp_w, vp_h);
    if (!t || p->surfaces[surface_nxt].pts < p->surfaces[p->surface_now].pts) {
        // No next frame available (eg. start of playback, after reconfigure
        // or end of file, so just draw the current frame instead of blending.
        // Also occurs when no timing information is available (eg. paused)
        GLSL(vec4 color = texture(texture0, texcoord0);)
        p->is_interpolated = false;
    } else {
        int64_t next_pts = p->surfaces[surface_nxt].pts,
                vsync_interval = t->next_vsync - t->prev_vsync;
        double inter_coeff = (double)(next_pts - t->next_vsync) / vsync_interval,
               threshold = p->opts.smoothmotion_threshold;
        inter_coeff = inter_coeff <= 0.0 + threshold ? 0.0 : inter_coeff;
        inter_coeff = inter_coeff >= 1.0 - threshold ? 1.0 : inter_coeff;
        inter_coeff = 1.0 - inter_coeff;
        gl_sc_uniform_f(p->sc, "inter_coeff", inter_coeff);
        p->is_interpolated = inter_coeff > 0;

        MP_STATS(p, "frame-mix");
        MP_DBG(p, "inter frame ppts: %lld, pts: %lld, vsync: %lld, mix: %f\n",
               (long long)p->surfaces[p->surface_now].pts,
               (long long)p->surfaces[surface_nxt].pts,
               (long long)t->next_vsync, inter_coeff);

        pass_load_fbotex(p, &p->surfaces[surface_nxt].fbotex, 1, vp_w, vp_h);
        GLSL(vec4 color = mix(texture(texture0, texcoord0),
                              texture(texture1, texcoord1),
                              inter_coeff);)
        // Dequeue the current frame if it's no longer needed
        if (t->next_vsync + vsync_interval > p->surfaces[surface_nxt].pts)
            p->surface_now = surface_nxt;
    }
    pass_draw_to_screen(p, fbo);
}

// (fbo==0 makes BindFramebuffer select the screen backbuffer)
void gl_video_render_frame(struct gl_video *p, int fbo, struct frame_timing *t)
{
    GL *gl = p->gl;
    struct video_image *vimg = &p->image;

    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo);

    if (p->dst_rect.x0 > 0 || p->dst_rect.y0 > 0
        || p->dst_rect.x1 < p->vp_w || p->dst_rect.y1 < abs(p->vp_h))
    {
        gl->Clear(GL_COLOR_BUFFER_BIT);
    }

    if (!vimg->mpi) {
        gl->Clear(GL_COLOR_BUFFER_BIT);
        goto draw_osd;
    }

    gl_sc_set_vao(p->sc, &p->vao);

    if (p->opts.smoothmotion) {
        gl_video_interpolate_frame(p, fbo, t);
    } else {
        pass_draw_frame(p);
        pass_draw_to_screen(p, fbo);
    }

    debug_check_gl(p, "after video rendering");

    if (p->hwdec_active)
        p->hwdec->driver->unmap_image(p->hwdec);

draw_osd:

    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->Viewport(0, 0, p->vp_w, abs(p->vp_h));

    mpgl_osd_generate(p->osd, p->osd_rect, p->osd_pts, p->image_params.stereo_out);

    for (int n = 0; n < MAX_OSD_PARTS; n++) {
        enum sub_bitmap_format fmt = mpgl_osd_get_part_format(p->osd, n);
        if (!fmt)
            continue;
        gl_sc_uniform_sampler(p->sc, "osdtex", GL_TEXTURE_2D, 0);
        switch (fmt) {
        case SUBBITMAP_RGBA: {
            GLSLF("// OSD (RGBA)\n");
            GLSL(vec4 color = texture(osdtex, texcoord).bgra;)
            break;
        }
        case SUBBITMAP_LIBASS: {
            GLSLF("// OSD (libass)\n");
            GLSL(vec4 color =
                vec4(ass_color.rgb, ass_color.a * texture(osdtex, texcoord).r);)
            break;
        }
        default:
            abort();
        }
        gl_sc_set_vao(p->sc, mpgl_osd_get_vao(p->osd));
        gl_sc_gen_shader_and_reset(p->sc);
        mpgl_osd_draw_part(p->osd, p->vp_w, p->vp_h, n);
    }

    debug_check_gl(p, "after OSD rendering");

    gl->UseProgram(0);
    gl->BindFramebuffer(GL_FRAMEBUFFER, 0);

    p->frames_rendered++;
}

// vp_w/vp_h is the implicit size of the target framebuffer.
// vp_h can be negative to flip the screen.
void gl_video_resize(struct gl_video *p, int vp_w, int vp_h,
                     struct mp_rect *src, struct mp_rect *dst,
                     struct mp_osd_res *osd)
{
    p->src_rect = *src;
    p->dst_rect = *dst;
    p->osd_rect = *osd;
    p->vp_w = vp_w;
    p->vp_h = vp_h;

    gl_video_reset_surfaces(p);
}

static bool get_image(struct gl_video *p, struct mp_image *mpi)
{
    GL *gl = p->gl;

    if (!p->opts.pbo)
        return false;

    struct video_image *vimg = &p->image;

    // See comments in init_video() about odd video sizes.
    // The normal upload path does this too, but less explicit.
    mp_image_set_size(mpi, vimg->planes[0].w, vimg->planes[0].h);

    for (int n = 0; n < p->plane_count; n++) {
        struct texplane *plane = &vimg->planes[n];
        mpi->stride[n] = mpi->plane_w[n] * p->image_desc.bytes[n];
        int needed_size = mpi->plane_h[n] * mpi->stride[n];
        if (!plane->gl_buffer)
            gl->GenBuffers(1, &plane->gl_buffer);
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, plane->gl_buffer);
        if (needed_size > plane->buffer_size) {
            plane->buffer_size = needed_size;
            gl->BufferData(GL_PIXEL_UNPACK_BUFFER, plane->buffer_size,
                           NULL, GL_DYNAMIC_DRAW);
        }
        if (!plane->buffer_ptr)
            plane->buffer_ptr = gl->MapBuffer(GL_PIXEL_UNPACK_BUFFER,
                                              GL_WRITE_ONLY);
        mpi->planes[n] = plane->buffer_ptr;
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    return true;
}

void gl_video_upload_image(struct gl_video *p, struct mp_image *mpi)
{
    GL *gl = p->gl;

    struct video_image *vimg = &p->image;

    p->osd_pts = mpi->pts;

    talloc_free(vimg->mpi);
    vimg->mpi = mpi;

    if (p->hwdec_active)
        return;

    assert(mpi->num_planes == p->plane_count);

    mp_image_t mpi2 = *mpi;
    bool pbo = false;
    if (!vimg->planes[0].buffer_ptr && get_image(p, &mpi2)) {
        for (int n = 0; n < p->plane_count; n++) {
            int line_bytes = mpi->plane_w[n] * p->image_desc.bytes[n];
            memcpy_pic(mpi2.planes[n], mpi->planes[n], line_bytes, mpi->plane_h[n],
                       mpi2.stride[n], mpi->stride[n]);
        }
        pbo = true;
    }
    vimg->image_flipped = mpi2.stride[0] < 0;
    for (int n = 0; n < p->plane_count; n++) {
        struct texplane *plane = &vimg->planes[n];
        void *plane_ptr = mpi2.planes[n];
        if (pbo) {
            gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, plane->gl_buffer);
            if (!gl->UnmapBuffer(GL_PIXEL_UNPACK_BUFFER))
                MP_FATAL(p, "Video PBO upload failed. "
                         "Remove the 'pbo' suboption.\n");
            plane->buffer_ptr = NULL;
            plane_ptr = NULL; // PBO offset 0
        }
        gl->ActiveTexture(GL_TEXTURE0 + n);
        gl->BindTexture(p->gl_target, plane->gl_texture);
        glUploadTex(gl, p->gl_target, plane->gl_format, plane->gl_type,
                    plane_ptr, mpi2.stride[n], 0, 0, plane->w, plane->h, 0);
    }
    gl->ActiveTexture(GL_TEXTURE0);
    if (pbo)
        gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static bool test_fbo(struct gl_video *p, bool *success)
{
    if (!*success)
        return false;

    GL *gl = p->gl;
    *success = false;
    MP_VERBOSE(p, "Testing user-set FBO format (0x%x)\n",
                   (unsigned)p->opts.fbo_format);
    struct fbotex fbo = {0};
    if (fbotex_init(&fbo, p->gl, p->log, 16, 16, p->opts.fbo_format)) {
        gl->BindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
        gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
        *success = true;
    }
    fbotex_uninit(&fbo);
    glCheckError(gl, p->log, "FBO test");
    return *success;
}

// Disable features that are not supported with the current OpenGL version.
static void check_gl_features(struct gl_video *p)
{
    GL *gl = p->gl;
    bool have_float_tex = gl->mpgl_caps & MPGL_CAP_FLOAT_TEX;
    bool have_fbo = gl->mpgl_caps & MPGL_CAP_FB;
    bool have_arrays = gl->mpgl_caps & MPGL_CAP_1ST_CLASS_ARRAYS;
    bool have_1d_tex = gl->mpgl_caps & MPGL_CAP_1D_TEX;
    bool have_3d_tex = gl->mpgl_caps & MPGL_CAP_3D_TEX;
    bool have_mix = gl->glsl_version >= 130;

    char *disabled[10];
    int n_disabled = 0;

    // Normally, we want to disable them by default if FBOs are unavailable,
    // because they will be slow (not critically slow, but still slower).
    // Without FP textures, we must always disable them.
    // I don't know if luminance alpha float textures exist, so disregard them.
    for (int n = 0; n < 2; n++) {
        const struct filter_kernel *kernel = mp_find_filter_kernel(p->opts.scalers[n]);
        if (kernel) {
            char *reason = NULL;
            if (!test_fbo(p, &have_fbo))
                reason = "scaler (FBO)";
            if (!have_float_tex)
                reason = "scaler (float tex.)";
            if (!have_arrays)
                reason = "scaler (no GLSL support)";
            if (!have_1d_tex && kernel->polar)
                reason = "scaler (1D tex.)";
            if (reason) {
                p->opts.scalers[n] = "bilinear";
                disabled[n_disabled++] = reason;
            }
        }
    }

    // GLES3 doesn't provide filtered 16 bit integer textures
    // GLES2 doesn't even provide 3D textures
    if (p->use_lut_3d && !(have_3d_tex && have_float_tex)) {
        p->use_lut_3d = false;
        disabled[n_disabled++] = "color management (GLES unsupported)";
    }

    // Missing float textures etc. (maybe ordered would actually work)
    if (p->opts.dither_algo >= 0 && gl->es) {
        p->opts.dither_algo = -1;
        disabled[n_disabled++] = "dithering (GLES unsupported)";
    }

    int use_cms = p->opts.target_prim != MP_CSP_PRIM_AUTO ||
                  p->opts.target_trc != MP_CSP_TRC_AUTO || p->use_lut_3d;

    // mix() is needed for some gamma functions
    if (!have_mix && (p->opts.linear_scaling || p->opts.sigmoid_upscaling)) {
        p->opts.linear_scaling = false;
        p->opts.sigmoid_upscaling = false;
        disabled[n_disabled++] = "linear/sigmoid scaling (GLSL version)";
    }
    if (!have_mix && use_cms) {
        p->opts.target_prim = MP_CSP_PRIM_AUTO;
        p->opts.target_trc = MP_CSP_TRC_AUTO;
        p->use_lut_3d = false;
        disabled[n_disabled++] = "color management (GLSL version)";
    }
    if (use_cms && !test_fbo(p, &have_fbo)) {
        p->opts.target_prim = MP_CSP_PRIM_AUTO;
        p->opts.target_trc = MP_CSP_TRC_AUTO;
        p->use_lut_3d = false;
        disabled[n_disabled++] = "color management (FBO)";
    }
    if (p->opts.smoothmotion && !test_fbo(p, &have_fbo)) {
        p->opts.smoothmotion = false;
        disabled[n_disabled++] = "smoothmotion (FBO)";
    }
    if (gl->es && p->opts.pbo) {
        p->opts.pbo = 0;
        disabled[n_disabled++] = "PBOs (GLES unsupported)";
    }

    if (n_disabled) {
        MP_ERR(p, "Some OpenGL extensions not detected, disabling: ");
        for (int n = 0; n < n_disabled; n++) {
            if (n)
                MP_ERR(p, ", ");
            MP_ERR(p, "%s", disabled[n]);
        }
        MP_ERR(p, ".\n");
    }
}

static int init_gl(struct gl_video *p)
{
    GL *gl = p->gl;

    debug_check_gl(p, "before init_gl");

    check_gl_features(p);

    gl->Disable(GL_DITHER);

    gl_vao_init(&p->vao, gl, sizeof(struct vertex), vertex_vao);

    gl_video_set_gl_state(p);

    // Test whether we can use 10 bit. Hope that testing a single format/channel
    // is good enough (instead of testing all 1-4 channels variants etc.).
    const struct fmt_entry *fmt = find_tex_format(gl, 2, 1);
    if (gl->GetTexLevelParameteriv && fmt->format) {
        GLuint tex;
        gl->GenTextures(1, &tex);
        gl->BindTexture(GL_TEXTURE_2D, tex);
        gl->TexImage2D(GL_TEXTURE_2D, 0, fmt->internal_format, 64, 64, 0,
                       fmt->format, fmt->type, NULL);
        GLenum pname = 0;
        switch (fmt->format) {
        case GL_RED:        pname = GL_TEXTURE_RED_SIZE; break;
        case GL_LUMINANCE:  pname = GL_TEXTURE_LUMINANCE_SIZE; break;
        }
        GLint param = 0;
        if (pname)
            gl->GetTexLevelParameteriv(GL_TEXTURE_2D, 0, pname, &param);
        if (param) {
            MP_VERBOSE(p, "16 bit texture depth: %d.\n", (int)param);
            p->texture_16bit_depth = param;
        }
        gl->DeleteTextures(1, &tex);
    }

    debug_check_gl(p, "after init_gl");

    return 1;
}

void gl_video_uninit(struct gl_video *p)
{
    if (!p)
        return;

    GL *gl = p->gl;

    uninit_video(p);

    gl_sc_destroy(p->sc);

    gl_vao_uninit(&p->vao);

    gl->DeleteTextures(1, &p->lut_3d_texture);

    mpgl_osd_destroy(p->osd);

    gl_set_debug_logger(gl, NULL);

    talloc_free(p);
}

void gl_video_set_gl_state(struct gl_video *p)
{
    GL *gl = p->gl;

    struct m_color c = p->opts.background;
    gl->ClearColor(c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
    gl->ActiveTexture(GL_TEXTURE0);
    if (gl->mpgl_caps & MPGL_CAP_ROW_LENGTH)
        gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void gl_video_unset_gl_state(struct gl_video *p)
{
    /* nop */
}

void gl_video_reset(struct gl_video *p)
{
    gl_video_reset_surfaces(p);
}

bool gl_video_showing_interpolated_frame(struct gl_video *p)
{
    return p->is_interpolated;
}

// dest = src.<w> (always using 4 components)
static void packed_fmt_swizzle(char w[5], const struct fmt_entry *texfmt,
                               const struct packed_fmt_entry *fmt)
{
    const char *comp = "rgba";

    // Normally, we work with GL_RG
    if (texfmt && texfmt->internal_format == GL_LUMINANCE_ALPHA)
        comp = "ragb";

    for (int c = 0; c < 4; c++)
        w[c] = comp[MPMAX(fmt->components[c] - 1, 0)];
    w[4] = '\0';
}

static bool init_format(int fmt, struct gl_video *init)
{
    struct GL *gl = init->gl;

    init->hwdec_active = false;
    if (init->hwdec && init->hwdec->driver->imgfmt == fmt) {
        fmt = init->hwdec->converted_imgfmt;
        init->hwdec_active = true;
    }

    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(fmt);
    if (!desc.id)
        return false;

    if (desc.num_planes > 4)
        return false;

    const struct fmt_entry *plane_format[4] = {0};

    init->color_swizzle[0] = '\0';
    init->has_alpha = false;

    // YUV/planar formats
    if (desc.flags & MP_IMGFLAG_YUV_P) {
        int bits = desc.component_bits;
        if ((desc.flags & MP_IMGFLAG_NE) && bits >= 8 && bits <= 16) {
            init->has_alpha = desc.num_planes > 3;
            plane_format[0] = find_tex_format(gl, (bits + 7) / 8, 1);
            for (int p = 1; p < desc.num_planes; p++)
                plane_format[p] = plane_format[0];
            goto supported;
        }
    }

    // YUV/half-packed
    if (fmt == IMGFMT_NV12 || fmt == IMGFMT_NV21) {
        if (!(init->gl->mpgl_caps & MPGL_CAP_TEX_RG))
            return false;
        plane_format[0] = find_tex_format(gl, 1, 1);
        plane_format[1] = find_tex_format(gl, 1, 2);
        if (fmt == IMGFMT_NV21)
            snprintf(init->color_swizzle, sizeof(init->color_swizzle), "rbga");
        goto supported;
    }

    // RGB/planar
    if (fmt == IMGFMT_GBRP) {
        snprintf(init->color_swizzle, sizeof(init->color_swizzle), "brga");
        plane_format[0] = find_tex_format(gl, 1, 1);
        for (int p = 1; p < desc.num_planes; p++)
            plane_format[p] = plane_format[0];
        goto supported;
    }

    // XYZ (same organization as RGB packed, but requires conversion matrix)
    if (fmt == IMGFMT_XYZ12) {
        plane_format[0] = find_tex_format(gl, 2, 3);
        goto supported;
    }

    // Packed RGB special formats
    for (const struct fmt_entry *e = mp_to_gl_formats; e->mp_format; e++) {
        if (!gl->es && e->mp_format == fmt) {
            plane_format[0] = e;
            goto supported;
        }
    }

    // Packed RGB(A) formats
    for (const struct packed_fmt_entry *e = mp_packed_formats; e->fmt; e++) {
        if (e->fmt == fmt) {
            int n_comp = desc.bytes[0] / e->component_size;
            plane_format[0] = find_tex_format(gl, e->component_size, n_comp);
            packed_fmt_swizzle(init->color_swizzle, plane_format[0], e);
            init->has_alpha = e->components[3] != 0;
            goto supported;
        }
    }

    // Packed YUV Apple formats
    if (init->gl->mpgl_caps & MPGL_CAP_APPLE_RGB_422) {
        for (const struct fmt_entry *e = gl_apple_formats; e->mp_format; e++) {
            if (e->mp_format == fmt) {
                init->is_packed_yuv = true;
                snprintf(init->color_swizzle, sizeof(init->color_swizzle),
                         "gbra");
                plane_format[0] = e;
                goto supported;
            }
        }
    }

    // Unsupported format
    return false;

supported:

    // Stuff like IMGFMT_420AP10. Untested, most likely insane.
    if (desc.num_planes == 4 && (desc.component_bits % 8) != 0)
        return false;

    if (desc.component_bits > 8 && desc.component_bits < 16) {
        if (init->texture_16bit_depth < 16)
            return false;
    }

    for (int p = 0; p < desc.num_planes; p++) {
        if (!plane_format[p]->format)
            return false;
    }

    for (int p = 0; p < desc.num_planes; p++) {
        struct texplane *plane = &init->image.planes[p];
        const struct fmt_entry *format = plane_format[p];
        assert(format);
        plane->gl_format = format->format;
        plane->gl_internal_format = format->internal_format;
        plane->gl_type = format->type;
    }

    init->is_yuv = desc.flags & MP_IMGFLAG_YUV;
    init->is_rgb = desc.flags & MP_IMGFLAG_RGB;
    init->plane_count = desc.num_planes;
    init->image_desc = desc;

    return true;
}

bool gl_video_check_format(struct gl_video *p, int mp_format)
{
    struct gl_video tmp = *p;
    return init_format(mp_format, &tmp);
}

void gl_video_config(struct gl_video *p, struct mp_image_params *params)
{
    mp_image_unrefp(&p->image.mpi);

    if (!mp_image_params_equal(&p->real_image_params, params)) {
        uninit_video(p);
        p->real_image_params = *params;
        p->image_params = *params;
        if (params->imgfmt)
            init_video(p);
    }

    gl_video_reset_surfaces(p);
}

void gl_video_set_output_depth(struct gl_video *p, int r, int g, int b)
{
    MP_VERBOSE(p, "Display depth: R=%d, G=%d, B=%d\n", r, g, b);
    p->depth_g = g;
}

struct gl_video *gl_video_init(GL *gl, struct mp_log *log, struct osd_state *osd)
{
    if (gl->version < 210 && gl->es < 200) {
        mp_err(log, "At least OpenGL 2.1 or OpenGL ES 2.0 required.\n");
        return NULL;
    }

    struct gl_video *p = talloc_ptrtype(NULL, p);
    *p = (struct gl_video) {
        .gl = gl,
        .log = log,
        .osd_state = osd,
        .opts = gl_video_opts_def,
        .gl_target = GL_TEXTURE_2D,
        .texture_16bit_depth = 16,
        .scalers = {
            { .index = 0, .name = "bilinear" },
            { .index = 1, .name = "bilinear" },
        },
        .sc = gl_sc_create(gl, log),
    };
    gl_video_set_debug(p, true);
    init_gl(p);
    recreate_osd(p);
    return p;
}

// Get static string for scaler shader.
static const char *handle_scaler_opt(const char *name)
{
    if (name && name[0]) {
        const struct filter_kernel *kernel = mp_find_filter_kernel(name);
        if (kernel)
            return kernel->name;

        for (const char *const *filter = fixed_scale_filters; *filter; filter++) {
            if (strcmp(*filter, name) == 0)
                return *filter;
        }
    }
    return NULL;
}

// Set the options, and possibly update the filter chain too.
// Note: assumes all options are valid and verified by the option parser.
void gl_video_set_options(struct gl_video *p, struct gl_video_opts *opts)
{
    p->opts = *opts;
    for (int n = 0; n < 2; n++)
        p->opts.scalers[n] = (char *)handle_scaler_opt(p->opts.scalers[n]);
    p->opts.dscaler = (char *)handle_scaler_opt(p->opts.dscaler);

    check_gl_features(p);
    uninit_rendering(p);
}

void gl_video_get_colorspace(struct gl_video *p, struct mp_image_params *params)
{
    *params = p->image_params; // supports everything
}

struct mp_csp_equalizer *gl_video_eq_ptr(struct gl_video *p)
{
    return &p->video_eq;
}

// Call when the mp_csp_equalizer returned by gl_video_eq_ptr() was changed.
void gl_video_eq_update(struct gl_video *p)
{
}

static int validate_scaler_opt(struct mp_log *log, const m_option_t *opt,
                               struct bstr name, struct bstr param)
{
    char s[20] = {0};
    int r = 1;
    if (bstr_equals0(param, "help")) {
        r = M_OPT_EXIT - 1;
    } else {
        snprintf(s, sizeof(s), "%.*s", BSTR_P(param));
        if (!handle_scaler_opt(s))
            r = M_OPT_INVALID;
    }
    if (r < 1) {
        mp_info(log, "Available scalers:\n");
        for (const char *const *filter = fixed_scale_filters; *filter; filter++)
            mp_info(log, "    %s\n", *filter);
        for (int n = 0; mp_filter_kernels[n].name; n++)
            mp_info(log, "    %s\n", mp_filter_kernels[n].name);
        if (s[0])
            mp_fatal(log, "No scaler named '%s' found!\n", s);
    }
    return r;
}

// Resize and redraw the contents of the window without further configuration.
// Intended to be used in situations where the frontend can't really be
// involved with reconfiguring the VO properly.
// gl_video_resize() should be called when user interaction is done.
void gl_video_resize_redraw(struct gl_video *p, int w, int h)
{
    p->vp_w = w;
    p->vp_h = h;
    gl_video_render_frame(p, 0, NULL);
}

float gl_video_scale_ambient_lux(float lmin, float lmax,
                                 float rmin, float rmax, float lux)
{
    assert(lmax > lmin);

    float num = (rmax - rmin) * (log10(lux) - log10(lmin));
    float den = log10(lmax) - log10(lmin);
    float result = num / den + rmin;

    // clamp the result
    float max = MPMAX(rmax, rmin);
    float min = MPMIN(rmax, rmin);
    return MPMAX(MPMIN(result, max), min);
}

void gl_video_set_ambient_lux(struct gl_video *p, int lux)
{
    if (p->opts.gamma_auto) {
        float gamma = gl_video_scale_ambient_lux(16.0, 64.0, 2.40, 1.961, lux);
        MP_VERBOSE(p, "ambient light changed: %dlux (gamma: %f)\n", lux, gamma);
        p->opts.gamma = MPMIN(1.0, 1.961 / gamma);
        gl_video_eq_update(p);
    }
}

void gl_video_set_hwdec(struct gl_video *p, struct gl_hwdec *hwdec)
{
    p->hwdec = hwdec;
    mp_image_unrefp(&p->image.mpi);
}
