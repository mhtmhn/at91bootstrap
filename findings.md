# SAMA7D65 Curiosity: early Zephyr loading display findings

Research date: 2026-10-16. Target: `configs/sama7d65_curiositysd1_zephyr_defconfig`, the non-Pro Curiosity board. Reference: `C:/Git/zephyr-mchp-workspace/app/zero_gfx`. The original research and unchanged baseline build are retained below. A first-pass display/SD-loader implementation was subsequently authorized and built; see the final section for current measurements and test instructions. LCDC DMA cannot access general SRAM; the logo framebuffer resides in DDR.

**Current status:** the user tested the delivered first-pass boot.bin on their board and reported **"works great!"**. The implementation is working and remains uncommitted. See [SESSION_STATE.md](SESSION_STATE.md) for the fresh-session handoff, tool paths, source inventory, current artifact/hash, validation scope and remaining measurements.

## Objective and panel

Bring the display up as early as possible, then show progress while copying a large `zephyr.bin` from SD into DDR. The current reference panel is 1280 x 800 LVDS, with 72,403,200 Hz pixel clock, horizontal sync/front/back porch 32/48/80, vertical sync/front/back porch 6/16/16, and 1440 x 838 total pixels at 60 Hz. Panel power is PB18, backlight is PB14, serializer mapping is VESA, unbalanced, 24-bit output. Keep full-panel timings even when the overlay is small. Sources: reference `app/app.overlay`, XLCDC and LVDSC display drivers.

## Earliest initialization

Assert panel power early so its settling time can overlap clock setup. Configure LVDSPLL, LCDC/LVDSC peripheral clocks, timing, BASE default background and OVR1 before starting scanout. Start the pipeline and enable the backlight in the panel's required power-sequence order. Then continue DDR training, SD initialization and copying.

The local official Zephyr SAMA7D6 clock table assigns LCDC (ID 56) and LVDSC (ID 104) to MCK3; this is now also confirmed by datasheet page 63, Table 14.1. In Bootstrap, the existing earliest sensible insertion is after DDRPLL and MCK3 setup inside `hw_init()`, before DDR memory training. DDRPLL must be running, but DDR memory need not yet be initialized for a default-color display. Operation before training still needs hardware validation. A logo must wait for DDR training: datasheet pages 64-66, Tables 14.4/14.5/14.8, show LCDC connected only to UDDRC_P3, with no SRAM_P0 or SRAM_P1 path.

SAMA7D65 LVDSPLL index is 7. Bootstrap's SAMA7D65 PLL enumeration currently stops before that index, and the existing PLL helper rejects IDs at or above `PLL_ID_MAX`; it needs extension or a dedicated setup. Use fixed panel PLL settings rather than runtime searches in an early display path. The reference derives a 1,013,644,800 Hz VCO and 506,822,400 Hz serializer clock for this panel.

Keep synchronization polling, attribute commit and output sequencing. Audit serial output: `CONFIG_DEBUG=n` does not remove all `console_printf()` output. The conditional SDHC1 power cycle delay is one 50 ms wait and is disabled in the supplied defconfig (`SD_POWER_OR_RESET_CTRL` defaults to n). There is no 100 ms SD power delay in this configuration.

## Overlay choices

Keep BASE enabled with DMA=0 and a constant background, allocate no BASE framebuffer, and leave HEO DMA inactive. SAMA7D65 OVR1 is windowable but has no scaler.

For a plain solid-color progress bar, OVR1 also supports DMA=0, selecting its default-color register instead of memory. Grow its width based on copied bytes, use opaque blending, and hide it at zero percent (size fields encode width/height minus one). This uses zero framebuffer SRAM. Suggested maximum bar extent: 512 x 24 at (384, 388) on the 1280 x 800 panel. Validate this mode on hardware.

The initial generic logo/text/loading canvas suggestion was 512 x 96 at (384, 352), 4-bit indexed, 24,576 bytes (24 KiB), single buffered. A smaller 512 x 64, 2-bit indexed canvas takes 8 KiB. Both require a DDR scanout buffer. The user's subsequently supplied logo needs a taller canvas; see the 432 x 192 recommendation below.

The official local device pack `modules/hal/microchip/packs/sam/sama7d6/include/sama7d65/component/lcdc.h` confirms OVR1 CLUT modes 1/2/4/8 bits per pixel (2/4/16/256 palette indices). Palette entries have 8-bit R, G, B and alpha. Separate R/W `OVR1CLUT[256]` registers exist at offset 0xB00. Datasheet pages 1293 and 1406 confirm direct writes are allowed only when CLUTEN=0 and GAM=0: initialize the palette before enabling indexed mode. This avoids a palette DMA buffer. OVR1CLA is an alternative DMA palette address with 8-byte alignment. Indexed packing order still requires verification. The current reference driver selects RGB mode, not indexed mode.

Datasheet page 1294 Table 48.6 requires CLUT window size/discard attributes to be multiples of 8 pixels at 1 bpp, 4 at 2 bpp, and 2 at 4 bpp. The proposed 432 x 192 canvas satisfies these restrictions. Indexed framebuffer DMA addresses need only byte alignment per page 1292 Table 48.3; stronger alignment is useful for bus transfers and future cache maintenance.

## SRAM accounting before a target build

Internal general SRAM is 128 KiB at 0x00100000 through 0x0011FFFF. Bootstrap's stack starts at 0x00120000 and grows downward. The linker reports `_sramsize = _ebss - _stext`; the existing build check caps this code/data/BSS span at 65,536 bytes. Embedded image data in ordinary rodata or BSS counts against this limit. A framebuffer in SRAM cannot be used as LCDC scanout on this chip. The initial calculation below is retained as CPU storage arithmetic, not as a valid SRAM framebuffer design; measured target results are below.

A planning allocation of 64 KiB for code/data/BSS and 8 KiB for stack/guard leaves 56 KiB for separately reserved CPU-accessible storage. It requires corresponding linker checks; it is not an allocation made by the current linker script and cannot be used for LCDC scanout.

| Mode | Maximum 512-wide height under a 56 KiB framebuffer budget | Storage |
| --- | ---: | ---: |
| 1-bit indexed, 2 colors | 800 (panel height) | 51,200 bytes |
| 2-bit indexed, 4 colors | 448 | 57,344 bytes |
| 4-bit indexed, 16 colors | 224 | 57,344 bytes |
| 8-bit indexed, 256 colors | 112 | 57,344 bytes |
| RGB565 | 56 | 57,344 bytes |
| ARGB8888 | 28 | 57,344 bytes |

A full-screen 1-bit framebuffer takes 128,000 bytes and is unsuitable alongside Bootstrap in SRAM; it can instead reside in DDR. A default-color overlay has no framebuffer allocation and can cover the entire panel, with one RGB color at a time. For an embedded asset, available bytes under the present build limit are `65,536 - baseline _sramsize - additional display code/data/BSS`.

## SD image size and progress

`driver/sdcard.c` reads until EOF in 0x40000-byte (256 KiB) requests directly into DDR. No 256 KiB SRAM staging allocation exists. `CONFIG_IMG_SIZE` in `Config.in.zephyr` applies to flash loading, not this SD path. The destination and jump address default to 0x60000000. An exact 8 MiB image needs 32 full reads plus the existing EOF check. Lower-level SD transfers are split as necessary; their per-transfer limit does not cap the file at 8 MiB. The filesystem's file size is 32-bit; available DDR and image placement are the practical limits. Sources: `driver/sdcard.c`, `driver/common.c`, `driver/mci_media.c`, `fs/src/ff.c`, `fs/include/ff.h`.

For progress, obtain total bytes from `f_size()` after open, accumulate actual bytes read, advance the destination by actual bytes (the current loader advances by requested bytes), and use a 64-bit intermediate for percentage. Check each read result immediately, reject destination overflow or an image outside usable DDR, and require final copied bytes to match file size. Update only when the visible filled width changes. Keep large reads initially; there is no need for a small SRAM transfer buffer.

The current Zephyr application reinitializes the display and allocates its own graphics surfaces. Preserving the loading screen until the first application frame requires coordinated handoff. A DDR splash buffer must be reserved outside the image load range, application BSS, stacks and heaps. General SRAM can also be used by CAN, but the recommended scanout buffer is in DDR.

## Remaining validation

- Actual stack high-water measurement (an 8 KiB reserve below is a planning allowance).
- Panel power/backlight minimum timings and actual time to first visible pixel.
- LCDC/LVDSC default-color operation before DDR training. SRAM DMA is now ruled out by the interconnect table; buffered scanout requires DDR.
- Indexed bit/nibble packing; direct CLUT writes must occur with CLUTEN/GAM disabled.
- Linker-enforced framebuffer/stack separation and display handoff to Zephyr.

## Primary reference links

- Microchip SAMA7D6 datasheet: https://ww1.microchip.com/downloads/aemDocuments/documents/MPU32/ProductDocuments/DataSheets/SAMA7D6-Series-Data-Sheet-DS60001851.pdf . The initial run checked the local official device pack; the follow-up directly read local revision DS60001851E at `C:/Users/I41690/Downloads/SAMA7D6-Series-Data-Sheet-DS60001851.pdf` and rendered the relevant tables for visual verification.
- Official Harmony XLCDC initialization: https://raw.githubusercontent.com/Microchip-MPLAB-Harmony/gfx/master/middleware/legato/driver/controller/xlcdc/templates/plib/plib_xlcdc.c.ftl . Uses a 150 us PLL startup setting; this is not a measured power-on-to-display time.
- Official Harmony XLCDC header: https://raw.githubusercontent.com/Microchip-MPLAB-Harmony/gfx/master/middleware/legato/driver/controller/xlcdc/templates/plib/plib_xlcdc.h.ftl . Recommends 32-byte framebuffer alignment. Prefer larger alignment when reserving buffers alongside other DMA users.

## Follow-up: baseline build and supplied logo

Requested toolchain: `C:/Git/zephyr-mchp-workspace/.sdk/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi`.

Requested image: `C:/Users/I41690/Pictures/image_271.png`, with a thin rounded progress bar below it and the entire group centered. This section records the research phase; the later authorized implementation is below.

### Reproducible baseline build

The original `./build-zephyr.ps1` run generated `.config` and autoconf with the repository's Python Kconfig tools, selected the supplied `arm-zephyr-eabi-gcc` toolchain (GCC 14.3.0), invoked the existing Makefile with the installed MPLAB X GNU Make and Git Bash, and generated the ROM boot format with official SAM-BA 3.9.2. Firmware sources and defconfig were unchanged at that point. The script now builds the splash implementation into a separate directory. It checks the configured 64 KiB limit and validates both PTI SHA256 hashes and the raw payload. It restores its temporary PATH/VERSION environment variables. Existing Kconfig warnings concern prompt whitespace and an empty fast-boot help block.

Official SAM-BA was downloaded into ignored `build/tools/` from https://github.com/atmelcorp/sam-ba/releases/download/v3.9.2/sam-ba_v3.9.2-win_x86_64.zip . Archive SHA256 was verified as `7ec94c06a47dab827fa6deed92c01a3f17e8346328a5fbaca1b49b2571cf75bc`. No board was connected or flashed.

| Measurement | Bytes | Meaning |
| --- | ---: | --- |
| `_romsize` / raw binary | 28,769 | Code, constants and initialized data copied by ROM |
| BSS | 1,016 | Zero-initialized storage |
| `_sramsize` | 29,792 | Entire span from 0x00100000 to `_ebss=0x00107460`, including alignment gaps |
| Remaining under current 64 KiB build limit | 35,744 | Available for added display code and embedded asset data |
| Physical SRAM above `_ebss` | 101,280 | Includes the active downward-growing stack; not all free storage |
| Physical remainder after an assumed 8 KiB stack/guard reserve | 93,088 | Estimate only; stack high-water not measured |
| ROM-compatible PTI file | 28,864 | 48-byte header + 28,784-byte padded payload + 32-byte payload digest |

Artifacts are in `build/curiosity_zephyr_baseline/`:

- `binaries/boot.bin`: official SAM-BA-generated Non-Secure ROM boot file, version 0.0.
- `binaries/sama7d65-sdcardboot-zephyr-4.0.14-rc3.bin`: raw Bootstrap binary, not the ROM header-wrapped file.
- Matching `.elf` and `.map` files.
- `baseline.config`, `metrics.json` and `build.log`.

This is an AT91Bootstrap binary configured to load `zephyr.bin`, not a rebuilt Zephyr application. Datasheet pages 290 and 338-339 explain the `boot.bin` name, `BSP1` magic, 16-byte payload padding, version fields and hash format. Secure Boot uses a different format. The selected raw image begins at 0x00100000; Zephyr continues to load at 0x60000000. Eight-MiB and larger Zephyr images remain supported by the SD path independently of the Bootstrap SRAM budget.

### Supplied image and centered layout

The input is 426 x 149 RGBA with opaque pixels. The existing dark image background is retained; surrounding screen is black. Suggested OVR1 canvas: **432 x 192 at (424, 304)** on the 1280 x 800 panel. All canvas dimensions meet the 4-bpp CLUT restrictions, with a 216-byte row stride. Logo origin inside canvas is (3, 6), giving panel origin (427, 310). Progress track is **320 x 6 with 3-pixel radius**, at panel (480, 483), 24 pixels below the image. The group is visually centered. Antialias the rounded edges offline, then use the same palette for the track and changing fill. Hardware windowing alone makes rectangular ends, so a rounded track/fill needs raster pixels.

A research-only 16-color quantization using maximum coverage was rendered and inspected. It preserves the logo well at native size (RGB RMS error approximately 2.60 out of 255 for the complete canvas), including rounded bar edges. Median-cut quantization overallocated colors to the dark background and lost edge shades, so maximum coverage is preferred for this asset. Preview shows 60 percent only to demonstrate the fill; no progress implementation is linked into the binary.

| Complete 432 x 192 canvas | DDR framebuffer | Embedded PackBits/RLE asset | Palette | Quality/tradeoff |
| --- | ---: | ---: | ---: | --- |
| 2-bit indexed / 4 colors | 20,736 bytes | 5,855 bytes | 16 bytes | Noticeable loss of edge/background shades |
| **4-bit indexed / 16 colors** | **41,472 bytes (40.5 KiB)** | **9,187 bytes** | **64 bytes** | Recommended: native logo and smooth thin bar |
| 8-bit indexed / 256 colors | 82,944 bytes | 43,648 bytes | 1,024 bytes | Better color accuracy, but simple RLE asset exceeds current headroom |
| RGB565 | 165,888 bytes | Not evaluated | None | Large raw embedded asset; no need for this splash |

The RLE estimate encodes an entire demo canvas, including one bar state, and was round-trip checked against the packed pixels. Palette storage is counted separately. A final implementation can store only the logo and render the small bar directly, avoiding a startup PNG decoder or graphics library. The research packed pixel order is explicitly provisional, not validated against hardware.

With the recommended RLE canvas and palette, `29,792 + 9,187 + 64 = 39,043` bytes, leaving **26,493 bytes under the 64 KiB span limit** for a small decoder, display register setup and progress accounting. This is an estimate until that code is linked. An uncompressed full 4-bit canvas would instead consume `29,792 + 41,472 = 71,264` bytes and exceed the existing limit by 5,728 bytes. Even an uncompressed native logo padded to 426 x 150 uses 31,950 bytes and leaves only 3,794 bytes for code/palette, so it is a poor margin.

Use a tiny streaming RLE decoder directly into the DDR framebuffer after DDR training: no full SRAM decode buffer, no double buffering, no PNG library, and no logo reads from SD. Then enable OVR1 DMA while BASE DMA remains off. At 60 Hz, a 4-bit 432 x 192 overlay fetches approximately 2.49 MB/s of pixel payload, far below a full-screen framebuffer. Update only the small progress pixels from actual successful SD-copy bytes and throttle any synchronization waits.

### Timing conclusion after the datasheet correction

**Display output can start before DDR training using constant colors; the full logo can first appear immediately after DDR becomes usable, before SD initialization and the large Zephyr copy.** The SRAM-only logo approach from the initial research is not supported by this chip's LCDC interconnect. Move panel power/PLL/timing startup earlier, then decode the approximately 9 KiB embedded logo into a 40.5 KiB reserved DDR window. This avoids waiting for Zephyr or loading an extra splash file from SD. Reserve that DDR window outside every loaded image and application memory range, and coordinate Zephyr display initialization if flicker-free handoff is required.

Research previews and sizing data: `build/curiosity_zephyr_baseline/research/splash-centered-clut4.png`, `splash-clut4-preview.png`, `splash-feasibility.json` and `analyze_splash.py`. These files describe the original feasibility layout. The current firmware implements the cropped layout below.

## Authorized first-pass implementation and board-test binary

The user subsequently requested implementation, a BASE default color of **0x0A0C18**, a one-pixel crop margin around the white artwork, and a binary to test on the non-Pro board. `CONFIG_SAMA7D65_CURIOSITY_SPLASH=y` is enabled only in the requested non-Pro SD1 Zephyr defconfig. The driver is intentionally specific to the 1280x800 LVDS panel configured by zero_gfx; other panels require their own timing/power configuration. Secure, backup and fast-boot configurations cannot select this first pass.

### Final display layout and memory placement

The source crop rectangle is **[28,14,394,135)**: a **366x121** logo, including one source pixel outside artwork detected above background. Dark gradient pixels are flattened to #0A0C18 and antialiased white artwork uses a 16-entry background-to-white palette. The group has a **366x152** OVR1 canvas at **(457,324)**; the 320x6 rounded bar starts at **(480,469)**, 24 pixels below the cropped logo. One extra bottom padding row meets the CLUT requirement that both dimensions be even. The entire group is centered to within the unavoidable half-pixel rounding. BASE covers the full panel with the constant color and **BASE DMA stays disabled**. HEO is disabled. Only OVR1 fetches image pixels.

The live framebuffer is **27,816 bytes**, single buffered, 4-bit indexed / 16 colors, with a 183-byte contiguous row stride. The last **64 KiB of the configured 1 GiB DDR**, **0x9FFF0000..0x9FFFFFFF**, is reserved by the loader; the actual framebuffer starts at 0x9FFF0000. No framebuffer or SD chunk staging allocation is placed in SRAM. The embedded PackBits stream is **7,383 bytes**, the palette 64 bytes, and capsule coverage table 18 bytes. The decoder writes directly to DDR. At 60 Hz, pixel payload fetches are about 1.67 MB/s. The progress renderer touches only the 320x6 bar and does not wait for vsync on each update.

The current local zero_gfx map ends allocated image RAM at **0x62E9C000**, well below the splash reservation; its named system/GPU heaps are also below that end. Its declared DDR region extends to 0xA0000000. The application has not been changed to formally reserve the splash region: any future allocator, memory clear/test, or application using the top of DDR must keep this range untouched until LCDC takeover. The current Zephyr driver reinitializes LVDS/LCDC and enables its own layers, so a flicker-free handoff is not established by this first pass.

For reference, maximum framebuffer-only windows inside the 64 KiB reservation are below. Embedded artwork complexity and boot code still determine the SRAM cost; these sizes are not promises that every uncompressed asset fits in the bootstrap binary.

| Color mode | Maximum height at 1280-pixel width | Bytes | Notes |
| --- | ---: | ---: | --- |
| 1-bit indexed / 2 colors | 408 | 65,280 | Height and width multiples of 8 |
| 2-bit indexed / 4 colors | 204 | 65,280 | Multiples of 4 |
| 4-bit indexed / 16 colors | 102 | 65,280 | Multiples of 2 |
| 8-bit indexed / 256 colors | 51 | 65,280 | More palette/asset storage |
| RGB565 | 25 | 64,000 | No CLUT |
| ARGB8888 | 12 | 61,440 | No CLUT |

At the selected **366-pixel width**, 4-bit indexed can reach **358 pixels high (65,514 bytes)** inside that reservation. There is **no legal SRAM DMA framebuffer size** for this LCDC. DDR itself can accommodate the full **1280x800 ARGB8888 panel (4,096,000 bytes)**, but this first pass deliberately restricts the buffer to 64 KiB and uses the smaller logo window for early startup and low bandwidth.

### Startup ordering and progress behavior

Panel power PB18 is asserted after timer initialization, with backlight PB14 held low. After DDRPLL/MCK3 are running, LVDSPLL and the timing engine start with BASE default color before DDR training. The PLL uses the reference 24 MHz crystal, MUL field 41, FRACR 986500, DIVPMC field 1, and the reference startup setting 6. Background output/backlight starts after CLKEN, SYNCEN, DISPEN and SDEN status checks. The logo is decoded and OVR1 DMA enabled immediately after successful DDR initialization, before SD initialization, PMIC/post-init work and Zephyr copying. There are no deliberate animation delays or an additional splash file read from SD.

Datasheet **Table 48.10, page 1297**, confirms **p0 in the low nibble, p1 in the high nibble**; the earlier provisional research packing is superseded. Palette registers are written with CLUTEN=0 and GAM=0 before enabling 4-bit mode. CPU caches are disabled in this target configuration; memory writes are ordered with DSB before enabling/updating scanout. Every new hardware wait is bounded to 100 ms and a display failure logs the cause and lets normal boot continue. These waits normally return as soon as status is ready; 100 ms is a failure timeout, not a startup delay.

SD reads remain **256 KiB per request**, directly into DDR. Total size comes from `f_size`, every successful read must return its requested byte count, and progress uses actual completed bytes. A binary search with 64-bit products computes bar width without overflow or a 64-bit division runtime dependency. The final pixel fills only after close and unmount succeed. Read, short-read, close and unmount errors return failure, leave the last copied fraction visible, print a serial message, and follow the existing failed-load halt behavior. The bar reports copying, not Zephyr's subsequent application startup.

**8 MiB zephyr.bin is supported.** It takes 32 full chunk reads; the improved loader does not need an extra EOF read. The local `build/zero_gfx/zephyr/zephyr.bin` is actually **10,908,520 bytes (~10.40 MiB)** and is also below the limit. With destination 0x60000000 and the final DDR reservation, the maximum accepted raw file size is **0x3FFF0000 = 1,073,676,288 bytes (1023.9375 MiB)**. That is a loader range limit, not a guarantee that a Zephyr application of that size has room for its own BSS/stacks/heaps. The file must be linked to execute at the existing 0x60000000 entry point. No application rebuild or binary modification was performed.

### Measured first-pass build

| Measurement | Bytes |
| --- | ---: |
| Raw bootstrap / `_romsize` | 38,573 |
| Code/data/BSS SRAM span / `_sramsize` | 39,604 |
| Increase versus baseline SRAM span | 9,812 |
| Remaining under configured 64 KiB span limit | 25,932 |
| Physical SRAM above `_ebss=0x00109AB4` | 91,468 |
| Remainder after an assumed 8 KiB stack reserve | 83,276 |
| ROM PTI `boot.bin` | 38,656 |

Compiler-generated stack reports from the first compile show `boot_splash_show` 40 bytes, `boot_splash_progress` 24 bytes, `draw_bar` 48 bytes, and `load_sdcard` 1128 bytes of static stack per function. These are individual frames, not whole-call-chain high-water measurements; the 8 KiB reserve remains an estimate.

### Artifacts, checks and board test

- **Board-test image:** `build/curiosity_zephyr_splash/binaries/boot.bin`. This is the official SAM-BA-generated Non-Secure ROM PTI image. Use this wrapped file as boot.bin, not the neighboring raw bootstrap .bin.
- Final boot.bin SHA256: `B5B7BFA6843A7F00DB5EDD7DE65D8E7DFD31F6D33F494D15CEAF9279F815275B` (also recorded by the build in metrics.json).
- `binaries/sama7d65-sdcardboot-zephyr-4.0.14-rc3.elf` and `.map`, `build.config`, `build.log`, `metrics.json`.
- Visual previews: `splash-0.png`, `splash-60.png`, `splash-100.png`, `logo-cropped.png`, and `splash-layout.json`.
- ARM execution checks: `verification.json`.
- Rebuild: `./build-zephyr.ps1` with the requested compiler. The script validates payload bytes, PTI layout, both SHA256 hashes, and the 64 KiB SRAM span limit. Output directory defaults to `build/curiosity_zephyr_splash` and the original baseline is retained.
- Asset regeneration: `python scripts/make_splash.py C:/Users/I41690/Pictures/image_271.png` (Pillow/numpy); the generated header is already present, so normal firmware builds do not require that source PNG or Python image packages.
- ARM verification: `python scripts/verify_splash.py build/curiosity_zephyr_splash/binaries/sama7d65-sdcardboot-zephyr-4.0.14-rc3.elf` (Unicorn, pyelftools, Pillow, numpy).

The compiled ARM splash decoder and progress renderer match the preview pixels exactly. Mocked-MMIO checks verify BASE DMA off, CLUT enable/palette ordering, centered window registers, and a returning PLL timeout. Sixteen mocked FatFs cases verify 8 MiB, an uneven final chunk, the actual local 10,908,520-byte image, 64 MiB, the exact reserved-DDR boundary, over-limit/overflow/empty/invalid-destination cases, and mount/open/read/short-read/close/unmount failures. This validates the linked instructions and copy control flow. The user subsequently tested the delivered binary on the physical board and reported "works great!"; no detailed log, exact tested application size, or timing measurements were supplied. Existing unrelated Kconfig warnings remain; the firmware compile has no new compiler warnings.

For testing, place **boot.bin and your existing zephyr.bin in the FAT SD card root**, use the requested non-Pro board's SD1 boot slot/boot selection, and capture the usual serial console. Expect `Splash: background on, BASE DMA off`, then `Splash: logo on, framebuffer 9fff0000 (27816 bytes)`, then the SD byte count and final load message. A clock/timing failure reports a display timeout and still attempts normal boot. No board was flashed or connected by this task.

The user has now confirmed a successful first-pass physical board test. Time to background, time to logo, minimum panel power-to-backlight timing, and detailed Zephyr takeover/flicker behavior remain unmeasured. The supplied binary starts each display stage at the earliest verified dependency point, but no absolute millisecond claim is made until measured on hardware. The latest request was to save session state without committing; only documentation was changed during that handoff turn.
