#pragma once

#include <stdint.h>

void random_init(uint32_t seed0, uint32_t seed1);
void random_mix_u32(uint32_t value);
void random_get_bytes(void *buf, uint32_t len);

