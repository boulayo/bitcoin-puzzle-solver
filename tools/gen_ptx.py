#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generates the CUDA PTX modules embedded in pzsolve (src/kernels/ptx/*.ptx) from the OpenCL C kernels.

The kernels are compiled as CUDA C++ by NVRTC (src/kernels/pz_cuda.h maps the OpenCL dialect). The output is PTX
for a low virtual architecture, so the NVIDIA driver can JIT it for any GPU from Maxwell on; the driver caches the
result, so the cost is paid once per driver/GPU. The .ptx files are committed: building the program needs neither
NVRTC nor the CUDA toolkit.

    python3 -I tools/gen_ptx.py --nvrtc /path/to/libnvrtc.so.12 [--ptxas /path/to/ptxas]   # regenerate
    python3 -I tools/gen_ptx.py --nvrtc ... --check                                         # fail if stale

NVRTC comes from the 'nvidia-cuda-nvrtc-cu12' wheel (pip download), ptxas (optional, for a register/spill report
on sm_86) from 'nvidia-cuda-nvcc-cu12'. The NVRTC version fixes the PTX ISA version and therefore the oldest driver
that can load the modules: NVRTC 12.0 -> PTX 8.0 -> driver >= 525.
"""
import ctypes, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KDIR = os.path.join(ROOT, 'src', 'kernels')
OUT = os.path.join(KDIR, 'ptx')
ARCH = 'compute_52'   # Maxwell and newer
BLOCK = 128           # threads per block the host launches with (must match RT_CUDA_BLOCK in rt_cuda.c)

MODULES = [
    # name, sources, defines
    ('linear128', ['pz_common.cl', 'pz_linear.cl'], ['-DHALF=128']),
    ('linear256', ['pz_common.cl', 'pz_linear.cl'], ['-DHALF=256']),
    ('linear512', ['pz_common.cl', 'pz_linear.cl'], ['-DHALF=512']),
    # at least 4 resident blocks per SM: the compiler caps the kernel at 128 registers (a few spills, more warps)
    ('linear128m4', ['pz_common.cl', 'pz_linear.cl'], ['-DHALF=128', '-DPZ_MINB=4']),
    ('linear256m4', ['pz_common.cl', 'pz_linear.cl'], ['-DHALF=256', '-DPZ_MINB=4']),
    ('linear512m4', ['pz_common.cl', 'pz_linear.cl'], ['-DHALF=512', '-DPZ_MINB=4']),
    # kangaroos per work-item (one field inversion per NK steps); capped at 128 registers (four blocks per SM: 512 threads;
    # without the cap ptxas uses 150-220 registers and only two blocks fit)
    ('kang64', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=64', '-DPZ_MINB=4']),
    ('kang128', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=128', '-DPZ_MINB=4']),
    ('kang256', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=256', '-DPZ_MINB=4']),
    # the same with the batch products kept only every 2 walkers (less memory traffic, half a product more per step)
    ('kang64b', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=64', '-DMC=2', '-DPZ_MINB=4']),
    ('kang128b', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=128', '-DMC=2', '-DPZ_MINB=4']),
    ('kang256b', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=256', '-DMC=2', '-DPZ_MINB=4']),
    # batch products kept only for every 4th walker (about 16 bytes less traffic per step): needs more registers than fit in 128
    # (about 212 without spills), so only two blocks of 128 threads stay resident per SM; the tuner keeps it only where it is faster
    ('kang64c', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=64', '-DMC=4', '-DPZ_MINB=2']),
    ('kang128c', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=128', '-DMC=4', '-DPZ_MINB=2']),
    # smaller herds per work-item (16, 32), for GPUs whose L2 cache can hold the whole herd (the tuner offers them only there)
    ('kang16', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=16', '-DPZ_MINB=4']),
    ('kang32', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=32', '-DPZ_MINB=4']),
    ('kang16b', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=16', '-DMC=2', '-DPZ_MINB=4']),
    ('kang32b', ['pz_common.cl', 'pz_kang.cl'], ['-DNK=32', '-DMC=2', '-DPZ_MINB=4']),
    ('test', ['pz_common.cl', 'pz_test.cl'], []),
]


def compile_one(lib, name, files, defines):
    src = open(os.path.join(KDIR, 'pz_cuda.h')).read()
    for f in files:
        src += '\n#line 1 "%s"\n' % f + open(os.path.join(KDIR, f)).read()
    prog = ctypes.c_void_p()
    rc = lib.nvrtcCreateProgram(ctypes.byref(prog), src.encode(), (name + '.cu').encode(), 0, None, None)
    if rc:
        raise SystemExit('nvrtcCreateProgram: %d' % rc)
    opts = ['--gpu-architecture=' + ARCH, '-default-device', '--std=c++14', '-DPZ_BLOCK=%d' % BLOCK, '-DPZ_FAST_FE'] + defines
    arr = (ctypes.c_char_p * len(opts))(*[o.encode() for o in opts])
    rc = lib.nvrtcCompileProgram(prog, len(opts), arr)
    logsz = ctypes.c_size_t()
    lib.nvrtcGetProgramLogSize(prog, ctypes.byref(logsz))
    log = ctypes.create_string_buffer(logsz.value)
    lib.nvrtcGetProgramLog(prog, log)
    if rc:
        raise SystemExit('NVRTC failed on %s:\n%s' % (name, log.value.decode(errors='replace')))
    if log.value.strip():
        print('  [%s] %s' % (name, log.value.decode(errors='replace').strip()))
    sz = ctypes.c_size_t()
    lib.nvrtcGetPTXSize(prog, ctypes.byref(sz))
    ptx = ctypes.create_string_buffer(sz.value)
    lib.nvrtcGetPTX(prog, ptx)
    lib.nvrtcDestroyProgram(ctypes.byref(prog))
    return ptx.value.decode()


def ptxas_report(ptxas, name, ptx):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, name + '.ptx')
        open(p, 'w').write(ptx)
        r = subprocess.run([ptxas, '-arch=sm_86', '-v', '-o', os.path.join(d, 'o.cubin'), p], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('ptxas rejected %s:\n%s' % (name, r.stderr))
        for line in r.stderr.splitlines():
            if 'registers' in line or 'spill' in line:
                print('  [%s sm_86] %s' % (name, line.split('info    : ')[-1]))


def main():
    args = sys.argv[1:]
    nvrtc = args[args.index('--nvrtc') + 1] if '--nvrtc' in args else 'libnvrtc.so.12'
    ptxas = args[args.index('--ptxas') + 1] if '--ptxas' in args else None
    lib = ctypes.CDLL(nvrtc)
    major, minor = ctypes.c_int(), ctypes.c_int()
    lib.nvrtcVersion(ctypes.byref(major), ctypes.byref(minor))
    print('NVRTC %d.%d, target %s, %d threads per block' % (major.value, minor.value, ARCH, BLOCK))
    os.makedirs(OUT, exist_ok=True)
    stale = []
    for name, files, defines in MODULES:
        ptx = compile_one(lib, name, files, defines)
        if ptxas:
            ptxas_report(ptxas, name, ptx)
        path = os.path.join(OUT, name + '.ptx')
        if '--check' in args:
            if not os.path.exists(path) or open(path).read() != ptx:
                stale.append(name)
        else:
            open(path, 'w').write(ptx)
    if '--check' in args:
        if stale:
            print('PTX out of date: %s (run tools/gen_ptx.py)' % ', '.join(stale))
            return 1
        print('PTX in sync')
    else:
        print('wrote %d modules to %s' % (len(MODULES), OUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
