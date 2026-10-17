/* SPDX-License-Identifier: MIT */
#ifndef BOOT_SPLASH_H
#define BOOT_SPLASH_H

#include "autoconf.h"

#ifdef CONFIG_SAMA7D65_CURIOSITY_SPLASH
void boot_splash_power(void);
void boot_splash_early_init(void);
void boot_splash_show(void);
void boot_splash_progress(unsigned int copied, unsigned int total);
void boot_splash_error(void);
unsigned int boot_splash_load_limit(void);
#else
static inline void boot_splash_power(void) {}
static inline void boot_splash_early_init(void) {}
static inline void boot_splash_show(void) {}
static inline void boot_splash_progress(unsigned int copied, unsigned int total) {}
static inline void boot_splash_error(void) {}
#endif
#endif
