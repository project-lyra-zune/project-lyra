#!/usr/bin/env python3
"""Resolve a DLL's imports by name using the imported DLL's export table.
For Windows CE, since most imports are by ordinal."""
import struct, sys, argparse
from pathlib import Path

def parse_pe(data):
    e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
    optional_size = struct.unpack_from('<H', data, e_lfanew + 0x14)[0]
    num_sections = struct.unpack_from('<H', data, e_lfanew + 6)[0]
    opt_off = e_lfanew + 0x18
    image_base = struct.unpack_from('<I', data, opt_off + 0x1c)[0]
    sections = []
    sec_off = opt_off + optional_size
    for i in range(num_sections):
        o = sec_off + i*0x28
        name = data[o:o+8].rstrip(b'\x00').decode('latin-1')
        vsize = struct.unpack_from('<I', data, o + 8)[0]
        vaddr = struct.unpack_from('<I', data, o + 12)[0]
        rsize = struct.unpack_from('<I', data, o + 16)[0]
        roff  = struct.unpack_from('<I', data, o + 20)[0]
        sections.append((name, vaddr, vsize, roff, rsize))
    exp_rva = struct.unpack_from('<I', data, opt_off + 0x60)[0]
    imp_rva = struct.unpack_from('<I', data, opt_off + 0x60 + 8)[0]
    return {
        'image_base': image_base,
        'sections': sections,
        'exp_rva': exp_rva,
        'imp_rva': imp_rva,
    }

def rva2fo(pe, rva):
    for name, vaddr, vsize, roff, rsize in pe['sections']:
        if vaddr <= rva < vaddr + vsize:
            return roff + (rva - vaddr)
    return None

def parse_exports(data, pe):
    """Returns dict: ordinal -> name"""
    if not pe['exp_rva']:
        return {}
    eo = rva2fo(pe, pe['exp_rva'])
    if eo is None: return {}
    base_ord = struct.unpack_from('<I', data, eo + 0x10)[0]
    n_func = struct.unpack_from('<I', data, eo + 0x14)[0]
    n_name = struct.unpack_from('<I', data, eo + 0x18)[0]
    names_rva = struct.unpack_from('<I', data, eo + 0x20)[0]
    ords_rva = struct.unpack_from('<I', data, eo + 0x24)[0]
    names_fo = rva2fo(pe, names_rva)
    ords_fo = rva2fo(pe, ords_rva)
    result = {}
    if names_fo and ords_fo:
        for i in range(n_name):
            name_rva = struct.unpack_from('<I', data, names_fo + i*4)[0]
            ord_idx = struct.unpack_from('<H', data, ords_fo + i*2)[0]
            n_fo = rva2fo(pe, name_rva)
            end = data.find(b'\x00', n_fo) if n_fo else 0
            fn_name = data[n_fo:end].decode('latin-1', errors='replace') if n_fo else '?'
            result[base_ord + ord_idx] = fn_name
    return result

def parse_imports(data, pe):
    """Returns list of (dll_name, [(ordinal, ft_rva)])"""
    if not pe['imp_rva']:
        return []
    fo = rva2fo(pe, pe['imp_rva'])
    imports = []
    i = 0
    while True:
        o = fo + i*20
        oft = struct.unpack_from('<I', data, o)[0]
        name_rva = struct.unpack_from('<I', data, o + 12)[0]
        ft = struct.unpack_from('<I', data, o + 16)[0]
        if oft == 0 and ft == 0:
            break
        name_fo = rva2fo(pe, name_rva)
        end = data.find(b'\x00', name_fo) if name_fo else 0
        dll_name = data[name_fo:end].decode('latin-1') if name_fo else '?'
        # walk thunks
        thunks = []
        thunk_fo = rva2fo(pe, oft or ft)
        if thunk_fo:
            for j in range(2000):
                t = struct.unpack_from('<I', data, thunk_fo + j*4)[0]
                if t == 0: break
                if t & 0x80000000:
                    thunks.append(('ordinal', t & 0xFFFF, ft + j*4))
                else:
                    # by name
                    n_fo = rva2fo(pe, t)
                    if n_fo:
                        e = data.find(b'\x00', n_fo + 2)
                        fn = data[n_fo+2:e].decode('latin-1')
                        thunks.append(('name', fn, ft + j*4))
        imports.append((dll_name, thunks))
        i += 1
    return imports

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('subject', help='DLL whose imports to resolve')
    ap.add_argument('--firmware', default='firmwares/extracted/v4.5-eimgfs-nk-out/xip',
                    help='Directory containing the imported DLLs')
    args = ap.parse_args()

    sdata = Path(args.subject).read_bytes()
    spe = parse_pe(sdata)
    base = spe['image_base']
    print(f'Subject: {Path(args.subject).name}  image_base=0x{base:x}')

    fw = Path(args.firmware)
    imp = parse_imports(sdata, spe)
    for dll_name, thunks in imp:
        # Find the DLL
        for candidate in (dll_name, dll_name.lower(), f'k.{dll_name}', f'k.{dll_name.lower()}'):
            p = fw / candidate
            if p.exists():
                ddata = p.read_bytes()
                dpe = parse_pe(ddata)
                exports = parse_exports(ddata, dpe)
                print(f'\n[{dll_name}] image_base=0x{dpe["image_base"]:x} exports={len(exports)}')
                for kind, val, ft_rva in thunks:
                    slot_va = base + ft_rva
                    if kind == 'ordinal':
                        name = exports.get(val, f'<ord #{val} not exported>')
                        print(f'  slot {slot_va:08x}: ord #{val:5d} = {name}')
                    else:
                        print(f'  slot {slot_va:08x}: name = {val}')
                break
        else:
            print(f'\n[{dll_name}] not found in {fw}; thunks:')
            for kind, val, ft_rva in thunks:
                slot_va = base + ft_rva
                if kind == 'ordinal':
                    print(f'  slot {slot_va:08x}: ord #{val}')

if __name__ == '__main__':
    main()
