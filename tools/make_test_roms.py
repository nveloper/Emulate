"""Generates tiny public-domain test ROMs for smoke-testing the emulator without commercial games.

    python tools/make_test_roms.py out_dir

emutest.nds: same idea on the DS top screen (VRAM display mode), with a banner icon and title.
emutest.gba: fills the screen with blue when idle and a grey derived from the held buttons
(A = dark grey, B = mid grey, SELECT = light grey ...),
so video output and input can be verified at a glance.
"""
import struct
import sys
from pathlib import Path


def mov_imm(rd, imm8, rot=0):
    return 0xE3A00000 | rd << 12 | rot << 8 | imm8


def add_imm(rd, rn, imm8, rot=0):
    return 0xE2800000 | rn << 16 | rd << 12 | rot << 8 | imm8


def orr_imm(rd, rn, imm8, rot=0):
    return 0xE3800000 | rn << 16 | rd << 12 | rot << 8 | imm8


def orr_reg(rd, rn, rm, shift_type=0, shift=0):
    return 0xE1800000 | rn << 16 | rd << 12 | shift << 7 | shift_type << 5 | rm


def mov_reg(rd, rm, shift_type=0, shift=0):
    return 0xE1A00000 | rd << 12 | shift << 7 | shift_type << 5 | rm


def mul(rd, rm, rs):
    return 0xE0000090 | rd << 16 | rs << 8 | rm


def cmp_imm(rn, imm8):
    return 0xE3500000 | rn << 16 | imm8


def mvn_reg(rd, rm):
    return 0xE1E00000 | rd << 12 | rm


def subs_imm(rd, rn, imm8):
    return 0xE2500000 | rn << 16 | rd << 12 | imm8


def strh(rd, rn, off=0):
    return 0xE1C000B0 | rn << 16 | rd << 12 | (off >> 4) << 8 | (off & 0xF)


def ldrh(rd, rn, off=0):
    return 0xE1D000B0 | rn << 16 | rd << 12 | (off >> 4) << 8 | (off & 0xF)


def str32(rd, rn):
    return 0xE5800000 | rn << 16 | rd << 12


def strb(rd, rn):
    return 0xE5C00000 | rn << 16 | rd << 12


def str_post4(rd, rn):
    return 0xE4800004 | rn << 16 | rd << 12


def branch(cond, here, target):
    return cond << 28 | 0x0A000000 | (((target - (here + 8)) >> 2) & 0xFFFFFF)


AL, NE = 0xE, 0x1
LSL, LSR = 0, 1


def gba_rom() -> bytes:
    base = 0x08000000
    start = base + 0xC0
    code = []

    def emit(word):
        code.append(word)

    def here():
        return start + 4 * len(code)

    emit(mov_imm(0, 0x04, 4))           # r0 = 0x04000000 (IO)
    emit(mov_imm(1, 0x04, 12))          # r1 = 0x0400
    emit(orr_imm(1, 1, 0x03))           # r1 = 0x0403: mode 3, BG2 on
    emit(strh(1, 0))                    # DISPCNT = r1
    emit(add_imm(8, 0, 0x13, 14))       # r8 = 0x04000130 (KEYINPUT)
    emit(mov_imm(9, 0x1C, 12))          # r9 = 0x1CE7: grey step per key bit
    emit(orr_imm(9, 9, 0xE7))
    emit(mov_imm(10, 0x02, 4))          # r10 = 0x02000000 (EWRAM)
    loop = here()
    emit(ldrh(2, 8))                    # r2 = KEYINPUT (active low)
    emit(mvn_reg(3, 2))                 # r3 = ~r2
    emit(mov_reg(3, 3, LSL, 22))        # keep the low 10 bits
    emit(mov_reg(3, 3, LSR, 22))
    emit(mul(4, 3, 9))                  # r4 = keys * 0x1CE7
    emit(mov_reg(4, 4, LSL, 17))        # keep 15 bits
    emit(mov_reg(4, 4, LSR, 17))
    emit(ldrh(11, 10))                  # r11 = halfword at 0x02000000 (0 unless a cheat sets it)
    emit(cmp_imm(3, 0))
    emit(0x03A04C50)                    # moveq r4, #0x5000 (blue when idle)
    emit(0x0184400B)                    # orreq r4, r4, r11 (cheat test: 82000000 001F turns it purple)
    emit(orr_reg(7, 4, 4, LSL, 16))     # r7 = two pixels
    emit(mov_imm(5, 0x06, 4))           # r5 = 0x06000000 (VRAM)
    emit(mov_imm(6, 0x4B, 12))          # r6 = 19200 words (240*160/2)
    fill = here()
    emit(str_post4(7, 5))
    emit(subs_imm(6, 6, 1))
    emit(branch(NE, here(), fill))
    emit(branch(AL, here(), loop))

    rom = bytearray(0x200)
    struct.pack_into("<I", rom, 0, branch(AL, base, start))
    rom[0xA0:0xAC] = b"EMUTEST".ljust(12, b"\0")
    rom[0xAC:0xB0] = b"ETST"
    rom[0xB0:0xB2] = b"00"
    rom[0xB2] = 0x96
    rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
    for i, word in enumerate(code):
        struct.pack_into("<I", rom, 0xC0 + 4 * i, word)
    return bytes(rom)


def tst_imm(rn, imm8, rot=0):
    return 0xE3100000 | rn << 16 | rot << 8 | imm8


def cmp_reg(rn, rm):
    return 0xE1500000 | rn << 16 | rm


def sub_imm(rd, rn, imm8):
    return 0xE2400000 | rn << 16 | rd << 12 | imm8


EQ = 0x0


def gba_header(rom, title, code):
    rom[0xA0:0xAC] = title.ljust(12, b"\0")
    rom[0xAC:0xB0] = code
    rom[0xB0:0xB2] = b"00"
    rom[0xB2] = 0x96
    rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF


def link_gba_rom() -> bytes:
    """GBA multiplayer-mode link test; run the same ROM on two consoles.

    The parent (SI low) sends an incrementing counter n; the child answers each transfer with the
    parent's previous value + 0x100, which it can only have prepared if it processed the previous
    transfer in time. The parent checks every answer: the screen turns green while answers are
    right, red (and stays red) on the first wrong one, and blue while no link is up. The child
    shows cyan while it is receiving.
    """
    base = 0x08000000
    start = base + 0xC0
    code = []

    def emit(word):
        code.append(word)

    def here():
        return start + 4 * len(code)

    labels = {}
    fixups = []

    def branch_to(cond, name):
        fixups.append((len(code), cond, name))
        emit(0)

    def label(name):
        labels[name] = here()

    def fill(colour_imm):  # r7 = colour pair; fills mode 3 VRAM, clobbers r5, r6
        emit(mov_imm(5, 0x06, 4))
        emit(mov_imm(6, 0x4B, 12))
        top = here()
        emit(str_post4(7, 5))
        emit(subs_imm(6, 6, 1))
        emit(branch(NE, here(), top))

    emit(mov_imm(0, 0x04, 4))           # r0 = 0x04000000
    emit(mov_imm(1, 0x04, 12))
    emit(orr_imm(1, 1, 0x03))
    emit(strh(1, 0))                    # DISPCNT = 0x0403
    emit(add_imm(10, 0, 0x01, 12))      # r10 = 0x04000100
    emit(add_imm(12, 0, 0x02, 12))      # r12 = 0x04000200
    emit(mov_imm(1, 0))
    emit(strh(1, 10, 0x34))             # RCNT = 0 (serial mode)
    emit(mov_imm(4, 1))                 # r4 = n

    label("wait_link")
    emit(mov_imm(1, 0x60, 12))          # SIOCNT = 0x6003: multiplayer, IRQ flag on, 115200 baud
    emit(orr_imm(1, 1, 0x03))
    emit(strh(1, 10, 0x28))
    emit(ldrh(1, 10, 0x28))
    emit(tst_imm(1, 0x08))              # SD: link up?
    branch_to(NE, "linked")
    emit(mov_imm(7, 0x7C, 12))          # blue
    emit(orr_reg(7, 7, 7, LSL, 16))
    fill(None)
    branch_to(AL, "wait_link")

    label("linked")
    emit(tst_imm(1, 0x04))              # SI: 0 = parent
    branch_to(NE, "child")

    label("parent")
    emit(strh(4, 10, 0x2A))             # SIOMLT_SEND = n
    emit(ldrh(1, 10, 0x28))
    emit(orr_imm(1, 1, 0x80))
    emit(strh(1, 10, 0x28))             # start
    label("busy")
    emit(ldrh(1, 10, 0x28))
    emit(tst_imm(1, 0x80))
    branch_to(NE, "busy")
    emit(ldrh(2, 10, 0x22))             # r2 = SIOMULTI1 (child's answer)
    emit(sub_imm(3, 4, 1))
    emit(add_imm(3, 3, 0x01, 12))       # r3 = n - 1 + 0x100
    emit(mov_reg(3, 3, LSL, 16))
    emit(mov_reg(3, 3, LSR, 16))
    emit(cmp_imm(4, 1))
    branch_to(EQ, "checked")            # nothing to check on the first transfer
    emit(cmp_reg(2, 3))
    branch_to(NE, "fail")
    label("checked")
    emit(add_imm(4, 4, 1))
    emit(mov_reg(4, 4, LSL, 16))
    emit(mov_reg(4, 4, LSR, 16))
    emit(cmp_imm(4, 0))
    emit(0x03A04001)                    # moveq r4, #1 (skip the check after wrap-around)
    emit(tst_imm(4, 0x3F))
    branch_to(NE, "delay")
    emit(0xE3A07E3E)                    # mov r7, #0x3E0
    emit(orr_reg(7, 7, 7, LSL, 16))
    fill(None)
    label("delay")
    emit(mov_imm(6, 0x40))              # ~256 cycles between transfers
    d = here()
    emit(subs_imm(6, 6, 1))
    emit(branch(NE, here(), d))
    branch_to(AL, "parent")

    label("fail")
    emit(mov_imm(7, 0x1F))              # red
    emit(orr_reg(7, 7, 7, LSL, 16))
    fill(None)
    label("halt")
    branch_to(AL, "halt")

    label("child")
    emit(ldrh(1, 12, 0x02))             # IF
    emit(tst_imm(1, 0x80))              # SIO
    branch_to(EQ, "child")
    emit(mov_imm(1, 0x80))
    emit(strh(1, 12, 0x02))             # acknowledge
    emit(ldrh(2, 10, 0x20))             # r2 = SIOMULTI0 (parent's n)
    emit(add_imm(3, 2, 0x01, 12))       # answer for the next transfer
    emit(strh(3, 10, 0x2A))
    emit(add_imm(4, 4, 1))
    emit(tst_imm(4, 0x3F))
    branch_to(NE, "child")
    emit(0xE3A07C7F)                    # mov r7, #0x7F00 -> cyan-ish
    emit(orr_imm(7, 7, 0xE0))
    emit(orr_reg(7, 7, 7, LSL, 16))
    fill(None)
    branch_to(AL, "child")

    for index, cond, name in fixups:
        code[index] = branch(cond, start + 4 * index, labels[name])

    rom = bytearray(0x400)
    struct.pack_into("<I", rom, 0, branch(AL, base, start))
    gba_header(rom, b"LINKTEST", b"ELNK")
    for i, word in enumerate(code):
        struct.pack_into("<I", rom, 0xC0 + 4 * i, word)
    return bytes(rom)


class SM83:
    """A tiny SM83 (Game Boy CPU) assembler: just what the link test needs."""

    def __init__(self, origin):
        self.origin = origin
        self.code = bytearray()
        self.labels = {}
        self.fixups = []

    def here(self):
        return self.origin + len(self.code)

    def label(self, name):
        self.labels[name] = self.here()

    def db(self, *values):
        self.code += bytes(values)

    def jr(self, cond, name):  # cond: None, "nz", "z", "nc", "c"
        op = {None: 0x18, "nz": 0x20, "z": 0x28, "nc": 0x30, "c": 0x38}[cond]
        self.db(op, 0)
        self.fixups.append(("jr", len(self.code) - 1, name))

    def jp(self, name):
        self.db(0xC3, 0, 0)
        self.fixups.append(("jp", len(self.code) - 2, name))

    def ld_a(self, n): self.db(0x3E, n)
    def ldh_w(self, reg): self.db(0xE0, reg)       # ldh (FF00+reg), a
    def ldh_r(self, reg): self.db(0xF0, reg)       # ldh a, (FF00+reg)

    def colour(self, rgb555):  # CGB background palette 0, colour 0
        self.ld_a(0x80); self.ldh_w(0x68)
        self.ld_a(rgb555 & 0xFF); self.ldh_w(0x69)
        self.ld_a(rgb555 >> 8); self.ldh_w(0x69)

    def finish(self):
        for kind, at, name in self.fixups:
            target = self.labels[name]
            if kind == "jr":
                offset = target - (self.origin + at + 1)
                assert -128 <= offset <= 127, name
                self.code[at] = offset & 0xFF
            else:
                self.code[at] = target & 0xFF
                self.code[at + 1] = target >> 8
        return bytes(self.code)


def link_gb_rom(master: bool) -> bytes:
    """GB serial link test (CGB mode); linktest_master.gb on one console, linktest_slave.gb on the other.

    The master clocks an incrementing byte n; the slave answers each transfer with the byte it last
    received + 0x40, which it can only have prepared if it handled the previous transfer in time.
    The master checks every answer: green while right, red (and halts) on the first wrong one. The
    slave shows cyan while it is receiving. Both start blue.
    """
    a = SM83(0x150)
    a.db(0xF3)                                    # di
    a.label("vblank")
    a.ldh_r(0x44); a.db(0xFE, 144)                # cp 144 (LY)
    a.jr("c", "vblank")
    a.db(0xAF); a.ldh_w(0x40)                     # LCD off
    a.db(0x21, 0x00, 0x98)                        # ld hl, $9800
    a.db(0x01, 0x00, 0x04)                        # ld bc, $0400
    a.label("clear")
    a.db(0xAF, 0x22, 0x0B, 0x78, 0xB1)            # xor a; ld (hl+),a; dec bc; ld a,b; or c
    a.jr("nz", "clear")
    a.ld_a(0x91); a.ldh_w(0x40)                   # LCD on, BG on
    a.colour(0x7C00)                              # blue
    a.db(0x16, 0x00)                              # ld d, 0 (transfer counter)
    if master:
        a.db(0x06, 0x01)                          # ld b, 1 (n)
        a.label("loop")
        a.db(0x78); a.ldh_w(0x01)                 # SB = n
        a.ld_a(0x81); a.ldh_w(0x02)               # SC = start, internal clock
        a.label("busy")
        a.ldh_r(0x02); a.db(0xCB, 0x7F)           # bit 7, a
        a.jr("nz", "busy")
        a.ldh_r(0x01); a.db(0x4F)                 # ld c, a (received)
        a.db(0x78, 0xFE, 0x01)                    # ld a,b; cp 1
        a.jr("z", "ok")                           # nothing to check on the first transfer
        a.db(0x78, 0xC6, 0x3F, 0xB9)              # ld a,b; add a,$3F; cp c   (expect n-1+$40)
        a.jr("nz", "fail")
        a.label("ok")
        a.db(0x04)                                # inc b
        a.db(0x14, 0x7A, 0xE6, 0x3F)              # inc d; ld a,d; and $3F
        a.jr("nz", "delay")
        a.colour(0x03E0)                          # green
        a.label("delay")
        a.db(0x1E, 0x10)                          # ld e, 16
        a.label("wait")
        a.db(0x1D)                                # dec e
        a.jr("nz", "wait")
        a.jp("loop")
        a.label("fail")
        a.colour(0x001F)                          # red
        a.label("halt")
        a.jr(None, "halt")
    else:
        a.db(0xAF); a.ldh_w(0x01)                 # SB = 0
        a.ld_a(0x80); a.ldh_w(0x02)               # arm, external clock
        a.label("loop")
        a.ldh_r(0x02); a.db(0xCB, 0x7F)
        a.jr("nz", "loop")                        # wait for the transfer to finish
        a.ldh_r(0x01); a.db(0xC6, 0x40); a.ldh_w(0x01)   # SB = received + $40
        a.ld_a(0x80); a.ldh_w(0x02)               # re-arm
        a.db(0x14, 0x7A, 0xE6, 0x3F)              # inc d; ld a,d; and $3F
        a.jr("nz", "loop")
        a.colour(0x7FE0)                          # cyan
        a.jp("loop")
    code = a.finish()

    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])   # nop; jp $0150
    rom[0x104:0x108] = bytes([0xCE, 0xED, 0x66, 0x66])  # header signature emulators check for
    rom[0x134:0x143] = (b"LINKMASTER" if master else b"LINKSLAVE").ljust(15, b"\0")
    rom[0x143] = 0x80                                       # CGB
    rom[0x147] = 0x00                                       # ROM only
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - (0x14D - 0x134)) & 0xFF
    rom[0x150:0x150 + len(code)] = code
    return bytes(rom)


def key_colour_loop(emit, here, vram_imm, words_imm):
    """KEYINPUT -> colour fill loop shared by both test ROMs. Expects r8 = &KEYINPUT, r9 = grey step."""
    loop = here()
    emit(ldrh(2, 8))                    # r2 = KEYINPUT (active low)
    emit(mvn_reg(3, 2))
    emit(mov_reg(3, 3, LSL, 22))
    emit(mov_reg(3, 3, LSR, 22))
    emit(mul(4, 3, 9))
    emit(mov_reg(4, 4, LSL, 17))
    emit(mov_reg(4, 4, LSR, 17))
    emit(cmp_imm(3, 0))
    emit(0x03A04C50)                    # moveq r4, #0x5000 (blue when idle)
    emit(orr_reg(7, 4, 4, LSL, 16))
    emit(mov_imm(5, *vram_imm))
    emit(mov_imm(6, *words_imm))
    fill = here()
    emit(str_post4(7, 5))
    emit(subs_imm(6, 6, 1))
    emit(branch(NE, here(), fill))
    emit(branch(AL, here(), loop))


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def nds_rom() -> bytes:
    arm9_addr, arm7_addr = 0x02000000, 0x02380000
    code = []

    def emit(word):
        code.append(word)

    def here():
        return arm9_addr + 4 * len(code)

    emit(mov_imm(0, 0x04, 4))           # r0 = 0x04000000
    emit(add_imm(1, 0, 0x03, 12))       # r1 = 0x04000300
    emit(mov_imm(2, 0x80, 12))          # POWCNT1 = 0x8003: LCDs + 2D engine A, A on top
    emit(orr_imm(2, 2, 0x03))
    emit(strh(2, 1, 4))
    emit(mov_imm(2, 0x02, 8))           # DISPCNT_A = 0x00020000: VRAM display (bank A)
    emit(str32(2, 0))
    emit(add_imm(1, 0, 0x24, 14))       # r1 = 0x04000240 (VRAMCNT_A)
    emit(mov_imm(2, 0x80))
    emit(strb(2, 1))                    # bank A enabled, LCDC mode
    emit(add_imm(8, 0, 0x13, 14))       # r8 = 0x04000130 (KEYINPUT)
    emit(mov_imm(9, 0x1C, 12))
    emit(orr_imm(9, 9, 0xE7))
    key_colour_loop(emit, here, (0x68, 6), (0x60, 12))   # 0x06800000, 256*192/2 words
    arm9 = b"".join(struct.pack("<I", w) for w in code)
    arm7 = struct.pack("<I", 0xEAFFFFFE)                  # b .

    rom = bytearray(0x20000)
    arm9_off, arm7_off, banner_off = 0x1000, 0x2000, 0x3000
    rom[0x00:0x0C] = b"EMUTEST".ljust(12, b"\0")
    rom[0x0C:0x10] = b"ETST"
    rom[0x10:0x12] = b"00"
    struct.pack_into("<IIII", rom, 0x20, arm9_off, arm9_addr, arm9_addr, len(arm9))
    struct.pack_into("<IIII", rom, 0x30, arm7_off, arm7_addr, arm7_addr, len(arm7))
    struct.pack_into("<I", rom, 0x68, banner_off)
    struct.pack_into("<II", rom, 0x80, len(rom), 0x4000)
    struct.pack_into("<H", rom, 0x15C, 0xCF56)
    struct.pack_into("<H", rom, 0x15E, crc16(bytes(rom[:0x15E])))
    rom[arm9_off:arm9_off + len(arm9)] = arm9
    rom[arm7_off:arm7_off + len(arm7)] = arm7

    # Banner (version 1): a two-colour checker icon and the same title in every language.
    banner = bytearray(0x840)
    struct.pack_into("<H", banner, 0, 1)
    for tile in range(16):
        nibble = 1 if (tile + tile // 4) % 2 == 0 else 2
        banner[0x20 + tile * 32:0x40 + tile * 32] = bytes([nibble | nibble << 4]) * 32
    struct.pack_into("<HHH", banner, 0x220, 0, 0x7C1F, 0x03E0)  # transparent, magenta, green
    title = "EmuTest DS\nDual screen test\nEmulate".encode("utf-16-le")
    for lang in range(6):
        banner[0x240 + lang * 0x100:0x240 + lang * 0x100 + len(title)] = title
    struct.pack_into("<H", banner, 2, crc16(bytes(banner[0x20:0x840])))
    rom[banner_off:banner_off + len(banner)] = banner
    return bytes(rom)


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    (out / "emutest.gba").write_bytes(gba_rom())
    (out / "emutest.nds").write_bytes(nds_rom())
    (out / "linktest.gba").write_bytes(link_gba_rom())
    (out / "linktest_master.gb").write_bytes(link_gb_rom(True))
    (out / "linktest_slave.gb").write_bytes(link_gb_rom(False))
    print("wrote", out / "emutest.gba", out / "emutest.nds")
