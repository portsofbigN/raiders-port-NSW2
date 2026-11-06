// Block-linear <-> linear texel copies (nx2_tex_deswizzle.c, from TotK Recomp's tex_deswizzle.c; contracts in tex.h).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_tex.h"

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear,
                  size_t dst_pitch);
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear, size_t src_pitch,
                void *dst_storage);
int nvn_texels_box(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t x, uint32_t y, uint32_t z, uint32_t wb,
                   uint32_t h, void *storage, void *rows, size_t pitch, int to_linear);
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td);
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t *row_bytes, uint32_t *rows,
                       uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb);
void tex_bl_copy(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, uint32_t slices,
                 unsigned th, unsigned td, int to_linear);
