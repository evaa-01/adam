# Yay0 (an LZ format common on the N64, the game's func_80097780) for the ROM's packed assets.
# Usage: python scripts\yay0.py list                 all Yay0 files in the ROM (offset, packed -> unpacked size)
#        python scripts\yay0.py find <ramdump> <addr> <len>   which ROM file unpacks to contain these dump bytes
import os, struct, sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project folder (scripts\..)


def decompress(data, offset=0):
    """Unpacks the Yay0 file at data[offset:]: header 'Yay0', size, link table offset, chunk offset."""
    magic, size, link_off, chunk_off = struct.unpack_from(">4sIII", data, offset)
    assert magic == b"Yay0"
    out = bytearray()
    mask_pos, link_pos, chunk_pos = offset + 16, offset + link_off, offset + chunk_off
    bits, mask = 0, 0
    while len(out) < size:
        if bits == 0:
            mask = struct.unpack_from(">I", data, mask_pos)[0]
            mask_pos += 4
            bits = 32
        if mask & 0x80000000:  # literal byte
            out.append(data[chunk_pos])
            chunk_pos += 1
        else:  # back-reference: 12-bit distance, 4-bit length (0 = length from the next chunk byte + 0x12)
            link = struct.unpack_from(">H", data, link_pos)[0]
            link_pos += 2
            count = link >> 12
            src = len(out) - (link & 0xFFF) - 1
            if count == 0:
                count = data[chunk_pos] + 0x12
                chunk_pos += 1
            else:
                count += 2
            for i in range(count):
                out.append(out[src + i])
        mask = (mask << 1) & 0xFFFFFFFF
        bits -= 1
    return bytes(out)


def find_all(rom):
    found, at = [], rom.find(b"Yay0")
    while at >= 0:
        size, link_off, chunk_off = struct.unpack_from(">III", rom, at + 4)
        if 0 < size < 0x400000 and 16 <= link_off < chunk_off < 0x400000:
            found.append(at)
        at = rom.find(b"Yay0", at + 4)
    return found


def main():
    rom = open(os.path.join(BASE, "rom", "game.z64"), "rb").read()
    if sys.argv[1] == "list":
        for at in find_all(rom):
            print(f"ROM 0x{at:07X}: unpacks to 0x{struct.unpack_from('>I', rom, at + 4)[0]:X} bytes")
    elif sys.argv[1] == "find":
        ram = open(sys.argv[2], "rb").read()
        addr, length = int(sys.argv[3], 16) & 0x3FFFFF, int(sys.argv[4], 16)
        needle = ram[addr:addr + length]
        for at in find_all(rom):
            data = decompress(rom, at)
            pos = data.find(needle)
            if pos >= 0:
                print(f"ROM file 0x{at:07X} (0x{len(data):X} bytes) contains it at offset 0x{pos:X}")


if __name__ == "__main__":
    main()
