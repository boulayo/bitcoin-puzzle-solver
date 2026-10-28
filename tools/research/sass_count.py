#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Counts the SASS instructions (sm_86) that NVIDIA's compilers produce for kernels of the CUDA dialect of src/kernels.

    tools/research/sass_count.py --nvrtc LIBNVRTC --ptxas PTXAS [--nvdisasm NVDISASM] [--hist KERNEL] [-DNAME ...] FILE...

The sources are concatenated after pz_cuda.h and pz_common.cl (as tools/gen_ptx.py does), compiled to PTX with NVRTC
(compute_52, -DPZ_FAST_FE), assembled with ptxas for sm_86, and the number of instructions of every kernel is the size of its
.text section divided by 16. With --nvdisasm and --hist KERNEL the opcode histogram of that kernel is printed (compile it alone:
nvdisasm lists every kernel of the file). The tools come from NVIDIA's pip wheels (nvidia-cuda-nvrtc-cu12, nvidia-cuda-nvcc-cu12:
see tools/README.md) and the CUDA 12 repository package cuda-nvdisasm-12-0.

Used for docs/en/ALGORITHMS.md section 8.6: tools/research/sass_count.py ... tools/research/sass/kara.h tools/research/sass/chain.h tools/research/sass/variants.cl"""
import collections, ctypes, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KDIR = os.path.join(ROOT, 'src', 'kernels')


def compile_ptx(nvrtc, files, defines):
    lib = ctypes.CDLL(nvrtc)
    src = open(os.path.join(KDIR, 'pz_cuda.h')).read()
    for f in ['pz_common.cl'] + files:
        path = os.path.abspath(f) if os.path.exists(f) else os.path.join(KDIR, f)
        src += '\n#line 1 "%s"\n' % os.path.basename(path) + open(path).read()
    prog = ctypes.c_void_p()
    lib.nvrtcCreateProgram(ctypes.byref(prog), src.encode(), b'sass_count.cu', 0, None, None)
    opts = ['--gpu-architecture=compute_52', '-default-device', '--std=c++14', '-DPZ_BLOCK=128', '-DPZ_FAST_FE'] + defines
    arr = (ctypes.c_char_p * len(opts))(*[o.encode() for o in opts])
    rc = lib.nvrtcCompileProgram(prog, len(opts), arr)
    n = ctypes.c_size_t()
    lib.nvrtcGetProgramLogSize(prog, ctypes.byref(n))
    log = ctypes.create_string_buffer(n.value)
    lib.nvrtcGetProgramLog(prog, log)
    if rc:
        raise SystemExit('NVRTC:\n' + log.value.decode(errors='replace'))
    lib.nvrtcGetPTXSize(prog, ctypes.byref(n))
    ptx = ctypes.create_string_buffer(n.value)
    lib.nvrtcGetPTX(prog, ptx)
    return ptx.value.decode()


def main():
    args = sys.argv[1:]
    opt = lambda name: args[args.index(name) + 1] if name in args else None
    nvrtc, ptxas, nvdisasm, hist = opt('--nvrtc'), opt('--ptxas'), opt('--nvdisasm'), opt('--hist')
    skip = {'--nvrtc', '--ptxas', '--nvdisasm', '--hist'}
    rest = [a for i, a in enumerate(args) if a not in skip and (i == 0 or args[i - 1] not in skip)]
    defines = [a for a in rest if a.startswith('-D')]
    files = [a for a in rest if not a.startswith('-D')]
    if not (nvrtc and ptxas and files):
        raise SystemExit(__doc__)
    ptx = compile_ptx(nvrtc, files, defines)
    with tempfile.TemporaryDirectory() as d:
        open(d + '/k.ptx', 'w').write(ptx)
        r = subprocess.run([ptxas, '-arch=sm_86', '-v', '-o', d + '/k.cubin', d + '/k.ptx'], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('ptxas:\n' + r.stderr)
        regs, cur = {}, None
        for line in r.stderr.splitlines():
            m = re.search(r"Compiling entry function '(\w+)'", line)
            if m:
                cur = m.group(1)
            m = re.search(r'Used (\d+) registers', line)
            if m and cur:
                regs[cur] = int(m.group(1))
        elf = subprocess.run(['readelf', '-S', '-W', d + '/k.cubin'], capture_output=True, text=True).stdout
        for line in elf.splitlines():
            m = re.search(r'\.text\.(\w+)\s+PROGBITS\s+\w+\s+\w+\s+(\w+)', line)
            if m:
                print('%-22s %6d instructions  %s registers' % (m.group(1), int(m.group(2), 16) // 16, regs.get(m.group(1), '?')))
        if hist and nvdisasm:
            txt = subprocess.run([nvdisasm, '-c', d + '/k.cubin'], capture_output=True, text=True).stdout
            h = collections.Counter()
            for line in txt.splitlines():
                m = re.match(r'\s+/\*[0-9a-f]+\*/\s+(@!?U?P\w+\s+)?([A-Z0-9_]+(\.[A-Z0-9_.]+)?)', line)
                if m:
                    h[m.group(2)] += 1
            print('\nopcodes of the file (%d instructions):' % sum(h.values()))
            for op, c in h.most_common(25):
                print('  %-22s %d' % (op, c))


if __name__ == '__main__':
    main()
