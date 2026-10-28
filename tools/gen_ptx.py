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

# The modules are listed in src/kernels/ptx/modules.txt, the one list that the Makefile, the CUDA runtime and the simulated driver read too:
# name, kind, cfg, base, defines (see the comments of the file).
SOURCES = {'linear': ['pz_common.cl', 'pz_linear.cl'], 'kang': ['pz_common.cl', 'pz_kang.h', 'pz_kang.cl'], 'test': ['pz_common.cl', 'pz_test.cl']}


def read_modules():
    """[(name, sources, defines)] from modules.txt"""
    mods = []
    for line in open(os.path.join(OUT, 'modules.txt')):
        f = line.split()
        if not f or f[0].startswith('#'):
            continue
        if len(f) < 5 or f[1] not in SOURCES:
            raise SystemExit('modules.txt: cannot read the line: %s' % line.strip())
        mods.append((f[0], SOURCES[f[1]], [] if f[4:] == ['-'] else f[4:]))
    return mods


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
    modules = read_modules()
    for name, files, defines in modules:
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
        print('wrote %d modules to %s' % (len(modules), OUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
