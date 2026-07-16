/**
 * rng.h — simple xorshift PRNG, seeded from ADC noise at boot.
 *
 * Purpose: provide firmware-wide pseudo-randomness (quote picks, ids,
 * bubbles) from one seeded generator.
 */
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

void     rng_seed(void);   /* call once, pre-scheduler (uses the raw ADC) */
uint32_t rng_next(void);

#endif /* RNG_H */
