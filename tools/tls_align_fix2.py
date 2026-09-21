#!/usr/bin/env python3
# v2: corrige tanto p_align como el "skew" (p_vaddr % p_align), no solo
# p_align. Redondea p_vaddr Y p_offset hacia abajo al multiplo de 64 mas
# cercano (moviendolos juntos, misma cantidad, para no romper la relacion
# vaddr/offset), y expande p_memsz/p_filesz por esa misma diferencia para
# no perder ningun byte real de la seccion.
import struct, sys

path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read())
    assert data[0:4] == b'\x7fELF' and data[4] == 2, "se esperaba ELF64"

    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]

    PT_TLS = 7
    ALIGN = 64
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if struct.unpack_from("<I", data, off)[0] != PT_TLS:
            continue

        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<QQQQQQ", data, off + 8)

        skew_before = p_vaddr % ALIGN
        print(f"antes: p_align={p_align} p_vaddr=0x{p_vaddr:x} "
              f"skew={skew_before} p_offset=0x{p_offset:x}")

        delta = p_vaddr % ALIGN  # cuanto hay que retroceder para alinear
        new_vaddr = p_vaddr - delta
        new_paddr = p_paddr - delta
        new_offset = p_offset - delta
        new_filesz = p_filesz + delta
        new_memsz = p_memsz + delta

        struct.pack_into("<QQQQQQ", data, off + 8,
                          new_offset, new_vaddr, new_paddr,
                          new_filesz, new_memsz, ALIGN)

        print(f"despues: p_align={ALIGN} p_vaddr=0x{new_vaddr:x} "
              f"skew={new_vaddr % ALIGN} p_offset=0x{new_offset:x}")

    f.seek(0)
    f.write(data)
print("listo:", path)
