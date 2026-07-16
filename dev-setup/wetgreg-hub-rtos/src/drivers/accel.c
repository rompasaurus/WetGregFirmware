/**
 * accel.c — SC7A20 accelerometer driver (I2C0 on GP0/GP1).
 *
 * Purpose: own all SC7A20 register I/O: probe/init, the 6-byte burst read,
 * and the raw-to-engineering-unit conversions.
 *
 * PCB wiring (U1): SDO->GND => I2C addr 0x18, CS->3V3 => I2C mode, INT1->GP15.
 * Differences from the old MPU driver:
 *   - address 0x18 (not 0x68)
 *   - WHO_AM_I at 0x0F (SC7A20 reports 0x11; ST parts report 0x33)
 *   - accel data at OUT_X_L 0x28, LITTLE-endian (low byte first)
 *   - multi-byte reads REQUIRE the auto-increment bit (0x80) in the sub-address
 *   - no gyro, no usable temperature (those vars stay 0)
 */
#include "accel.h"

#include <math.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define MPU_SDA    0   /* GP0 = pin 1 */
#define MPU_SCL    1   /* GP1 = pin 2 */

/* LIS2DH / LIS3DH / SC7A20 registers */
#define SC_WHO_AM_I   0x0F
#define SC_CTRL_REG1  0x20
#define SC_CTRL_REG4  0x23
#define SC_OUT_X_L    0x28
#define SC_AUTO_INC   0x80   /* OR into the sub-address for multi-byte transfer */

static uint8_t mpu_addr = 0x18;  /* SC7A20 SDO=GND -> 0x18 (0x19 if SDO=VDD) */

bool mpu_ok = false;

/* Raw sensor data (accel only — SC7A20 has no gyro/temp) */
int16_t accel_x, accel_y, accel_z;
int16_t gyro_x, gyro_y, gyro_z;   /* unused on SC7A20 — held at 0 */
float   mpu_temp_c;               /* unused on SC7A20 — held at 0 */

/* ALWAYS use the *_timeout_us variants — never the plain _blocking ones — so a
 * missing / half-soldered / bus-held-low accelerometer can never hang a task
 * (especially the boot-time probe). A no-device bus then just returns an error
 * and mpu_ok stays false; the firmware runs fine without orientation/steps. */

static bool mpu_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(MPU_I2C, mpu_addr, buf, 2, false, MPU_I2C_TIMEOUT_US) == 2;
}

static bool mpu_read_burst(uint8_t reg, uint8_t *dst, uint8_t len) {
    reg |= SC_AUTO_INC;  /* SC7A20/LIS2DH need this for multi-byte reads */
    if (i2c_write_timeout_us(MPU_I2C, mpu_addr, &reg, 1, true, MPU_I2C_TIMEOUT_US) < 0) return false;
    return i2c_read_timeout_us(MPU_I2C, mpu_addr, dst, len, false, MPU_I2C_TIMEOUT_US) == (int)len;
}

void mpu_init(void) {
    i2c_init(MPU_I2C, 400 * 1000);  /* 400 kHz */
    gpio_set_function(MPU_SDA, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA);
    gpio_pull_up(MPU_SCL);
    sleep_ms(20);  /* SC7A20 boot/turn-on time */

    /* SC7A20 SDO->GND is 0x18 on the WetGreg PCB; probe 0x18 then 0x19. */
    uint8_t try_addrs[] = {0x18, 0x19};
    bool found = false;
    for (int a = 0; a < 2; a++) {
        uint8_t addr = try_addrs[a];
        uint8_t reg = SC_WHO_AM_I;
        uint8_t who = 0;
        int w = i2c_write_timeout_us(MPU_I2C, addr, &reg, 1, true, MPU_I2C_TIMEOUT_US);
        int r = i2c_read_timeout_us(MPU_I2C, addr, &who, 1, false, MPU_I2C_TIMEOUT_US);
        printf("[ACCEL] Probe 0x%02X: w=%d r=%d who=0x%02X\n", addr, w, r, who);
        if (w >= 0 && r >= 0) {
            mpu_addr = addr;
            found = true;
            /* Known IDs: SC7A20=0x11, ST LIS2DH12/LIS3DH=0x33. Accept either;
               warn (but still try) if a clone reports something else yet ACKs. */
            if (who != 0x11 && who != 0x33)
                printf("[ACCEL] Unexpected WHO_AM_I 0x%02X (continuing anyway)\n", who);
            printf("[ACCEL] SC7A20 found at 0x%02X\n", addr);
            break;
        }
    }

    if (!found) {
        printf("[ACCEL] NOT DETECTED on I2C0 (tried 0x18/0x19)\n");
        mpu_ok = false;
        return;
    }

    /* CTRL_REG1 0x57: 100 Hz ODR, normal mode, X/Y/Z enabled. */
    mpu_write_reg(SC_CTRL_REG1, 0x57);
    /* CTRL_REG4 0x88: block-data-update, +/-2g full scale, high-resolution. */
    mpu_write_reg(SC_CTRL_REG4, 0x88);
    sleep_ms(10);
    mpu_ok = true;
    printf("[ACCEL] SC7A20 initialized OK at 0x%02X\n", mpu_addr);
}

void mpu_read_all(void) {
    if (!mpu_ok) return;

    /* 6 bytes from OUT_X_L (auto-increment): XL,XH,YL,YH,ZL,ZH. Data is
       left-justified 16-bit (12 significant bits) and LITTLE-endian. */
    uint8_t buf[6];
    if (!mpu_read_burst(SC_OUT_X_L, buf, 6)) return;

    accel_x = (int16_t)((buf[1] << 8) | buf[0]);
    accel_y = (int16_t)((buf[3] << 8) | buf[2]);
    accel_z = (int16_t)((buf[5] << 8) | buf[4]);

    /* SC7A20 has no gyro and no usable temperature path here. */
    gyro_x = gyro_y = gyro_z = 0;
    mpu_temp_c = 0.0f;
}

/* Convert raw accel to g. +/-2g high-res ~= 16384 LSB/g (same scale as before). */
float accel_g(int16_t raw) { return raw / 16384.0f; }

/* Convert raw gyro to deg/s (at +/-250, 131 LSB/(deg/s)) */
float gyro_dps(int16_t raw) { return raw / 131.0f; }

/* Magnitude of acceleration vector in g */
float accel_magnitude(void) {
    float ax = accel_g(accel_x);
    float ay = accel_g(accel_y);
    float az = accel_g(accel_z);
    return sqrtf(ax * ax + ay * ay + az * az);
}

/* Tilt angles in degrees */
float tilt_x_deg(void) {
    return atan2f(accel_g(accel_x), sqrtf(accel_g(accel_y) * accel_g(accel_y) + accel_g(accel_z) * accel_g(accel_z))) * 57.2958f;
}
float tilt_y_deg(void) {
    return atan2f(accel_g(accel_y), sqrtf(accel_g(accel_x) * accel_g(accel_x) + accel_g(accel_z) * accel_g(accel_z))) * 57.2958f;
}
