#!/usr/bin/env python3
# Dump the RPC surface of a CE 6 stream-device service DLL.
#
# Usage: zune-dump-service-rpc.py <path-to-dll>
#
# Looks at the XX_IOControl export, identifies the dispatch pattern (jump
# table vs if-chain), enumerates IOCTL codes + sub-commands, and resolves
# each handler's first-call target. Output is plain text, intended for
# pasting into experiment notes or regex-scanning across services.

import argparse
import struct
import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM


def parse_pe(data):
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e_lfanew:e_lfanew+4] != b'PE\x00\x00':
        raise ValueError('not a PE file')
    file_h = e_lfanew + 4
    nsec = struct.unpack_from('<H', data, file_h + 2)[0]
    opt_size = struct.unpack_from('<H', data, file_h + 16)[0]
    opt = file_h + 20
    image_base = struct.unpack_from('<I', data, opt + 28)[0]
    exp_rva = struct.unpack_from('<I', data, opt + 96)[0]
    sections_off = opt + opt_size
    sections = []
    for i in range(nsec):
        s = sections_off + i * 40
        vsize = struct.unpack_from('<I', data, s + 8)[0]
        vaddr = struct.unpack_from('<I', data, s + 12)[0]
        rsize = struct.unpack_from('<I', data, s + 16)[0]
        roff = struct.unpack_from('<I', data, s + 20)[0]
        sections.append((vaddr, vsize, roff, rsize))
    return image_base, exp_rva, sections


def make_rva_to_off(sections):
    def rva_to_off(rva):
        for v, vs, ro, rs in sections:
            if v <= rva < v + max(vs, rs):
                return rva - v + ro
        return None
    return rva_to_off


def list_exports(data, image_base, exp_rva, rva_to_off):
    if exp_rva == 0:
        return []
    eo = rva_to_off(exp_rva)
    n_names = struct.unpack_from('<I', data, eo + 0x18)[0]
    func_rva = struct.unpack_from('<I', data, eo + 0x1c)[0]
    name_rva = struct.unpack_from('<I', data, eo + 0x20)[0]
    ord_rva = struct.unpack_from('<I', data, eo + 0x24)[0]
    fo = rva_to_off(func_rva)
    no = rva_to_off(name_rva)
    oo = rva_to_off(ord_rva)
    out = []
    for i in range(n_names):
        nrv = struct.unpack_from('<I', data, no + i * 4)[0]
        nm_off = rva_to_off(nrv)
        nm = data[nm_off:nm_off + 128].split(b'\x00')[0].decode('ascii', 'replace')
        ord_idx = struct.unpack_from('<H', data, oo + i * 2)[0]
        f_rva = struct.unpack_from('<I', data, fo + ord_idx * 4)[0]
        out.append((image_base + f_rva, nm))
    return out


# ARM dispatch-table marker:
#   lsl Rd, Rsel, #1
#   add Rd, Rd, pc
#   ldrh Rd, [Rd, #N]
#   add pc, pc, Rd
def find_jump_table(md, code, base_va):
    seq = list(md.disasm(code, base_va))
    for i in range(len(seq) - 3):
        a, b, c, d = seq[i], seq[i + 1], seq[i + 2], seq[i + 3]
        if (a.mnemonic == 'lsl' and a.op_str.endswith('#1')
            and b.mnemonic == 'add' and 'pc' in b.op_str
            and c.mnemonic == 'ldrh'
            and d.mnemonic == 'add' and 'pc, pc, ' in d.op_str):
            try:
                ldrh_imm = int(c.op_str.split('#')[-1].rstrip(']'), 0)
            except Exception:
                ldrh_imm = 0
            # Table starts at: pc-of-add-r-pc + 8 + ldrh_imm
            #   add Rd, Rd, pc: pc when add executes is b.address + 8
            #   ldrh Rd, [Rd, #imm]: reads from (b.address + 8) + imm
            table_va = b.address + 8 + ldrh_imm
            # add pc, pc, Rd: pc when add executes is d.address + 8
            base_for_target = d.address + 8
            return (a, b, c, d, table_va, base_for_target)
    return None


# ARM if-chain marker: repeated CMP-immediate against the same register
def find_if_chain(md, code, base_va):
    seq = list(md.disasm(code, base_va))
    cmps = []
    for i, inst in enumerate(seq):
        if inst.mnemonic == 'cmp' and '#' in inst.op_str:
            parts = inst.op_str.split(',')
            if len(parts) == 2:
                reg = parts[0].strip()
                try:
                    imm = int(parts[1].strip().split('#')[1], 0)
                except Exception:
                    continue
                # Look at next instruction for branch
                if i + 1 < len(seq):
                    nx = seq[i + 1]
                    if nx.mnemonic.startswith('b') and len(nx.mnemonic) <= 3:
                        try:
                            tgt = int(nx.op_str.lstrip('#'), 0)
                        except Exception:
                            tgt = None
                        if tgt is not None:
                            cmps.append((inst.address, reg, imm, nx.mnemonic, tgt))
    return cmps


def emit_jump_table(md, data, rva_to_off, image_base, jt, max_table):
    a, b, c, d, table_va, base_for_target = jt
    print(f'-- jump table @ 0x{table_va:08x}, target_base 0x{base_for_target:08x} --')
    table_off = rva_to_off(table_va - image_base)
    if table_off is None:
        print('  (table out of range)')
        return
    targets = []
    for i in range(max_table):
        hw = struct.unpack_from('<H', data, table_off + i * 2)[0]
        tgt = base_for_target + hw
        tgt_off = rva_to_off(tgt - image_base)
        # Termination heuristics:
        #   1) halfword resolves outside the file
        #   2) halfword < 0x10 (table never reaches that close to dispatch site)
        #   3) halfword > 0x800 (table jumps too far; likely fallen off the
        #      end into handler code being misread as a table entry)
        if tgt_off is None or hw < 0x10 or hw > 0x800:
            break
        targets.append((i, tgt))
    for idx, tgt in targets:
        tgt_off = rva_to_off(tgt - image_base)
        handler_code = data[tgt_off:tgt_off + 0x10]
        insts = list(md.disasm(handler_code, tgt))[:3]
        disasm = '; '.join(f'{i.mnemonic} {i.op_str}' for i in insts) if insts else '(empty)'
        print(f'  sub_cmd 0x{idx:02x} -> 0x{tgt:08x}: {disasm}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('--max-table', type=int, default=64)
    ap.add_argument('--scan-bytes', type=int, default=0x600)
    args = ap.parse_args()

    data = open(args.dll, 'rb').read()
    image_base, exp_rva, sections = parse_pe(data)
    rva_to_off = make_rva_to_off(sections)
    exports = list_exports(data, image_base, exp_rva, rva_to_off)

    ioctl_export = next((e for e in exports if e[1].endswith('_IOControl')), None)
    if ioctl_export is None:
        print(f'{args.dll}: no _IOControl export found', file=sys.stderr)
        return 1
    ioc_va, ioc_name = ioctl_export
    print(f'== {args.dll} ==')
    print(f'IOControl: {ioc_name} @ 0x{ioc_va:08x}')

    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)

    ioc_off = rva_to_off(ioc_va - image_base)
    code = data[ioc_off:ioc_off + args.scan_bytes]

    print()
    print('-- top-level IOCTL codes (r1 = dwCode) --')
    chain = find_if_chain(md, code, ioc_va)
    main_targets = []
    for addr, reg, imm, br, tgt in chain:
        if reg in ('r1', 'r3') and imm > 0x10:
            print(f'  0x{addr:08x}: cmp {reg}, #0x{imm:08x}  {br} 0x{tgt:08x}')
            if br == 'beq' and imm > 0x100:
                main_targets.append((imm, tgt))

    # Follow each "main work" branch and look for the jump table at the
    # target. Most services have one such branch with the inner jump table.
    for imm, tgt in main_targets:
        sub_off = rva_to_off(tgt - image_base)
        if sub_off is None:
            continue
        sub_code = data[sub_off:sub_off + args.scan_bytes]
        jt = find_jump_table(md, sub_code, tgt)
        if jt:
            print()
            print(f'-- sub-command dispatch reached via dwCode=0x{imm:x} (handler 0x{tgt:08x}) --')
            emit_jump_table(md, data, rva_to_off, image_base, jt, args.max_table)

    # Byte-aligned raw-encoding scan for the 4-instruction dispatch pattern
    # (lsl Rd, Rs, #1 / add Rd, Rd, pc / ldrh Rd, [Rd, #imm] / add pc, pc, Rd).
    # Avoids capstone linear-disasm desync near in-section data.
    print()
    print('-- whole-.text jump table scan --')
    text_section = sections[0]
    text_va = image_base + text_section[0]
    text_off = text_section[2]
    text_size = text_section[3]
    seen = set()
    for off in range(0, text_size - 16, 4):
        w0 = struct.unpack_from('<I', data, text_off + off)[0]
        # lsl Rd, Rm, #1 == mov Rd, Rm, lsl #1
        # encoding: AL | 000 | 1101 (MOV) | S=0 | Rn=0 | Rd | imm5=1 | LSL=00 | bit4=0 | Rm
        if (w0 & 0xffff_0ff0) != 0xe1a0_0080:
            continue
        Rd = (w0 >> 12) & 0xf
        if Rd == 15:
            continue
        w1 = struct.unpack_from('<I', data, text_off + off + 4)[0]
        # add Rd, Rn, pc (Rn == Rd in our pattern)
        if (w1 & 0xfff0_0fff) != 0xe080_000f:
            continue
        if ((w1 >> 12) & 0xf) != Rd or ((w1 >> 16) & 0xf) != Rd:
            continue
        w2 = struct.unpack_from('<I', data, text_off + off + 8)[0]
        # ldrh Rt, [Rn, #imm]  (Rt == Rn == Rd in our pattern)
        if (w2 & 0xfff0_00f0) != 0xe1d0_00b0:
            continue
        if ((w2 >> 12) & 0xf) != Rd or ((w2 >> 16) & 0xf) != Rd:
            continue
        ldrh_imm = ((w2 >> 4) & 0xf0) | (w2 & 0xf)
        w3 = struct.unpack_from('<I', data, text_off + off + 12)[0]
        # add pc, pc, Rm  (Rm == Rd in our pattern)
        if (w3 & 0xffff_fff0) != 0xe08f_f000:
            continue
        if (w3 & 0xf) != Rd:
            continue
        site_va = text_va + off
        # add Rd, Rd, pc: pc when add executes is site_va + 4 + 8
        # ldrh Rd, [Rd, #imm] reads from (site_va + 4 + 8) + imm
        table_va = site_va + 4 + 8 + ldrh_imm
        base_for_target = site_va + 12 + 8
        if (table_va, base_for_target) in seen:
            continue
        seen.add((table_va, base_for_target))
        # Build a minimal JT tuple for the emitter
        class _Inst: pass
        a = _Inst(); a.address = site_va
        jt = (a, None, None, None, table_va, base_for_target)
        print(f'  dispatch site @ 0x{site_va:08x} (Rsel=r{(w0>>16)&0xf}, table @ 0x{table_va:08x}):')
        emit_jump_table(md, data, rva_to_off, image_base, jt, args.max_table)

    if not chain and not seen:
        print()
        print('-- raw disasm (no recognized dispatch pattern) --')
        for inst in list(md.disasm(code, ioc_va))[:40]:
            print(f'  0x{inst.address:08x}  {inst.mnemonic} {inst.op_str}')

    return 0


if __name__ == '__main__':
    sys.exit(main())
