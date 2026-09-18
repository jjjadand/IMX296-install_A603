/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-in for NVIDIA generated nvidia/conftest.h.
 *
 * JetPack 6.2 / L4T 36.4.3 / kernel 5.15.148-tegra:
 *  - i2c_driver::probe still takes (client, id)
 *  - i2c_driver::remove still returns int
 *  - struct camera_common_data has NO tegra_pmc *pmc field
 *
 * Do NOT define NV_TEGRA_PMC_IO_PAD_POWER_ENABLE_PRESENT here. That
 * macro inserts pmc into camera_common_data on JP7 and shifts priv /
 * mode / numlanes relative to the prebuilt tegra-camera.ko.
 */
#ifndef _NV_CONFTEST_SHIM_H
#define _NV_CONFTEST_SHIM_H

#include <linux/version.h>

#define NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT 1

#endif /* _NV_CONFTEST_SHIM_H */
