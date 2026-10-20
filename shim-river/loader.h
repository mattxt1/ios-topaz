#pragma once
#include <stdint.h>

/* Locate the SWEETXNU payload (the boot image ramdisk); NULL on success. */
const char *loader_find(uintptr_t dtb, uintptr_t *start, uintptr_t *end);
/* Boot it; returns only on error. */
const char *loader_boot(uintptr_t start, uintptr_t end);
