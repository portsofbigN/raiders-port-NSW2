// C interface of the in-game shader translator (nx2_xlate.cpp, Eglantine's shader_recompiler).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NX2X_CBUF, NX2X_SSBO, NX2X_TEXBUF, NX2X_IMGBUF, NX2X_TEXTURE, NX2X_IMAGE };
// Shader::TextureType values
enum { NX2X_T_1D, NX2X_T_ARRAY_1D, NX2X_T_2D, NX2X_T_ARRAY_2D, NX2X_T_3D, NX2X_T_CUBE, NX2X_T_ARRAY_CUBE,
       NX2X_T_BUFFER, NX2X_T_2D_RECT };

typedef struct {
    int kind;                       // NX2X_*
    uint32_t binding;               // descriptor set 0 binding (first of count)
    uint32_t count;
    uint32_t index;                 // constant buffers: cbuf slot
    uint32_t cbuf_index, cbuf_offset;   // others: where the handle / address lives
    int type;                       // textures / images: NX2X_T_*
    int is_depth, is_integer, is_written, is_multisample;
    int image_format;               // Shader::ImageFormat
    int has_secondary;
    uint32_t secondary_cbuf_index, secondary_cbuf_offset, shift_left, secondary_shift_left, size_shift;
} Nx2XDesc;

typedef struct {
    uint32_t *spirv;   // malloc'd
    size_t words;
    int stage;         // Shader::Stage: 0 vertex, 1 tess control, 2 tess eval, 3 geometry, 4 fragment, 5 compute
    int ndesc;
    Nx2XDesc desc[96];
    uint32_t loads_generic;    // vertex: generic attributes read (bit i)
    uint32_t stores_frag;      // fragment: colour outputs written (bit i)
    uint32_t workgroup[3], shared_size;
    int uses_rescaling, uses_render_area;
    uint32_t cbuf_used_size[18];
    char error[256];
} Nx2XStage;

typedef struct {
    void *user;
    // A 32-bit word of constant buffer `index` at `offset` of this stage (texture handles, ...)
    uint32_t (*cbuf_value)(void *user, int stage, uint32_t index, uint32_t offset);
    // The texture type (NX2X_T_*) and whether its format is integer, for a texture handle read from a cbuf
    int (*texture_type)(void *user, int stage, uint32_t handle);
    int (*texture_integer)(void *user, int stage, uint32_t handle);
    uint8_t attrib_types[32];   // Shader::AttributeType per generic input (0 float, 1 sint, 2 uint, 5 disabled)
    uint8_t color_types[8];     // Shader::AttributeType per colour output
    int convert_depth;          // NVN depth mode near = -w: convert to Vulkan's [0, 1]
    int y_negate;
    int topology;               // Shader::InputTopology (0 points, 1 lines, 2 lines adj., 3 triangles)
    uint32_t workgroup[3], shared_size;   // compute
    // Float controls the device has (VkPhysicalDeviceVulkan12Properties); all-zero options mean "all of them"
    int float_controls_set;
    int denorm_flush16, denorm_flush32, denorm_preserve16, denorm_preserve32, szinf_nan16, szinf_nan32, szinf_nan64;
} Nx2XOptions;

// Translates n stages of one program (graphics: stages in pipeline order, vertex first; compute: one stage).
// code[i]: the native stage as NVN holds it (0x80-byte header then SM86 code for graphics; code only for compute);
// size[i]: bytes readable there. Descriptor bindings continue across the stages. Returns 0 on success, else the
// index + 1 of the failed stage (its error is set).
int nx2_xlate_program(int n, const void *const *code, const size_t *size, int compute, const Nx2XOptions *opt,
                      Nx2XStage *out);
void nx2_xlate_free(Nx2XStage *stages, int n);

#ifdef __cplusplus
}
#endif
