"""Split BTU1 console captures into contiguous PowerPC binary regions."""
import argparse
from pathlib import Path
import struct

PAGE_SIZE = 4096
RANGES = ((0x800D6000, 0x800E6000), (0x80070000, 0x80078000))
HEADER = struct.Struct(">4sII")


def parse_capture(data):
    pages = {}
    while data:
        if len(data) < HEADER.size + PAGE_SIZE:
            raise ValueError("registro BTU1 truncado")
        magic, address, size = HEADER.unpack_from(data)
        if (magic != b"BTU1" or size != PAGE_SIZE or address % PAGE_SIZE
                or address in pages or not any(lo <= address < hi for lo, hi in RANGES)):
            raise ValueError(f"registro BTU1 inválido: {address:08X}")
        end = HEADER.size + size
        pages[address] = data[HEADER.size:end]
        data = data[end:]
    if not pages:
        raise ValueError("captura sem páginas completas")
    regions = {}
    start = None
    for address, page in sorted(pages.items()):
        if start is None or address != start + len(regions[start]):
            start = address
            regions[start] = bytearray()
        regions[start].extend(page)
    return regions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        regions = parse_capture(args.capture.read_bytes())
    except ValueError as error:
        parser.error(str(error))
    args.output.mkdir(parents=True, exist_ok=True)
    for address, region in regions.items():
        path = args.output / f"{address:08x}.bin"
        path.write_bytes(region)
        print(f"{path}: base=0x{address:08X}, bytes={len(region)}")


if __name__ == "__main__":
    main()
