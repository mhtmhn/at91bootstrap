# Display handover to Zephyr

The splash (`device/sama7d65/splash.c`) leaves the display running when it jumps to
Zephyr. With `CONFIG_MICROCHIP_SAM_XLCDC_BOOT_HANDOVER=y` (default) the Zephyr XLCDC
driver keeps it running, so the logo stays on screen until the application's first frame.
Nothing in Zephyr is told what the bootloader did: the driver reads the hardware and decides.

## What the bootloader leaves running

| Block | State |
| --- | --- |
| LVDSPLL (PLL ID 7) | 24 MHz x (41 + 1 + 986500 / 2^22) / 2 / 7 = 72 403 200 Hz pixel clock, locked |
| LVDSC | 24-bit, DC-unbalanced, VESA mapping, DCBIAS 9, no pre-emphasis, serializer on |
| LCDC timing | 1280 x 800, HSPW/HFP/HBP 32/48/80, VSPW/VFP/VBP 6/16/16, CLKPOL=1, bypass, guard time 1 |
| LCDC start | CLKEN, SYNCEN, DISPEN, SDEN done; PWM backlight not enabled by the LCDC |
| BASE | enabled, DMA off: shows its default colour 0x0A0C18 |
| OVR1 | enabled, 4 bpp CLUT, 366 x 152 centred, framebuffer at the top 64 KiB of DDR (`boot_splash_load_limit()`), 27 816 B |
| HEO | disabled |
| Interrupts | all LCDC interrupts disabled |
| GPIO | PB18 (panel power) and PB14 (backlight) high |

## What Zephyr checks before keeping it

Everything is read back from the registers; nothing about the bootloader's layer use is
assumed, so another bootloader (or another SoC, e.g. SAM9X75) can configure it differently.

1. LCDC peripheral clock on and the controller running (clock, sync and display status, no
   synchronisation in progress).
2. The pixel clock computed from the LVDSPLL registers within 0.1 % of the devicetree's
   `clock-frequency`.
3. `LCDC_LCDCFG0` clock bits and `LCDC_LCDCFG1..5` equal to the values the driver would
   write from the devicetree (timings, polarities, output mode, guard time).
4. LVDSC peripheral clock on and `CFGR`/`UCBR`/`ACR` equal to the devicetree's. The
   serializer is then never stopped (a panel smears its image when the LVDS link drops);
   its enable cannot be read back (`CR` is write-only, `SR.CS` reads 0 while running), so
   Zephyr writes `SER_EN` again, which leaves a running serializer as it is.

If any check fails, Zephyr stops the controller (datasheet power-down sequence), switches
the LVDSPLL off and does its normal full init. The panel blanks for a moment; nothing breaks.

## Each layer

Checked from its own registers, the same way for every layer (BASE, OVR1, HEO, OVR2):

| Layer reads | Zephyr does |
| --- | --- |
| nothing (DMA off), or memory outside Zephyr's RAM (`zephyr,sram`) | keeps it as it is |
| free RAM inside `zephyr,sram` | copies the pixels into the system heap and points the layer at the copy, before the kernel hands out free RAM (PRE_KERNEL_2) |
| the Zephyr image itself, a YCbCr or pixel-stride layout, or the copy fails | switches the layer off (BASE: DMA off, so it shows its default colour) |
| (layer has no devicetree node) | kept until release, then switched off |

The splash buffer lies inside Zephyr's RAM (Zephyr uses all of DDR), so it is the "copy"
case. The copy is the size the layer actually scans out (27 816 B for this logo), comes from
the kernel heap (`CONFIG_HEAP_MEM_POOL_SIZE`) and is freed as soon as the application's
first frame replaces it. No DDR has to be reserved, and the bootloader may move its buffer
anywhere in DDR, at any DDR size.

**Keep the boot screen small.** The copy needs heap space of the same size; a full-screen
image would need a full-screen allocation. If the allocation fails the layer is switched off
(no corruption, the logo disappears early).

## Release

Layer settings the application makes are staged but not committed while the boot screen is
up. The first `display_blanking_off()` (or `display_blanking_on()`) commits every layer in
one `LCDC_ATTRE` write, so the application's screen replaces the splash in one frame. Then
the copies are freed. An application that never calls `display_blanking_off()` keeps the
splash on screen.

The backlight PWM is reprogrammed from the devicetree at handover (`LCDCFG0` PWM source,
`LCDCFG6`).

## Rules for changing the splash

- Use timings, polarities, guard time, LVDS mapping and pixel clock **identical** to the
  Zephyr devicetree (`app/zero_gfx/app/app.overlay`), or Zephyr restarts the display.
- Any layer, format (RGB or CLUT, any bpp), position and size is fine; YCbCr and pixel
  stride are not kept.
- The framebuffer must stay in DDR (LCDC DMA cannot read SRAM) and outside the Zephyr image
  (the copy happens before the kernel uses free RAM, so anywhere above the loaded image is safe).
- Leave LCDC interrupts disabled; Zephyr masks them anyway before installing its handler.
- A CLUT layer keeps its palette in the layer's palette registers, whether written directly
  (as the splash does) or loaded once by DMA (`xxxCLA` + `ATTRE.xxxCL`); Zephyr does not
  reload it, so a DMA palette's source buffer need not survive.

## Verified on the board (SD boot, 2026-10-17)

- LVDS link kept, controller kept at 72 403 199 Hz, the logo shown from a heap copy, the
  switch to the application's first frame clean (no smear), 0 underruns.
- HEO works when first enabled after the controller started (the splash never enables it),
  so an old driver note saying HEO must be enabled before start does not hold.
- Restarting the LVDS serializer under a running controller smears the panel from top to
  bottom for a moment; that is why the LVDS driver keeps a matching link running.

Not tested yet: the restart path from a splash whose timings differ from the devicetree.
