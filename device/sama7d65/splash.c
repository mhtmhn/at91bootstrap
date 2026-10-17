/* SPDX-License-Identifier: MIT */
/* Non-Pro Curiosity: timings and LVDS mapping from app/zero_gfx/app/app.overlay.
 * Register layout: Microchip SAMA7D6 DS60001851E, sections 48 and 51.
 * LCDC cannot fetch general SRAM: scanout uses the final 64 KiB of DDR.
 */
#include "boot_splash.h"
#include "hardware.h"
#include "arch/at91_pio.h"
#include "arch/at91_pmc/pmc.h"
#include "gpio.h"
#include "pmc.h"
#include "timer.h"
#include "debug.h"
#include "umctl2.h"
#include "splash_logo.h"

#define LCDC 0xe1400000U
#define LVDSC 0xe1408000U
#define DDR_START 0x60000000U
#define DDR_RESERVE 0x10000U
#define BACKGROUND 0x000a0c18U
#define LCDCFG0 0x00
#define LCDCFG1 0x04
#define LCDCFG2 0x08
#define LCDCFG3 0x0c
#define LCDCFG4 0x10
#define LCDCFG5 0x14
#define LCDCFG6 0x18
#define LCDEN 0x20
#define LCDSR 0x28
#define LCDIDR 0x30
#define ATTRE 0x3c
#define ATTRS 0x40
#define BASEIDR 0x64
#define BASEEN 0x70
#define BASECFG0 0x7c
#define BASECFG1 0x80
#define BASECFG3 0x88
#define BASECFG4 0x8c
#define OVR1IDR 0x164
#define OVR1EN 0x170
#define OVR1FBA 0x178
#define OVR1CFG0 0x17c
#define OVR1CFG1 0x180
#define OVR1CFG2 0x184
#define OVR1CFG3 0x188
#define OVR1CFG4 0x18c
#define OVR1CFG5 0x190
#define OVR1CFG6 0x194
#define OVR1CFG9 0x1a0
#define HEOIDR 0x364
#define HEOEN 0x370
#define OVR1CLUT 0xb00
#define LCD_SIP (1U << 4)
#define ATTR_SIP (1U << 31)
#define ATTR_BASE 1U
#define ATTR_OVR1 2U
#define ATTR_HEO 8U
#define LAYER_REPEAT 2U
#define LAYER_DMA 1U
/* Opaque color: source ONE (0), destination ZERO (0); alpha source ONE (1). */
#define OVR_BLEND (1U << 9)

static unsigned char *framebuffer;
static unsigned int display_ready;
static unsigned int last_width;

typedef char splash_fits_reservation[(SPLASH_BYTES <= DDR_RESERVE) ? 1 : -1];

static int wait_bits(unsigned int address, unsigned int mask, unsigned int value)
{
	unsigned int tries;
	for (tries = 0; tries < 10000; tries++) {
		if ((readl(address) & mask) == value)
			return 0;
		udelay(10);
	}
	return -1;
}

static int lcd_config(unsigned int offset, unsigned int value)
{
	writel(value, LCDC + offset);
	return wait_bits(LCDC + LCDSR, LCD_SIP, 0);
}

unsigned int boot_splash_load_limit(void)
{
	return DDR_START + get_ddram_size() - DDR_RESERVE;
}

void boot_splash_power(void)
{
	pmc_enable_periph_clock(AT91C_ID_PIOB, PMC_PERIPH_CLK_DIVIDER_NA);
	pio_set_gpio_output(AT91C_PIN_PB(14), 0);
	pio_set_gpio_output(AT91C_PIN_PB(18), 1);
}

void boot_splash_early_init(void)
{
	unsigned int stage;
	/* 24 MHz crystal, 1013.6448 MHz VCO / 2 / 7 = 72.4032 MHz pixel. */
	unsigned int update = AT91C_PLL_UPDT_ID_(7) | AT91C_PLL_UPDT_STUPTIM_(6);
	writel(update, AT91C_BASE_PMC + PMC_PLL_UPDT);
	writel(AT91C_PLL_ACR_LOOP_FILTER_(0x1b) | AT91C_PLL_ACR_LOCK_THR_(4) |
	       AT91C_PLL_ACR_CONTROL_(0x10), AT91C_BASE_PMC + PMC_PLL_ACR);
	writel(AT91C_PLL_CTRL1_MUL_(41) | AT91C_PLL_CTRL1_FRACR_(986500),
	       AT91C_BASE_PMC + PMC_PLL_CTRL1);
	writel(update | AT91C_PLL_UPDT_UPDATE, AT91C_BASE_PMC + PMC_PLL_UPDT);
	writel(AT91C_PLL_CTRL0_ENLOCK | AT91C_PLL_CTRL0_ENPLL |
	       AT91C_PLL_CTRL0_DIVPMC_(1) | AT91C_PLL_CTRL0_ENPLLCK,
	       AT91C_BASE_PMC + PMC_PLL_CTRL0);
	writel(update | AT91C_PLL_UPDT_UPDATE, AT91C_BASE_PMC + PMC_PLL_UPDT);
	if (wait_bits(AT91C_BASE_PMC + PMC_PLL_ISR0, 1U << 7, 1U << 7))
		goto failed;
	pmc_enable_periph_clock(56, PMC_PERIPH_CLK_DIVIDER_NA);
	pmc_enable_periph_clock(104, PMC_PERIPH_CLK_DIVIDER_NA);
	writel(0, LVDSC);
	if (wait_bits(LVDSC + 0x0c, 1, 0))
		goto failed;
	writel(0, LVDSC + 0x04); /* 24-bit, unbalanced, VESA */
	writel(0, LVDSC + 0x08);
	writel(9, LVDSC + 0x14); /* DCBIAS, no pre-emphasis */
	writel(1, LVDSC);
	writel(~0U, LCDC + LCDIDR);
	writel(~0U, LCDC + BASEIDR);
	writel(~0U, LCDC + OVR1IDR);
	writel(~0U, LCDC + HEOIDR);
	if (lcd_config(LCDCFG0, 3) || /* CLKPOL=1, bypass; slow PWM source */
	    lcd_config(LCDCFG6, (255U << 8) | (1U << 4)) ||
	    lcd_config(LCDCFG1, 31U | (5U << 16)) ||
	    lcd_config(LCDCFG2, 15U | (15U << 16)) ||
	    lcd_config(LCDCFG3, 47U | (79U << 16)) ||
	    lcd_config(LCDCFG4, 1279U | (799U << 16)) ||
	    lcd_config(LCDCFG5, 7U | (1U << 7) | (5U << 8) |
		       (1U << 11) | (1U << 16)))
		goto failed;
	if (wait_bits(LCDC + ATTRS, ~0U, 0))
		goto failed;
	writel(4U << 4, LCDC + BASECFG0);
	writel(0, LCDC + BASECFG1);
	writel(BACKGROUND, LCDC + BASECFG3);
	writel(LAYER_REPEAT, LCDC + BASECFG4); /* BASE DMA permanently off */
	writel(1, LCDC + BASEEN);
	writel(0, LCDC + OVR1EN);
	writel(0, LCDC + HEOEN);
	writel(ATTR_BASE | ATTR_OVR1 | ATTR_HEO, LCDC + ATTRE);
	if (wait_bits(LCDC + ATTRS, ATTR_SIP, 0))
		goto failed;
	/* CLKEN -> SYNCEN -> DISPEN, then SDEN (SDSTS clears). */
	for (stage = 1; stage <= 4; stage <<= 1) {
		writel(stage, LCDC + LCDEN);
		if (wait_bits(LCDC + LCDSR, LCD_SIP, 0) ||
		    wait_bits(LCDC + LCDSR, stage, stage))
			goto failed;
	}
	writel(1U << 5, LCDC + LCDEN);
	if (wait_bits(LCDC + LCDSR, LCD_SIP | (1U << 5), 0))
		goto failed;
	display_ready = 1;
	pio_set_value(AT91C_PIN_PB(14), 1);
	dbg_info("Splash: background on, BASE DMA off\n");
	return;
failed:
	pio_set_value(AT91C_PIN_PB(14), 0);
	display_ready = 0;
	dbg_info("Splash: display timeout; continuing boot\n");
}

static void set_pixel(unsigned int x, unsigned int y, unsigned int color)
{
	unsigned int offset = y * (SPLASH_WIDTH / 2) + x / 2;
	unsigned int shift = (x & 1) * 4;
	framebuffer[offset] = (framebuffer[offset] & ~(15U << shift)) | (color << shift);
}

static unsigned int coverage(unsigned int x, unsigned int y, unsigned int width)
{
	if (x >= width || width < 6)
		return 0;
	if (x < 3)
		return splash_caps[y * 3 + x];
	if (x >= width - 3)
		return splash_caps[y * 3 + width - 1 - x];
	return 15;
}

static void draw_bar(unsigned int width)
{
	unsigned int x, y, track, fill;
	for (y = 0; y < SPLASH_BAR_HEIGHT; y++) {
		for (x = 0; x < SPLASH_BAR_WIDTH; x++) {
			track = (3 * coverage(x, y, SPLASH_BAR_WIDTH) + 7) / 15;
			fill = coverage(x, y, width);
			set_pixel(SPLASH_BAR_X + x, SPLASH_BAR_Y + y,
				  fill > track ? fill : track);
		}
	}
	asm volatile("dsb sy" ::: "memory");
}

void boot_splash_show(void)
{
	unsigned int i = 0, out = 0, count, control, color;
	if (!display_ready)
		return;
	framebuffer = (unsigned char *)boot_splash_load_limit();
	while (i < sizeof(splash_rle)) {
		control = splash_rle[i++];
		if (control == 128)
			continue;
		count = control < 128 ? control + 1 : 257 - control;
		if (count > SPLASH_BYTES - out ||
		    (control < 128 ? count : 1) > sizeof(splash_rle) - i)
			goto failed;
		if (control < 128) {
			while (count--)
				framebuffer[out++] = splash_rle[i++];
		} else {
			color = splash_rle[i++];
			while (count--)
				framebuffer[out++] = color;
		}
	}
	if (out != SPLASH_BYTES)
		goto failed;
	draw_bar(0);
	if (wait_bits(LCDC + ATTRS, ~0U, 0))
		goto failed;
	writel(0, LCDC + OVR1CFG1); /* CLUTEN=GAM=0 for direct palette writes */
	for (i = 0; i < 16; i++)
		writel(splash_palette[i], LCDC + OVR1CLUT + i * 4);
	writel(4U << 4, LCDC + OVR1CFG0);
	writel(1U | (2U << 8), LCDC + OVR1CFG1); /* 4bpp CLUT */
	writel((1280 - SPLASH_WIDTH) / 2 | (((800 - SPLASH_HEIGHT) / 2) << 16),
	       LCDC + OVR1CFG2);
	writel((SPLASH_WIDTH - 1) | ((SPLASH_HEIGHT - 1) << 16), LCDC + OVR1CFG3);
	writel(0, LCDC + OVR1CFG4); /* no row skip, contiguous packed pixels */
	writel(0, LCDC + OVR1CFG5);
	writel(BACKGROUND, LCDC + OVR1CFG6);
	writel((unsigned int)framebuffer, LCDC + OVR1FBA);
	writel(LAYER_REPEAT | LAYER_DMA | OVR_BLEND, LCDC + OVR1CFG9);
	writel(1, LCDC + OVR1EN);
	writel(ATTR_OVR1, LCDC + ATTRE);
	if (wait_bits(LCDC + ATTRS, ~0U, 0))
		goto failed;
	dbg_info("Splash: logo on, framebuffer %x (%u bytes)\n",
		 (unsigned int)framebuffer, SPLASH_BYTES);
	return;
failed:
	framebuffer = 0;
	dbg_info("Splash: logo initialization failed; continuing boot\n");
}

void boot_splash_progress(unsigned int copied, unsigned int total)
{
	unsigned int low = 0, high = SPLASH_BAR_WIDTH, middle;
	unsigned long long scaled;
	if (!framebuffer || !total)
		return;
	/* No 32-bit product overflow and no 64-bit division runtime dependency. */
	scaled = (unsigned long long)copied * SPLASH_BAR_WIDTH;
	while (low < high) {
		middle = (low + high + 1) / 2;
		if ((unsigned long long)total * middle <= scaled)
			low = middle;
		else
			high = middle - 1;
	}
	if (low != last_width) {
		draw_bar(low);
		last_width = low;
	}
}

void boot_splash_error(void)
{
	/* Keep the last successfully copied fraction on screen; serial gives cause. */
	dbg_info("Splash: SD copy failed\n");
}
