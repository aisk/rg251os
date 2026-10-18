#ifndef FX_H
#define FX_H

#include <stdint.h>

#include "blit.h"

#define FX_LUT_SIZE 32768

/* Menu steps for the grid options (any value in range is accepted). */
extern const int grid_dim_levels[], grid_boost_levels[];
extern const int grid_dim_count, grid_boost_count;

/*
 * Fill the tables for one combination: lit always, dark only when not
 * NULL (grid on). dim and boost are percent.
 */
void fx_build(uint16_t *lit, uint16_t *dark, int color, int dim, int boost);

/* Rebuild the active tables from settings and rom_settings if they
 * changed. Cheap when nothing changed. */
void fx_update(void);
/* The active effects for the game blit, NULL when everything is off. */
const struct blit_fx *fx_current(void);

#endif
