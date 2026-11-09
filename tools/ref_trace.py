"""Reference trace generator using BrickEmuPy's E0C6200 core.

Usage: python ref_trace.py <BrickEmuPy dir> <rom.bin> <steps> [interval]

Prints one checkpoint line per `interval` steps in the same format as
`host_test trace`, so the two outputs can be diffed to validate the C core.
"""
import sys

sys.path.insert(0, sys.argv[1])
from cores.E0C6200 import E0C6200  # noqa: E402


class DummyInterconnect:
    def register_pin_device(self, dev):
        pass

    def emit_audio(self, *a):
        pass

    def emit_port(self, *a):
        pass


def fnv(data):
    h = 0x811C9DC5
    for v in data:
        h ^= v & 0xFF
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


rom_path = sys.argv[2]
steps = int(sys.argv[3])
interval = int(sys.argv[4]) if len(sys.argv) > 4 else 10000

mask = {"rom_path": rom_path, "port_pullup": {"K0": 15, "K1": 15}, "p3_dedicated": 0}
cpu = E0C6200(mask, 1060000, DummyInterconnect())

BUTTONS = [4, 2, 1]  # A (top), B (center), C (bottom) on port K0
lcg = 12345
press_at = 0
release_at = -1
held = 0
for step in range(1, steps + 1):
    if step == press_at:
        lcg = (lcg * 1103515245 + 12345) & 0x7FFFFFFF
        held = (lcg >> 16) % 3
        cpu.pin_handler("K0", BUTTONS[held], 0)
        release_at = step + 3000
    if step == release_at:
        cpu.pin_handler("K0", BUTTONS[held], -1)
        lcg = (lcg * 1103515245 + 12345) & 0x7FFFFFFF
        press_at = step + 20000 + (lcg >> 16) % 40000
    if step == 1:
        press_at = 200000
    cpu.clock()
    if step % interval == 0:
        s = cpu
        print(
            "%d pc=%04X a=%X b=%X ix=%03X iy=%03X sp=%02X f=%d%d%d%d h=%d tm=%02X lcd=%X ram=%08X vram=%08X"
            % (
                step, s._PC, s._A, s._B, s._IX, s._IY, s._SP,
                int(s._IF), int(s._DF), int(s._ZF), int(s._CF), int(s._HALT),
                s._TM, s._CTRL_LCD, fnv(s._RAM), fnv(s._VRAM),
            )
        )
