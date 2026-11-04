/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* gpu_variant.c - the names, the text codes and the options of the OpenCL build of the GPU kernel variants; the layout of their code is in
 * gpu_variant.h. A new configuration of the kangaroo kernel is one row of KV_FORMS, a new width of the herd one name in KV_WIDE_NAMES. */
#include "gpu_variant.h"
#include <stdio.h>
#include <string.h>

/* The configurations of the kangaroo kernel (KV_CFG), the row being the number: the word that the name of a variant has for it, the suffix of
 * PZ_GPU_NK that selects it (0: none, the plain one) and the option that the OpenCL build takes for it. */
static const struct { const char *word; char suffix; const char *cl_option; } KV_FORMS[] = {
    {"", 0, ""},
    {" pairs", 'b', " -DMC=2"},
    {" quads", 'c', " -DMC=4"},
    {" block herd", 'g', " -DKANG_BLOCK=1"},
};
#define KV_NFORMS ((int)(sizeof KV_FORMS / sizeof *KV_FORMS))

/* What the name of a variant says of a herd wider than the default one (KV_WIDE), the row being the number; there is no suffix for them. */
static const char *const KV_WIDE_NAMES[] = {"", ", 4x herd", ", 8x herd", ", full herd"};

#define KV_META_WORD ", tags once"
#define KV_META_SUFFIX 'm'
#define KV_META_CL_OPTION " -DKANG_META=1"
#define KV_L2_WORD " L2 herd"
#define KV_L2_SUFFIX 'l'

char *gpu_variant_name(int v, char *b, size_t n)
{
    snprintf(b, n, "%d%s", VAR_HALF(v), VAR_MINB(v) ? ", register cap" : "");
    return b;
}

char *gpu_kvariant_name(int v, char *b, size_t n)
{
    snprintf(b, n, "NK %d%s%s%s%s%s", KV_NK(v), KV_FORMS[KV_CFG(v) % KV_NFORMS].word, KV_META(v) ? KV_META_WORD : "",
             KV_L2(v) && (KV_CFG(v) || KV_META(v)) ? "," : "", KV_L2(v) ? KV_L2_WORD : "", KV_WIDE_NAMES[KV_WIDE(v)]);
    return b;
}

char *gpu_variant_code(int v, char *b, size_t n)
{
    snprintf(b, n, "%d", v);
    return b;
}

char *gpu_kvariant_code(int v, char *b, size_t n)
{
    if (KV_WIDE(v)) { snprintf(b, n, "-"); return b; }
    const char suffix[4] = {KV_FORMS[KV_CFG(v) % KV_NFORMS].suffix, KV_META(v) ? KV_META_SUFFIX : 0, KV_L2(v) ? KV_L2_SUFFIX : 0, 0};
    char text[4];
    int k = 0;
    for (int i = 0; i < 3; i++) if (suffix[i]) text[k++] = suffix[i];
    text[k] = 0;
    snprintf(b, n, "%d%s", KV_NK(v), text);
    return b;
}

bool gpu_kvariant_parse(const char *text, int *code)
{
    long nk = 0;
    const char *e = text;
    while (*e >= '0' && *e <= '9' && nk <= 0xFFFF) nk = nk * 10 + (*e++ - '0');   /* no strtol: glibc >= 2.38 would be needed */
    if (nk <= 0 || nk > 0xFFFF) return false;
    int cfg = 0;
    for (int c = 1; c < KV_NFORMS; c++)
        if (*e == KV_FORMS[c].suffix) { cfg = c; e++; break; }
    int meta = 0, l2 = 0;
    if (*e == KV_META_SUFFIX) { meta = 1; e++; }
    if (*e == KV_L2_SUFFIX) { l2 = 1; e++; }
    if (*e) return false;
    *code = KV_MAKE((int)nk, cfg, meta, l2, 0);
    return true;
}

const char *gpu_kvariant_cl_config(int cfg) { return cfg >= 0 && cfg < KV_NFORMS ? KV_FORMS[cfg].cl_option : ""; }
const char *gpu_kvariant_cl_meta(int meta) { return meta ? KV_META_CL_OPTION : ""; }
