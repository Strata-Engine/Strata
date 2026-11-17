#pragma once
#include <stdint.h>

int strata_browser_ready(void);
int strata_browser_eye_cell(int axis);
/* Returns -1 for an unloaded or out-of-height cell. */
int strata_browser_voxel(const int cell[3]);
int strata_browser_edit(const int cell[3], int32_t value);
