/**
 * rng.c — xorshift32 PRNG seeded from ADC noise (floating GP26) + the µs timer.
 *
 * Purpose: implement the xorshift32 generator and its boot-time ADC-noise
 * seeding.
 */
#include "rng.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"

static uint32_t rng_state;

void rng_seed(void) {
    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);
    uint32_t seed = 0;
    for (int i = 0; i < 32; i++)
        seed = (seed << 1) | (adc_read() & 1);
    seed ^= time_us_32();
    rng_state = seed ? seed : 0xDEADBEEF;
}

uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
