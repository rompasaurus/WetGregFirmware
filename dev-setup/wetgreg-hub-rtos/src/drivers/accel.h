/**
 * accel.h — SC7A20 accelerometer (I2C0 on GP0/GP1).
 *
 * Purpose: expose the accelerometer's readings and unit conversions to the
 * motion services and the debug screens.
 *
 * The WetGreg PCB fits an SC7A20HTR — LIS2DH12 / LIS3DH register-compatible,
 * NOT an MPU-6050. Variable/function names (accel_x.., mpu_ok, accel_g,
 * tilt_*) are kept from the old MPU driver so the UI/render code reads
 * unchanged; there is no gyro and no usable temperature on this part
 * (those values are held at 0).
 */
#ifndef ACCEL_H
#define ACCEL_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

/* Bus + timeout are public so the I2C-scan debug screen can probe the
 * same bus this driver owns. */
#define MPU_I2C            i2c0
#define MPU_I2C_TIMEOUT_US 3000

/* Raw sensor data — written only by mpu_read_all() (Housekeeping task),
 * read by the UI screens. Aligned 16-bit stores are atomic on the M33 and
 * both tasks are pinned to core 0 (see rtos_tasks.h). */
extern bool    mpu_ok;
extern int16_t accel_x, accel_y, accel_z;
extern int16_t gyro_x, gyro_y, gyro_z;   /* unused on SC7A20 — held at 0 */
extern float   mpu_temp_c;               /* unused on SC7A20 — held at 0 */

void  mpu_init(void);
void  mpu_read_all(void);

/* Conversions / derived values (raw counts → g, deg/s, degrees). */
float accel_g(int16_t raw);
float gyro_dps(int16_t raw);
float accel_magnitude(void);
float tilt_x_deg(void);
float tilt_y_deg(void);

#endif /* ACCEL_H */
