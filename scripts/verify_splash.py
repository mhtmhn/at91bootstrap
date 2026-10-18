#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the linked ARM firmware using mocked MMIO and FatFs in Unicorn.

Requires unicorn, pyelftools, Pillow and numpy. No hardware timing is emulated.
"""
import argparse
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

import make_splash

LCD = 0xe1400000
PMC = 0xe0018000
FRAME = 0x9fff0000
STOP = 0x110000
REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)


class Firmware:
    def __init__(self, path, size=8 * 1024 * 1024, failure=None):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.mem_map(0x100000, 0x20000)
        self.uc.mem_map(0x61000000, 0x1000)
        self.uc.mem_map(FRAME, 0x10000)
        self.uc.mem_map(LCD, 0x10000)
        self.uc.mem_map(PMC, 0x1000)
        self.symbols = {}
        with path.open('rb') as stream:
            elf = ELFFile(stream)
            for section in elf.iter_sections():
                address = section['sh_addr']
                if section['sh_flags'] & 2 and section['sh_type'] != 'SHT_NOBITS' and section['sh_size']:
                    self.uc.mem_write(address, section.data())
            for symbol in elf.get_section_by_name('.symtab').iter_symbols():
                self.symbols[symbol.name] = symbol['st_value']
        self.size = size
        self.failure = failure
        self.reads = []
        self.progress = []
        self.copied = self.mounts = self.closes = self.delays = 0
        self.mmio = []
        self.pins = []
        self.uc.mem_write(PMC + 0xec, struct.pack('<I', 0 if failure == 'pll' else 128))
        if failure == 'lvds':
            self.uc.mem_write(LCD + 0x800c, struct.pack('<I', 1))
        stubs = {'dbg_printf': self.noop, 'udelay': self.delay,
                 'pmc_enable_periph_clock': self.noop,
                 'pio_set_gpio_output': self.pin, 'pio_set_value': self.pin,
                 'at91_sdhc_hw_init': self.noop, 'f_mount': self.mount,
                 'f_open': self.open, 'f_read': self.read, 'f_close': self.close}
        # Unused GPIO helpers may disappear under linker garbage collection.
        self.stubs = {self.symbols[name]: callback for name, callback in stubs.items()
                      if name in self.symbols}
        self.uc.hook_add(UC_HOOK_CODE, self.code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=LCD, end=LCD + 0xffff)

    def word(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def setword(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value))

    def code(self, uc, address, size, _):
        if address == STOP:
            uc.emu_stop()
        elif address in self.stubs:
            args = [uc.reg_read(reg) for reg in REGS]
            result = self.stubs[address](*args)
            uc.reg_write(UC_ARM_REG_R0, result or 0)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        elif address == self.symbols['boot_splash_progress']:
            self.progress.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))

    def write(self, uc, access, address, size, value, _):
        self.mmio.append((address - LCD, value))
        if address == LCD + 0x20 and self.failure != 'lcdc':
            status = self.word(LCD + 0x28)
            self.setword(LCD + 0x28, status | (value & 7))

    def noop(self, *args):
        return 0

    def delay(self, *args):
        self.delays += 1

    def pin(self, pin, value, *args):
        assert pin != 46, 'Firmware must leave USB-powered backlight PB14 untouched'
        self.pins.append((pin, value))

    def mount(self, *args):
        self.mounts += 1
        return 1 if self.failure == 'mount' or (self.failure == 'unmount' and self.mounts > 1) else 0

    def open(self, file, *args):
        self.setword(file + 8, 0)
        self.setword(file + 12, self.size)
        return 1 if self.failure == 'open' else 0

    def read(self, file, destination, requested, actual_pointer):
        assert destination == self.destination + self.copied
        assert requested == min(0x40000, self.size - self.copied)
        self.reads.append(requested)
        actual = requested - 1 if self.failure == 'short' else requested
        self.setword(actual_pointer, actual)
        self.copied += actual
        return 1 if self.failure == 'read' else 0

    def close(self, *args):
        self.closes += 1
        return 1 if self.failure == 'close' else 0

    def call(self, name, *args):
        for reg, value in zip(REGS, args):
            self.uc.reg_write(reg, value)
        self.uc.reg_write(UC_ARM_REG_SP, 0x120000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP)
        self.uc.emu_start(self.symbols[name], STOP + 4, count=100_000_000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP, f'{name} did not return'
        return self.uc.reg_read(UC_ARM_REG_R0)

    def load(self, destination=0x60000000):
        self.destination = destination
        self.uc.mem_write(0x61000000, struct.pack('<III', 0x61000020, destination, destination))
        self.uc.mem_write(0x61000020, b'zephyr.bin\0')
        return self.call('load_sdcard', 0x61000000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    output = args.elf.parent.parent
    layout = json.loads((output / 'splash-layout.json').read_text())
    firmware = Firmware(args.elf)
    firmware.call('boot_splash_power')
    firmware.call('boot_splash_early_init')
    assert firmware.word(firmware.symbols['display_ready']) == 1
    assert firmware.word(LCD + 0x88) == 0x0a0c18
    assert firmware.word(LCD + 0x8c) & 1 == 0
    assert firmware.pins == [(50, 1)]  # Only PB18 panel power; no PB14 control.
    assert firmware.call('boot_splash_load_limit') == FRAME
    firmware.call('boot_splash_show')
    assert firmware.word(firmware.symbols['framebuffer']) == FRAME
    assert firmware.word(LCD + 0x178) == FRAME
    assert firmware.word(LCD + 0x180) == 0x201
    assert firmware.word(LCD + 0x1a0) == 0x203
    assert firmware.word(LCD + 0x184) == 457 | (324 << 16)
    # Palette writes precede CLUTEN. Confirm actual decoded ARM pixels against preview.
    palette_start = next(i for i, (offset, value) in enumerate(firmware.mmio) if offset == 0xb00)
    clut_enable = next(i for i, (offset, value) in enumerate(firmware.mmio) if offset == 0x180 and value == 0x201)
    assert palette_start < clut_enable
    assert not any(offset == 0x18 for offset, value in firmware.mmio)  # No LCD PWM setup.
    firmware.call('boot_splash_progress', 3, 5)
    assert firmware.word(firmware.symbols['last_width']) == 192
    w, h = layout['overlay_size']
    packed = np.frombuffer(firmware.uc.mem_read(FRAME, layout['framebuffer_bytes']), dtype='uint8').reshape(h, w // 2)
    indices = np.empty((h, w), dtype='uint8')
    indices[:, 0::2], indices[:, 1::2] = packed & 15, packed >> 4
    palette = np.array([[round(b + (255 - b) * i / 15) for b in make_splash.BACKGROUND] for i in range(16)], dtype='uint8')
    expected = np.asarray(Image.open(output / 'splash-60.png'))
    x, y = layout['overlay_origin']
    assert np.array_equal(palette[indices], expected[y:y + h, x:x + w])
    # Byte totals requiring a wide multiplication, including maximum 32-bit inputs.
    for done, total in [(0, 8388608), (262144, 8388608), (4194304, 8388608),
                        (8388608, 8388608), (1000000000, 1073676288),
                        (0xffffffff - 1, 0xffffffff), (0xffffffff, 0xffffffff)]:
        firmware.call('boot_splash_progress', done, total)
        assert firmware.word(firmware.symbols['last_width']) == min(320, done * 320 // total)
    cases = []
    for size, failure, destination, success in [
        (8388608, None, 0x60000000, True),
        (8388608 + 123, None, 0x60000000, True),
        (10908520, None, 0x60000000, True),
        (64 * 1024 * 1024 + 17, None, 0x60000000, True),
        (FRAME - 0x60000000, None, 0x60000000, True),
        (FRAME - 0x60000000 + 1, None, 0x60000000, False),
        (0, None, 0x60000000, False),
        (8388608, None, 0x5ffffff0, False),
        (8388608, None, FRAME, False),
        (0xffffffff, None, 0x60000000, False),
        *[(8388608, failure, 0x60000000, False) for failure in
          ('mount', 'open', 'read', 'short', 'close', 'unmount')],
    ]:
        tested = Firmware(args.elf, size, failure)
        result = tested.load(destination)
        assert (result == 0) == success, (size, failure, hex(destination), result)
        if success:
            assert tested.copied == size
            assert tested.progress[-1] == (1, 1)
            assert tested.closes == 1 and tested.mounts == 2
        else:
            assert (1, 1) not in tested.progress
        cases.append(dict(bytes=size, failure=failure, destination=hex(destination), reads=len(tested.reads), expected_success=success))
    for failure in ('pll', 'lvds', 'lcdc'):
        timeout = Firmware(args.elf, failure=failure)
        timeout.call('boot_splash_power')
        timeout.call('boot_splash_early_init')
        assert timeout.delays == 10000 and timeout.word(timeout.symbols['display_ready']) == 0
        assert timeout.pins == [(50, 1)]
    result = dict(arm_decode_matches_preview=True, base_dma_disabled=True,
                  palette_written_before_clut_enable=True, progress_large_totals_pass=True,
                  pb14_untouched=True, lcd_pwm_configuration_untouched=True,
                  pll_timeout_returns=True, lvds_timeout_returns=True,
                  lcdc_timeout_returns=True, sd_cases=cases)
    (output / 'verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS: ARM splash decode/render, wide progress, PB14 untouched, three display timeouts, {len(cases)} SD load cases')


if __name__ == '__main__':
    main()
