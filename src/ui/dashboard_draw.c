#include "dashboard_draw.h"
#include <stddef.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "text_shaders.h"

typedef struct {
    uint16_t x, y;
    uint8_t width, height;
    int8_t left, top;
    uint8_t advance;
} Glyph;

#include "dashboard_atlas.h"

typedef struct {
    float x, y, u, v;
} Vertex;

enum { MAX_VERTICES = 12288, FENCE_TIMEOUT_MS = 1000 };

static struct {
    XsdkTexture atlas;
    XsdkSamplerState sampler;
    XsdkShader vertex, pixel;
    XsdkBlendState blend;
    XsdkDepthStencilState depth;
    XsdkRasterState raster;
    volatile uint32_t *vertices;
    uint32_t count, first, color, fence;
} canvas;

static void flush(void) {
    uint32_t count = canvas.count - canvas.first;
    if (!count) return;
    const uint32_t bytes = count * sizeof(Vertex);
    const uint32_t address = xsdk_gpu_address(canvas.vertices) + canvas.first * sizeof(Vertex);
    const float color[] = {
        (float)((canvas.color >> 16) & 255) / 255.0f,
        (float)((canvas.color >> 8) & 255) / 255.0f,
        (float)(canvas.color & 255) / 255.0f, 1.0f
    };
    xsdk_gpu_set_pixel_constants(TEXT_FRAG_PUSH, color, TEXT_FRAG_PUSH_REGISTERS);
    xsdk_gpu_invalidate(address, bytes);
    xsdk_gpu_set_vertices(0, address, bytes);
    xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, count);
    canvas.first = canvas.count;
}

bool dash_open(void) {
    static const XsdkVertexElement layout[] = {
        { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
        { .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 8 }
    };
    if (!NT_SUCCESS(xsdk_texture_create(&canvas.atlas, XSDK_FORMAT_R8,
                                        ATLAS_WIDTH, ATLAS_HEIGHT, 1, true))) return false;
    xsdk_texture_upload(&canvas.atlas, 0, atlas_pixels, ATLAS_WIDTH);
    xsdk_gpu_invalidate(canvas.atlas.address, canvas.atlas.size_bytes);
    xsdk_texture_sampler_state(&canvas.sampler, &(const XsdkSampler){
        .mag_filter = XSDK_FILTER_LINEAR, .min_filter = XSDK_FILTER_LINEAR,
        .address_u = XSDK_ADDRESS_CLAMP, .address_v = XSDK_ADDRESS_CLAMP
    });
    canvas.vertices = xsdk_gpu_alloc(MAX_VERTICES * sizeof(Vertex));
    if (!canvas.vertices ||
        !NT_SUCCESS(xsdk_shader_load(&canvas.vertex, text_vert.container, text_vert.container_bytes)) ||
        !NT_SUCCESS(xsdk_shader_load(&canvas.pixel, text_frag.container, text_frag.container_bytes)) ||
        !NT_SUCCESS(xsdk_shader_set_vertex_layout(&canvas.vertex, layout, 2,
                                                 (const uint32_t[]){sizeof(Vertex)}))) {
        dash_close();
        return false;
    }
    xsdk_gpu_blend_state(&canvas.blend, &(const XsdkBlend){
        .color_source = XSDK_BLEND_ONE, .color_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
        .color_op = XSDK_BLEND_OP_ADD, .alpha_source = XSDK_BLEND_ONE,
        .alpha_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA, .alpha_op = XSDK_BLEND_OP_ADD,
        .write_mask = XSDK_WRITE_ALL
    });
    xsdk_gpu_depth_stencil_state(&canvas.depth, &(const XsdkDepthStencil){ .depth_test = false });
    xsdk_gpu_raster_state(&canvas.raster, &(const XsdkRaster){ .cull = XSDK_CULL_NONE });
    return true;
}

void dash_close(void) {
    if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS))) return;
    xsdk_gpu_free(canvas.vertices);
    xsdk_shader_free(&canvas.pixel);
    xsdk_shader_free(&canvas.vertex);
    xsdk_texture_free(&canvas.atlas);
}

bool dash_begin(uint32_t width, uint32_t height) {
    if (!NT_SUCCESS(xsdk_gpu_wait(canvas.fence, FENCE_TIMEOUT_MS))) return false;
    canvas.count = 0;
    canvas.first = 0;
    xsdk_gpu_set_target(width, height);
    /* Keep the 720p composition even if the console falls back to its current video mode. */
    const float to_clip[] = {2.0f / 1280.0f, -2.0f / 720.0f, -1.0f, 1.0f};
    xsdk_gpu_set_shaders(&canvas.vertex, &canvas.pixel);
    xsdk_built_shader_set_literals(&text_vert);
    xsdk_built_shader_set_literals(&text_frag);
    xsdk_gpu_set_vertex_constants(TEXT_VERT_PUSH, to_clip, TEXT_VERT_PUSH_REGISTERS);
    xsdk_gpu_set_texture(TEXT_FRAG_SAMPLER_0_0, &canvas.atlas, &canvas.sampler);
    xsdk_gpu_set_blend(&canvas.blend);
    xsdk_gpu_set_depth_stencil(&canvas.depth);
    xsdk_gpu_set_raster(&canvas.raster);
    return true;
}

void dash_end(void) {
    flush();
    canvas.fence = xsdk_gpu_fence();
}

static void quad(float x, float y, float w, float h, float u, float v, float uw, float vh, uint32_t rgb) {
    if (canvas.count + 6 > MAX_VERTICES) return;
    if (rgb != canvas.color) {
        flush();
        canvas.color = rgb;
    }
    x -= 0.5f;
    y -= 0.5f;
    const Vertex vertices[] = {
        {x,y,u,v}, {x+w,y,u+uw,v}, {x,y+h,u,v+vh},
        {x,y+h,u,v+vh}, {x+w,y,u+uw,v}, {x+w,y+h,u+uw,v+vh}
    };
    union { Vertex vertex; uint32_t words[4]; } packed;
    for (unsigned i = 0; i < 6; i++) {
        packed.vertex = vertices[i];
        for (unsigned j = 0; j < 4; j++) canvas.vertices[canvas.count * 4 + j] = packed.words[j];
        canvas.count++;
    }
}

void dash_rect(float x, float y, float w, float h, uint32_t rgb) {
    quad(x, y, w, h, 1.5f / ATLAS_WIDTH, 1.5f / ATLAS_HEIGHT, 0, 0, rgb);
}

void dash_border(float x, float y, float w, float h, float t, uint32_t rgb) {
    if (t <= 0.0f) return;
    dash_rect(x, y, w, t, rgb);
    dash_rect(x, y + h - t, w, t, rgb);
    dash_rect(x, y + t, t, h - 2.0f * t, rgb);
    dash_rect(x + w - t, y + t, t, h - 2.0f * t, rgb);
}

void dash_pill(float x, float y, float w, float h, uint32_t rgb) {
    if (w <= h) {
        dash_icon(DASH_CIRCLE, x, y, h, rgb);
        return;
    }
    const float r = h * 0.5f;
    dash_icon(DASH_CIRCLE, x, y, h, rgb);
    dash_icon(DASH_CIRCLE, x + w - h, y, h, rgb);
    dash_rect(x + r, y, w - h, h, rgb);
}

void dash_icon(dash_icon_t icon, float x, float y, float size, uint32_t rgb) {
    static const float start[]  = {768, 76, 132, 196, 256, 320, 384, 448, 512};
    static const float width[]  = {256, 52,  64,  52,  64,  64,  64,  64,  64};
    static const float height[] = {256, 64,  64,  64,  64,  64,  64,  64,  64};
    quad(x, y, size, size, (start[icon] + 0.5f) / ATLAS_WIDTH, 0.5f / ATLAS_HEIGHT,
         (width[icon] - 1) / ATLAS_WIDTH, (height[icon] - 1) / ATLAS_HEIGHT, rgb);
}

void dash_art(dash_art_t art, float x, float y, float w, float h, uint32_t rgb) {
    static const struct { float u, v, w, h; } regions[] = {
        {0, 512, 512, 288}, {512, 512, 512, 288}, {0, 800, 8, 128},
    };
    quad(x, y, w, h, (regions[art].u + .5f) / ATLAS_WIDTH,
         (regions[art].v + .5f) / ATLAS_HEIGHT, (regions[art].w - 1) / ATLAS_WIDTH,
         (regions[art].h - 1) / ATLAS_HEIGHT, rgb);
}

static unsigned next_codepoint(const unsigned char **pp) {
    const unsigned char *p = *pp;
    unsigned point = *p++;
    if (point >= 0xc2 && point <= 0xc3 && (*p & 0xc0) == 0x80) {
        point = ((point & 0x1f) << 6) | (*p++ & 0x3f);
    } else if (point >= 128) {
        while ((*p & 0xc0) == 0x80) p++;
        point = '?';
    }
    if (point < 32) point = ' ';
    if (point >= 127 && point < 160) point = '?';
    *pp = p;
    return point;
}

float dash_text_width(dash_font_t font, const char *text) {
    if (!text) return 0.0f;
    float w = 0.0f;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        const unsigned point = next_codepoint(&p);
        const Glyph *glyph = &glyphs[font][point < 127 ? point - 32 : point - 65];
        w += glyph->advance;
    }
    return w;
}

void dash_text(dash_font_t font, float x, float baseline, float max_width, uint32_t rgb, const char *text) {
    if (!text) return;
    const float limit = x + max_width;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        const unsigned point = next_codepoint(&p);
        const Glyph *glyph = &glyphs[font][point < 127 ? point - 32 : point - 65];
        if (x + glyph->advance > limit || x + glyph->left + glyph->width > limit) break;
        if (glyph->width && glyph->height) {
            quad(x + glyph->left, baseline + glyph->top, glyph->width, glyph->height,
                 (float)glyph->x / ATLAS_WIDTH, (float)glyph->y / ATLAS_HEIGHT,
                 (float)glyph->width / ATLAS_WIDTH, (float)glyph->height / ATLAS_HEIGHT, rgb);
        }
        x += glyph->advance;
    }
}
