#pragma once

#define SWAP_FLAG_PREFER 0x8000

int swapon(const char *path, int flags);
int swapoff(const char *path);
