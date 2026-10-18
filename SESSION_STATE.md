# Session handoff: early SAMA7D65 Curiosity splash

Last updated: 2026-10-19. Workspace: `C:/Git/at91bootstrap`, Windows PowerShell.

## Current result and user direction

The first-pass early LVDS splash and SD-copy progress implementation is built. The user tested the delivered binary on their board and replied **"works great!"**. Treat this as a successful first-pass board test. No serial log, measured startup times, exact tested application file size, or detailed display-handoff observations were supplied.

On 2026-10-19, the user reported a brief backlight dip when boot.bin starts and explained that USB power already keeps the backlight on. They requested removal of all splash backlight control. The three PB14 calls (startup low, success high, failure low) and unused LCDCFG6 PWM setup have been removed. **Bootstrap now leaves PB14 untouched**, including display failures. PB18 panel power and display/SD-copy behavior remain in place. The updated binary is built and verified in simulation; the user's earlier "works great!" applies to the prior binary, and the new backlight revision awaits board confirmation.

The broader goal remains **get the display up as fast as possible**. No timing instrumentation was performed. The user subsequently requested committing the backlight revision in the same style as the prior splash commit, without a co-author trailer; this handoff is included in that commit. Measuring power-on to first background and first logo remains a possible next step when requested.

Read this file first, then the **Authorized first-pass implementation and board-test binary** section of [findings.md](findings.md). Earlier sections in findings.md preserve research estimates and superseded layouts; use the final implementation section for current dimensions and memory use.

## Target and design to preserve

- **SAMA7D65 Curiosity NON-Pro**, `configs/sama7d65_curiositysd1_zephyr_defconfig`, SDHC1, Zephyr image load/jump address **0x60000000**. Do not substitute the Curiosity Pro configuration.
- Reference application: `C:/Git/zephyr-mchp-workspace/app/zero_gfx`. Its Zephyr sources/binary were not changed or rebuilt by this task.
- 1280x800 LVDS panel, 72,403,200 Hz pixel clock; horizontal sync/front/back 32/48/80, vertical 6/16/16; VESA mapping, unbalanced, 24-bit output. Panel power PB18. Backlight PB14 is left in its existing USB-power state; do not reintroduce GPIO/PWM backlight control into the splash.
- Full-screen BASE default color **0x0A0C18**, **BASE DMA disabled**; HEO disabled. Only OVR1 scans out a compact framebuffer.
- Source logo: `C:/Users/I41690/Pictures/image_271.png` (426x149). Crop box **[28,14,394,135)** yields **366x121**, with one source pixel outside the white artwork. Dark background is flattened to #0A0C18 while preserving white edge coverage.
- Centered OVR1 canvas **366x152 at (457,324)**; rounded progress bar **320x6 at (480,469)**, 24 pixels below the logo, 3-pixel radius. One bottom padding row meets the even-dimension requirement.
- **4-bit indexed / 16 colors**, p0 in low nibble, p1 in high nibble; contiguous row stride 183 bytes. Live framebuffer **27,816 bytes**, compressed asset **7,383 bytes**, palette 64 bytes, capsule coverage table 18 bytes.
- **LCDC DMA cannot access general SRAM on this SoC.** The framebuffer resides in DDR at **0x9FFF0000**, within a 64 KiB loader reservation ending at **0x9FFFFFFF**, the top of configured 1 GiB DDR. Do not move scanout into SRAM.
- Panel power starts after timer initialization. Background/LVDS startup runs after DDRPLL/MCK3, before DDR training. Logo decoding/OVR1 enable occurs immediately after successful DDR initialization, before SD initialization and application copy.
- New hardware polls have 100 ms failure timeouts; they exit immediately when ready. Display failures log and allow normal boot to continue. No deliberate animation delays, PNG decoder, splash-file SD read, SRAM chunk staging buffer, or double buffering.

## Large SD files and remaining limits

The splash-enabled SD path reads 256 KiB chunks directly into the destination DDR, gets total size with `f_size`, advances by actual successful bytes, rejects empty/overflow/out-of-range files, and fails on short reads and read/close/unmount errors. Progress uses 64-bit products with binary search, avoiding overflow and a 64-bit division helper. The final bar pixel fills only after close and unmount succeed. Failure retains the last copied fraction and follows the existing failed-load halt.

An **8 MiB** image is supported (32 full reads). The inspected local `C:/Git/zephyr-mchp-workspace/build/zero_gfx/zephyr/zephyr.bin` was **10,908,520 bytes (~10.40 MiB)** and also fits. The loader's maximum accepted file at 0x60000000 is **0x3FFF0000 = 1,073,676,288 bytes**, bounded by the top-DDR splash reservation; application BSS, stacks and heaps impose additional practical limits. No claim is made that the user tested this maximum or a specific file size.

The local zero_gfx map had `_image_ram_end` **0x62E9C000**, below the splash buffer. Zephyr has not been modified to formally reserve the top 64 KiB. Future allocations, memory clears/tests, or application changes must preserve the buffer until display takeover. Zephyr currently reinitializes LVDS/LCDC and its own layers; seamless takeover remains unimplemented. User acceptance confirms the first pass works, but no explicit flicker measurement was supplied.

## Delivered artifacts and measurements

Current rebuilt ROM image: **`build/curiosity_zephyr_splash/binaries/boot.bin`** (2026-10-19 backlight revision, awaiting board confirmation). This is the SAM-BA-generated **Non-Secure ROM PTI** file, not the adjacent raw .bin and not a rebuilt Zephyr application.

SHA256 of the current backlight revision:

```text
76AC25F58FCE0FB2094987CBE51C24F8A795E6EE615CE32E67A3C3992DAB079C
```

| Measurement | Bytes |
| --- | ---: |
| Raw bootstrap / `_romsize` | 38,457 |
| Code/data/BSS SRAM span / `_sramsize` | 39,492 |
| Remaining below configured 64 KiB span limit | 26,044 |
| Physical SRAM above `_ebss=0x00109A44` | 91,580 |
| Remainder after assumed 8 KiB stack reserve | 83,388 |
| Wrapped `boot.bin` | 38,544 |

The earlier board-tested binary is preserved at **`build/curiosity_zephyr_splash/archive/2026-10-16-board-tested/boot.bin`**, with SHA256 `B5B7BFA6843A7F00DB5EDD7DE65D8E7DFD31F6D33F494D15CEAF9279F815275B` and its original metrics.json. That version deliberately toggles PB14 and has the reported startup dip; keep it as a reference, not the current fix.

The 8 KiB stack reserve is an estimate, not a measured high-water mark. Compiler stack reports from the initial compile give individual frames: show 40 bytes, progress 24 bytes, draw_bar 48 bytes, load_sdcard 1128 bytes; call chains add to these.

Other splash artifacts: matching `binaries/sama7d65-sdcardboot-zephyr-4.0.14-rc3.elf` and `.map`, `build.config`, `build.log`, `metrics.json`, `verification.json`, `splash-layout.json`, `logo-cropped.png`, `splash-0.png`, `splash-60.png`, `splash-100.png`. The unchanged baseline is retained in **`build/curiosity_zephyr_baseline/`**, with SRAM span 29,792 bytes. Build folders are ignored by Git; preserve the working binary locally if rebuilding.

For board use, boot.bin and an existing correctly linked zephyr.bin go in the FAT SD-card root; use the non-Pro SD1 boot slot/selection. Expected console messages include:

```text
Splash: background on, BASE DMA off
Splash: logo on, framebuffer 9fff0000 (27816 bytes)
SD/MMC: copying <file bytes> bytes (limit 9fff0000)
SD/MMC: Done to load image
```

## Source and working-tree state

At the 2026-10-19 revisit, the workspace was already clean on branch **a7d65_curiosity_boot_splash**, HEAD **ef6f64c23c4959733f170b274a0ed7c899b6928c**. The prior implementation/documentation is tracked there. This session did not create that commit or branch. The backlight revision modifies only splash.c, verify_splash.py and the two documentation files and is committed at the user's subsequent request under **"Preserve SAMA7D65 backlight during splash startup"**. Use git log for its hash.

| File | Responsibility |
| --- | --- |
| `Config.in.zephyr` | New `SAMA7D65_CURIOSITY_SPLASH` option; requires SAMA7D65 + SDCARD, excludes fast boot, backup, secure |
| `configs/sama7d65_curiositysd1_zephyr_defconfig` | Enables the splash for requested non-Pro target |
| `device/sama7d65/sama7d65.c` | Early panel/background and post-DDR logo hooks |
| `driver/driver.mk` | Conditionally builds splash object |
| `driver/sdcard.c` | Bounded large-file loader, progress/error hooks for splash configuration |
| `device/sama7d65/splash.c` (new) | Minimal LCDC/LVDS setup, DDR decode, capsule renderer, progress |
| `device/sama7d65/splash_logo.h` (new) | Generated compressed pixels/palette/caps; normal build needs no original PNG |
| `include/boot_splash.h` (new) | Splash API and disabled-feature stubs |
| `build-zephyr.ps1` (new) | Compiler wiring, target build, vendor PTI wrapping and accounting/hash checks |
| `scripts/make_splash.py` (new) | Offline crop/flatten/pack/preview generation |
| `scripts/verify_splash.py` (new) | Runs linked ARM code with mocked FatFs/MMIO in Unicorn |
| `findings.md`, `SESSION_STATE.md` (new) | Research, implementation record and handoff |

The table above records the implementation's files; entries marked "new" were additions in the original session and are now tracked. Generated `.config` and `config/at91bootstrap-config/` are ignored. The backlight revision is committed as requested, without a co-author trailer. No flashing action was performed by the assistant. The physical board test was performed by the user.

## Rebuild and verification

From `C:/Git/at91bootstrap`, use **PowerShell 7** and:

```powershell
./build-zephyr.ps1
python scripts/verify_splash.py build/curiosity_zephyr_splash/binaries/sama7d65-sdcardboot-zephyr-4.0.14-rc3.elf
```

The build script defaults to these existing local tools:

- Compiler: `C:/Git/zephyr-mchp-workspace/.sdk/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc.exe`, GCC 14.3.0.
- GNU Make: `C:/Program Files/Microchip/MPLABX/v6.30/gnuBins/GnuWin32/bin/make.exe`.
- Git Bash: `C:/Program Files/Git/bin/bash.exe`, with Git usr/bin on temporary PATH.
- Official SAM-BA 3.9.2: `build/tools/sam-ba-3.9.2/sam-ba_v3.9.2/sam-ba.exe` (ignored, already downloaded).

Normal build requires Python and the repo Kconfig tools. ARM verification requires **unicorn, pyelftools, Pillow, numpy**, installed in the current Python environment. Optional image regeneration:

```powershell
python scripts/make_splash.py C:/Users/I41690/Pictures/image_271.png
```

The build validates the 64 KiB SRAM span, SAM-BA PTI layout, raw payload, and both SHA256 digests without contacting a board. Binary hashes can change on rebuild because the build embeds date/time. Do not claim a rebuilt artifact is byte-identical to the user-tested file without checking.

The current compiled ARM instructions passed pixel-for-pixel preview comparison, BASE DMA off, palette-before-CLUT-enable checks, centered registers, wide progress inputs, and bounded PLL/LVDS/LCDC timeouts. Verification asserts that the only splash GPIO action is PB18 high and rejects any PB14 GPIO call; it also checks that LCDCFG6 PWM configuration is untouched. **16 mocked SD cases** passed: 8 MiB, partial final chunk, actual local 10.40 MiB length, 64 MiB, exact DDR limit, rejection boundaries, and mount/open/read/short-read/close/unmount errors. These mock checks do not model real panel timing or SD-controller electrical behavior. The earlier user board success adds first-pass evidence; this backlight revision still needs a board test.

Known build details: two existing unrelated Kconfig warnings (prompt whitespace and empty help) remain; no new compiler warnings. Do not pass `-fstack-usage` through `EXTRA_CC_ARGS`: this Makefile also forwards it to raw ld, causing `-f may not be used without -shared`. Existing .su files came from the initial compile; current build script does not inject that option. The script must use the real versioned output name; overriding AT91BOOTSTRAP with a generic at91bootstrap.bin previously caused a self-referential symlink issue.

## Useful next-session work, when requested

Measure power-on to background and logo on the board before choosing further startup optimizations. Capture serial timestamps or external GPIO/video timing; quantify DDR and pre-logo work. Confirm handoff behavior and formally reserve framebuffer DDR in Zephyr if needed. Measure stack high-water if tightening SRAM budgets. These are future options, not work started by this handoff request.

Hardware sources and detailed arithmetic are in findings.md. The local official datasheet is `C:/Users/I41690/Downloads/SAMA7D6-Series-Data-Sheet-DS60001851.pdf`, revision DS60001851E; the reference device headers are under `C:/Git/zephyr-mchp-workspace/modules/hal/microchip/packs/sam/sama7d6/include/sama7d65/`. Critical references: LCDC DDR-only interconnect tables pp64-66, indexed pixel table 48.10 p1297, direct CLUT writes with CLUTEN/GAM off.

If external research is needed later, preserve the user's AGENTS instructions: multipage/cross-checking through web_researcher; quick lookup through searxng; other web tools only as fallback.
