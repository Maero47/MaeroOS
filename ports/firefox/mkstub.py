#!/usr/bin/env python3
"""Write a minimal, symbol-less i386 ELF shared object.

Used by fetch-runtime.sh when no i686 C compiler is available to build the
GL stub libraries (libGL.so.1, libEGL.so.1, ...).  The output is what
`gcc -m32 -shared -nostdlib` would produce for an empty translation unit,
reduced to what the glibc dynamic loader actually reads: one RW PT_LOAD
covering the file, PT_DYNAMIC, PT_GNU_STACK (non-executable), and a dynamic
section carrying DT_SONAME plus empty DT_HASH/DT_SYMTAB/DT_STRTAB tables.
Section headers are included so readelf/objdump can inspect the result.

Usage: mkstub.py OUTPUT SONAME
"""
import struct
import sys


def align(n, a):
    return (n + a - 1) & ~(a - 1)


def build(soname):
    # --- string tables ------------------------------------------------------
    dynstr = b"\0" + soname.encode() + b"\0"
    soname_off = 1
    shstr_names = [b"", b".hash", b".dynsym", b".dynstr", b".dynamic", b".shstrtab"]
    shstrtab = b""
    shstr_off = []
    for n in shstr_names:
        shstr_off.append(len(shstrtab))
        shstrtab += n + b"\0"

    # --- fixed-size pieces ---------------------------------------------------
    ehdr_size, phdr_size, shdr_size = 52, 32, 40
    nphdr, nshdr = 3, 6
    dynsym = b"\0" * 16                                  # STN_UNDEF only
    dyn_hash = struct.pack("<IIII", 1, 1, 0, 0)          # nbucket=1 nchain=1
    ndyn = 7
    dynamic_size = ndyn * 8

    # --- layout ----------------------------------------------------------------
    off = ehdr_size + nphdr * phdr_size
    hash_off = align(off, 4);        off = hash_off + len(dyn_hash)
    dynsym_off = align(off, 4);      off = dynsym_off + len(dynsym)
    dynstr_off = off;                off = dynstr_off + len(dynstr)
    dynamic_off = align(off, 4);     off = dynamic_off + dynamic_size
    shstr_off_file = off;            off = shstr_off_file + len(shstrtab)
    shdr_off = align(off, 4)
    total = shdr_off + nshdr * shdr_size

    dynamic = b"".join(struct.pack("<iI", tag, val) for tag, val in (
        (14, soname_off),      # DT_SONAME
        (4, hash_off),         # DT_HASH
        (5, dynstr_off),       # DT_STRTAB
        (6, dynsym_off),       # DT_SYMTAB
        (10, len(dynstr)),     # DT_STRSZ
        (11, 16),              # DT_SYMENT
        (0, 0),                # DT_NULL
    ))

    # --- ELF header ------------------------------------------------------------
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8   # ELFCLASS32, LSB, EV_CURRENT, SYSV
    ehdr = ident + struct.pack("<HHIIIIIHHHHHH",
        3,            # e_type   ET_DYN
        3,            # e_machine EM_386
        1,            # e_version
        0,            # e_entry
        ehdr_size,    # e_phoff
        shdr_off,     # e_shoff
        0,            # e_flags
        ehdr_size, phdr_size, nphdr, shdr_size, nshdr,
        5)            # e_shstrndx

    # --- program headers ---------------------------------------------------------
    PF_R, PF_W = 4, 2
    phdrs = struct.pack("<IIIIIIII", 1, 0, 0, 0, total, total, PF_R | PF_W, 0x1000)     # PT_LOAD
    phdrs += struct.pack("<IIIIIIII", 2, dynamic_off, dynamic_off, dynamic_off,
                         dynamic_size, dynamic_size, PF_R | PF_W, 4)                     # PT_DYNAMIC
    phdrs += struct.pack("<IIIIIIII", 0x6474e551, 0, 0, 0, 0, 0, PF_R | PF_W, 16)       # PT_GNU_STACK

    # --- section headers ----------------------------------------------------------
    SHF_ALLOC, SHF_WRITE = 2, 1
    def sh(name, typ, flags, addr, offset, size, link, info, addralign, entsize):
        return struct.pack("<IIIIIIIIII", name, typ, flags, addr, offset, size, link, info, addralign, entsize)
    shdrs = sh(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    shdrs += sh(shstr_off[1], 5, SHF_ALLOC, hash_off, hash_off, len(dyn_hash), 2, 0, 4, 4)
    shdrs += sh(shstr_off[2], 11, SHF_ALLOC, dynsym_off, dynsym_off, len(dynsym), 3, 1, 4, 16)
    shdrs += sh(shstr_off[3], 3, SHF_ALLOC, dynstr_off, dynstr_off, len(dynstr), 0, 0, 1, 0)
    shdrs += sh(shstr_off[4], 6, SHF_ALLOC | SHF_WRITE, dynamic_off, dynamic_off, dynamic_size, 3, 0, 4, 8)
    shdrs += sh(shstr_off[5], 3, 0, 0, shstr_off_file, len(shstrtab), 0, 0, 1, 0)

    out = bytearray(total)
    out[0:len(ehdr)] = ehdr
    out[ehdr_size:ehdr_size + len(phdrs)] = phdrs
    out[hash_off:hash_off + len(dyn_hash)] = dyn_hash
    out[dynsym_off:dynsym_off + len(dynsym)] = dynsym
    out[dynstr_off:dynstr_off + len(dynstr)] = dynstr
    out[dynamic_off:dynamic_off + dynamic_size] = dynamic
    out[shstr_off_file:shstr_off_file + len(shstrtab)] = shstrtab
    out[shdr_off:shdr_off + len(shdrs)] = shdrs
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: mkstub.py OUTPUT SONAME\n")
        sys.exit(2)
    with open(sys.argv[1], "wb") as f:
        f.write(build(sys.argv[2]))


if __name__ == "__main__":
    main()
