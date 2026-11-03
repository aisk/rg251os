/* Renderer/decoder regression entry point shared by the callback harness,
 * and the quarter-pixel position of a fighter at pixel column px. */
#ifndef FIGHTER_TEST_RENDER_H
#define FIGHTER_TEST_RENDER_H
#include <stddef.h>
#include <stdint.h>
void render_tests(const uint8_t *pack, size_t len);
#define QX(px) ((px) * 4)
#endif
