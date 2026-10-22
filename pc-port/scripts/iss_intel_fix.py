#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guarded compatibility patch for the supported inFAMOUS Second Son eboot.

The linker calls :func:`patch_linked_image` on the generated ISSPRB02 image. It
rewrites the 96 register-form SSE4A EXTRQ instructions and relocates one known
leaf function away from the SysV red zone that Windows exception dispatch can
clobber. Exact-layout checks fail closed if the image has changed.

For standalone verification, run: py -3.12 iss_intel_fix.py <input> <output>
Dependency: py -3.12 -m pip install iced-x86
"""
import hashlib
import struct
import sys
from pathlib import Path
from iced_x86 import (
    BlockEncoder, Code, Decoder, FlowControl, Instruction, MemoryOperand,
    Mnemonic, OpKind, Register, RegisterExt,
)

IMAGE_SIZE = 0x823854D
TEXT_END = 0xCC55B8
SSE4A_CAVE, SSE4A_CAVE_END = 0xCBF000, 0xCC0600
RZ_FUNC, RZ_END = 0x1CF4E0, 0x1CFB50
RZ_TABLES = {0x99C018: 6, 0x99C030: 6}
RZ_CAVE = 0xCC0600
EXPECTED_SITE = 0x8938F1
EXPECTED_EXTRQ_COUNT = 96


def rex(w, r):
    return bytes([0x40 | (8 if w else 0) | (4 if r else 0)])


def extrq_helper(dst, src):
    """EXTRQ xmmD,xmmS: dst[63:0]=(dst>>index)&mask(length); dst[127:64]=0."""
    return (
        b'\x50\x51\x52'  # push rax/rcx/rdx
        + b'\x66' + rex(1, dst >= 8) + b'\x0f\x7e' + bytes([0xC0 | (dst & 7) << 3])
        + b'\x66' + rex(1, src >= 8) + b'\x0f\x7e' + bytes([0xC1 | (src & 7) << 3])
        + b'\x0f\xb6\xd5'                    # movzx edx,ch (index = control[13:8])
        + b'\xc4\xe2\xeb\xf7\xc0'            # shrx rax,rax,rdx
        + b'\xf7\xd1'                          # not ecx
        + b'\x8d\x49\x01'                    # lea ecx,[rcx+1] (-length mod 64)
        + b'\xc4\xe2\xf1\xf7\xc0'            # shlx rax,rax,rcx
        + b'\xc4\xe2\xf3\xf7\xc0'            # shrx rax,rax,rcx
        + b'\x66' + rex(1, dst >= 8) + b'\x0f\x6e' + bytes([0xC0 | (dst & 7) << 3])
        + b'\x5a\x59\x58\xc3'                # pop rdx/rcx/rax; ret
    )


def xmm(reg):
    return RegisterExt.number(reg) - RegisterExt.number(Register.XMM0)


def patch_sse4a(img):
    text = bytes(img[:TEXT_END])
    targets, sites, last = set(), [], None
    for ins in Decoder(64, text, ip=0):
        if ins.op0_kind == OpKind.NEAR_BRANCH64:
            targets.add(ins.near_branch_target)
        if last is not None:
            site, last = last, None
            plain = (
                ins.flow_control == FlowControl.NEXT
                and not ins.is_ip_rel_memory_operand
                and ins.mnemonic not in (Mnemonic.EXTRQ, Mnemonic.INSERTQ)
                and ins.segment_prefix == Register.NONE
            )
            site['follower'] = ins if plain else None
        if ins.mnemonic == Mnemonic.INSERTQ or (
            ins.mnemonic == Mnemonic.EXTRQ and ins.code != Code.EXTRQ_XMM_XMM
        ):
            raise ValueError(f'unexpected SSE4A form at eboot+0x{ins.ip:x}')
        if ins.mnemonic == Mnemonic.EXTRQ:
            sites.append({'ip': ins.ip, 'len': ins.len,
                          'dst': xmm(ins.op0_register), 'src': xmm(ins.op1_register)})
            if ins.len < 5:
                last = sites[-1]

    if len(sites) != EXPECTED_EXTRQ_COUNT:
        raise ValueError(f'expected {EXPECTED_EXTRQ_COUNT} EXTRQ sites; found {len(sites)}; refusing to patch')
    expected = next((s for s in sites if s['ip'] == EXPECTED_SITE), None)
    if expected is None or (expected['dst'], expected['src']) != (4, 7):
        raise ValueError('eboot+0x8938f1 is not the expected EXTRQ xmm4,xmm7; refusing to patch')

    pos, helpers = SSE4A_CAVE, {}
    for pair in sorted({(s['dst'], s['src']) for s in sites}):
        code = extrq_helper(*pair)
        helpers[pair] = pos
        img[pos:pos + len(code)] = code
        pos = (pos + len(code) + 15) & ~15

    for s in sites:
        relocated = b''
        if s['len'] < 5:
            follower = s.get('follower')
            if follower is None or follower.ip in targets:
                raise ValueError(f'EXTRQ site eboot+0x{s["ip"]:x}: no safe relocatable follower')
            relocated = text[follower.ip:follower.next_ip]
        stub = pos
        back = s['ip'] + s['len'] + len(relocated)
        code = b'\x48\x8d\x64\x24\x80'  # lea rsp,[rsp-0x80] to protect SysV red zone
        code += b'\xe8' + struct.pack('<i', helpers[(s['dst'], s['src'])] - (stub + len(code) + 5))
        code += b'\x48\x8d\xa4\x24\x80\x00\x00\x00'  # restore rsp
        code += relocated
        code += b'\xe9' + struct.pack('<i', back - (stub + len(code) + 5))
        img[stub:stub + len(code)] = code
        pos = (pos + len(code) + 15) & ~15
        covered = back - s['ip']
        img[s['ip']:back] = b'\xe9' + struct.pack('<i', stub - (s['ip'] + 5)) + b'\xcc' * (covered - 5)

    if pos > SSE4A_CAVE_END:
        raise ValueError('SSE4A stubs exceeded their reserved code cave')
    return len(sites)


def patch_red_zone(img):
    insns = list(Decoder(64, bytes(img[RZ_FUNC:RZ_END]), ip=RZ_FUNC))
    returns = [k for k, ins in enumerate(insns) if ins.code == Code.RETNQ]
    if not returns:
        raise ValueError('red-zone function has no expected RET; refusing to patch')
    insns = insns[:max(returns) + 1]
    if sum(ins.flow_control == FlowControl.RETURN for ins in insns) != 1:
        raise ValueError('red-zone function has unexpected return structure')
    if any(ins.flow_control in (FlowControl.CALL, FlowControl.INDIRECT_CALL) for ins in insns):
        raise ValueError('red-zone function contains a call; refusing to relocate')
    if not all(RZ_FUNC <= ins.near_branch_target < RZ_END
               for ins in insns if ins.op0_kind == OpKind.NEAR_BRANCH64):
        raise ValueError('red-zone function has an out-of-range branch; refusing to relocate')

    tables, pos = {}, RZ_CAVE
    for table, count in RZ_TABLES.items():
        tables[table], pos = pos, pos + count * 4
    code_at = (pos + 15) & ~15
    fake = 0x7F000000
    down = Instruction.create_reg_mem(
        Code.LEA_R64_M, Register.RSP,
        MemoryOperand(Register.RSP, displ=-0x80, displ_size=1),
    )
    down.ip = RZ_FUNC
    out, origin = [down], [None]
    for ins in insns:
        ip = ins.ip
        if ins.memory_base == Register.RSP:
            disp = ins.memory_displacement
            disp = (disp - (1 << 64) if disp >= (1 << 63) else disp) + 0x80
            ins.memory_displacement = disp & ((1 << 64) - 1)
            ins.memory_displ_size = 1 if -128 <= disp <= 127 else 8
        elif ins.is_ip_rel_memory_operand and ins.ip_rel_memory_address in RZ_TABLES:
            ins.memory_displacement = tables[ins.ip_rel_memory_address]
        if ip == RZ_FUNC:
            ins.ip = fake
        if ins.code == Code.RETNQ:
            up = Instruction.create_reg_mem(
                Code.LEA_R64_M, Register.RSP,
                MemoryOperand(Register.RSP, displ=0x80, displ_size=8),
            )
            up.ip, ins.ip = ip, fake + 1
            out += [up, ins]
            origin += [ip, None]
            continue
        out.append(ins)
        origin.append(ip)

    encoder = BlockEncoder(64)
    encoder.add_many(out)
    code = encoder.encode(code_at)
    new = list(Decoder(64, code, ip=code_at))
    if len(new) != len(out):
        raise ValueError('relocated red-zone function changed instruction count')
    newip = {old: dec.ip for old, dec in zip(origin, new) if old is not None}
    for table, count in RZ_TABLES.items():
        old_targets = [table + e for e in struct.unpack_from(f'<{count}i', img, table)]
        img[tables[table]:tables[table] + count * 4] = struct.pack(
            f'<{count}i', *[newip[target] - tables[table] for target in old_targets]
        )
    img[code_at:code_at + len(code)] = code
    img[RZ_FUNC:RZ_FUNC + 5] = b'\xe9' + struct.pack('<i', code_at - (RZ_FUNC + 5))
    if code_at + len(code) > TEXT_END:
        raise ValueError('relocated red-zone function exceeded text segment')
    return code_at


def patch_linked_image(source):
    """Return ``(patched_image, report)`` for the supported linked image.

    ``source`` is the exact ISSPRB02 byte stream produced by ``link_modules.py``.
    The input object is never mutated. Callers must validate the source eboot
    identity before applying this exact-layout patch; link_modules.py does so.
    """
    data = bytearray(source)
    if len(data) < 16 or data[:8] != b'ISSPRB02':
        raise ValueError('not an ISSPRB02 linked image; refusing to patch')
    declared_size = struct.unpack_from('<Q', data, 8)[0]
    if declared_size != IMAGE_SIZE:
        raise ValueError(
            f'unexpected linked image size 0x{declared_size:x}; expected 0x{IMAGE_SIZE:x}; refusing to patch'
        )
    if len(data) < IMAGE_SIZE:
        raise ValueError('linked image is truncated')

    base = len(data) - IMAGE_SIZE
    img = bytearray(data[base:])
    if any(img[0xCBEAD0:TEXT_END]):
        raise ValueError('expected code-cave padding is not empty; refusing to patch')

    extrq_count = patch_sse4a(img)
    red_zone_target = patch_red_zone(img)
    data[base:] = img
    return bytes(data), {
        'sse4a_extrq_sites_rewritten': extrq_count,
        'red_zone_function_offset': hex(RZ_FUNC),
        'red_zone_function_relocated_to': hex(red_zone_target),
        'method': 'guarded exact-layout binary compatibility patches',
    }


def main():
    if len(sys.argv) != 3:
        raise SystemExit('Usage: py -3.12 iss_intel_fix.py <boot-linked.bin> <output.bin>')
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    if src.resolve() == dst.resolve():
        raise SystemExit('Input and output must be different files; original will not be overwritten')
    try:
        patched, report = patch_linked_image(src.read_bytes())
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(patched)
    print(f"{report['sse4a_extrq_sites_rewritten']} EXTRQ sites rewritten; "
          f"red-zone function +0x{RZ_FUNC:x} relocated to +{report['red_zone_function_relocated_to']}.")
    print(f'Original preserved: {src}')
    print(f'Patched output:     {dst}')
    print(f'Patched SHA-256:    {hashlib.sha256(patched).hexdigest()}')


if __name__ == '__main__':
    main()
