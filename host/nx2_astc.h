// ASTC LDR decoder (nx2_astc.c, from TotK / LAS Recomp, MIT).
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool astc_decode_block(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint8_t *rgba_out);
// blocks: ceil(width / bw) x ceil(height / bh) 16-byte blocks in rows; rgba_out: width x height RGBA8.
void astc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                     uint8_t *rgba_out);
