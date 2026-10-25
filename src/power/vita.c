/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer, Sunguk Lee
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */
#include <stdbool.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include "../config.h"
#include "../debug.h"

enum {
  ENABLE_ALL = 0,
  DISABLE_SUSPEND = 1,
};

static int powermode = ENABLE_ALL;
static bool active_power_thread = false;

static struct {
  bool saved;
  int arm, bus, gpu, gpu_xbar;
} stock_clocks;

int vitapower_thread(SceSize args, void *argp) {
  while (1) {
    if (!active_power_thread) {
      sceKernelDelayThread(10 * 1000 * 1000);
      continue;
    }
    if (powermode & DISABLE_SUSPEND) {
      sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
      sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
    }
    if (!scePowerIsBatteryCharging() && scePowerIsLowBattery()) {
      // TODO print warning message
    }
    sceKernelDelayThread(10 * 1000 * 1000);
  }

  return 0;
}

bool vitapower_init() {
  SceUID thid = sceKernelCreateThread("vitapower_thread", vitapower_thread, 0x10000100, 0x40000, 0, 0, NULL);
  if (thid >= 0)
    sceKernelStartThread(thid, 0, NULL);

  return true;
}

void vitapower_config(CONFIGURATION config) {
  powermode = ENABLE_ALL;

  if (config.disable_powersave) {
    powermode |= DISABLE_SUSPEND;
  }
}

static void boost_clocks() {
  if (!config.performance_mode || stock_clocks.saved) {
    return;
  }
  stock_clocks.arm = scePowerGetArmClockFrequency();
  stock_clocks.bus = scePowerGetBusClockFrequency();
  stock_clocks.gpu = scePowerGetGpuClockFrequency();
  stock_clocks.gpu_xbar = scePowerGetGpuXbarClockFrequency();
  stock_clocks.saved = true;

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);
  vita_debug_log("clocks boosted: arm %d bus %d gpu %d xbar %d (stock %d/%d/%d/%d)\n",
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                 scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(),
                 stock_clocks.arm, stock_clocks.bus, stock_clocks.gpu, stock_clocks.gpu_xbar);
}

static void restore_clocks() {
  if (!stock_clocks.saved) {
    return;
  }
  scePowerSetArmClockFrequency(stock_clocks.arm);
  scePowerSetBusClockFrequency(stock_clocks.bus);
  scePowerSetGpuClockFrequency(stock_clocks.gpu);
  scePowerSetGpuXbarClockFrequency(stock_clocks.gpu_xbar);
  stock_clocks.saved = false;
}

void vitapower_start() {
  active_power_thread = true;
  boost_clocks();
}

void vitapower_stop() {
  active_power_thread = false;
  restore_clocks();
}
