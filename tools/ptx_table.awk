# SPDX-License-Identifier: AGPL-3.0-or-later
# ptx_table.awk - reads src/kernels/ptx/modules.txt, the list of the CUDA modules, for the Makefile. Run as
#   awk -f tools/ptx_table.awk -v mode=MODE [-v cc=COMPILER -v dir=DIR] src/kernels/ptx/modules.txt
# and prints, by MODE:
#   names     the names of the modules, in the order of the file
#   bases     name:base for each module that has a base
#   rtcu      the C source of build/ptx_mods.c: which module an rt_build(program, parameter) call of the CUDA runtime loads (src/rt_priv.h)
#   mockhdr   tests/mock_cuda: the header with the modules the simulated driver knows and the kernel it runs for each
#   mockcc    tests/mock_cuda: the commands (with COMPILER, objects in DIR) that compile the kernels the simulated driver runs
#   mockobjs  tests/mock_cuda: the objects those commands make
# Only POSIX awk is used. A line of the file: name kind cfg base defines...
function size_of(    i, v) {
    for (i = 5; i <= NF; i++)
        if ($i ~ /^-D(HALF|NK)=[0-9]+$/) { v = $i; sub(/^[^=]*=/, "", v); return v }
    return 0
}
function param_of(    s) {
    s = size_of()
    return $3 + 0 ? s " | " $3 " << 16" : s
}
# the -D options the kernel emulation takes: the size and the batch products (not the register cap, which is for the compiler of the GPU)
function emul_defines(    i, d) {
    d = ""
    for (i = 5; i <= NF; i++)
        if ($i ~ /^-D(HALF|NK|MC)=[0-9]+$/) d = d " " $i
    return d
}
function linear_obj() { return dir "/linear" size_of() ".o" }
/^[ \t]*(#|$)/ { next }
{
    n++
    name[n] = $1; kind[n] = $2; base[n] = $4
    p[n] = $2 == "test" ? "0" : param_of()
    sz[n] = size_of()
    ed[n] = emul_defines()
}
END {
    if (mode == "names") {
        for (i = 1; i <= n; i++) printf "%s%s", (i > 1 ? " " : ""), name[i]
        printf "\n"
    } else if (mode == "bases") {
        k = 0
        for (i = 1; i <= n; i++) if (base[i] != "-") printf "%s%s:%s", (k++ ? " " : ""), name[i], base[i]
        printf "\n"
    } else if (mode == "rtcu") {
        print "/* Generated from src/kernels/ptx/modules.txt by tools/ptx_table.awk: do not edit. */"
        print "#include \"rt_priv.h\""
        print ""
        for (i = 1; i <= n; i++) printf "extern const pz_blob pz_ptx_%s;\n", name[i]
        print ""
        print "const rtcu_module rtcu_modules[] = {"
        for (i = 1; i <= n; i++)
            printf "    {RT_PROG_%s, %s, &pz_ptx_%s},\n", toupper(kind[i]), p[i], name[i]
        print "};"
        printf "const int rtcu_module_count = %d;\n", n
    } else if (mode == "mockhdr") {
        print "/* Generated from src/kernels/ptx/modules.txt by tools/ptx_table.awk: do not edit. */"
        for (i = 1; i <= n; i++) printf "extern const unsigned char mock_ptx_%s[];\n", name[i]
        for (i = 1; i <= n; i++) {
            if (kind[i] == "linear" && !(("L" sz[i]) in seen)) { seen["L" sz[i]] = 1; printf "DECL_SEARCH(%s)\n", sz[i] }
            if (kind[i] == "kang") printf "DECL_KANG(%s)\n", name[i]
        }
        print "static const struct { const unsigned char *ptx; int kind; search_fn search; kang_fn kang; } MODS[] = {"
        for (i = 1; i <= n; i++) {
            if (kind[i] == "linear") printf "    {mock_ptx_%s, M_LINEAR, mock_search%s, NULL},\n", name[i], sz[i]
            else if (kind[i] == "kang") printf "    {mock_ptx_%s, M_KANG, NULL, mock_%s},\n", name[i], name[i]
            else printf "    {mock_ptx_%s, M_TEST, NULL, NULL},\n", name[i]
        }
        print "};"
    } else if (mode == "mockcc" || mode == "mockobjs") {
        for (i = 1; i <= n; i++) {
            if (kind[i] == "linear") {
                if (("L" sz[i]) in done) continue
                done["L" sz[i]] = 1
                o = dir "/linear" sz[i] ".o"
                cmd = cc " -c tests/mock_cuda/kern.c -DMOCK_LINEAR" ed[i] " -Dpz_search=mock_search" sz[i] " -o " o ";"
            } else if (kind[i] == "kang") {
                o = dir "/" name[i] ".o"
                cmd = cc " -c tests/mock_cuda/kern.c -DMOCK_KANG" ed[i] " -Dpz_kang_step=mock_" name[i] " -o " o ";"
            } else continue
            if (mode == "mockcc") print cmd
            else printf "%s ", o
        }
        if (mode == "mockobjs") printf "\n"
    } else {
        print "ptx_table.awk: -v mode=names|bases|rtcu|mockhdr|mockcc|mockobjs" > "/dev/stderr"
        exit 1
    }
}
