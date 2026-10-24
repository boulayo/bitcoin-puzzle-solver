/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_x86.c - kangaroo round compiled with BMI2/ADX (see Makefile). */
#include "kangaroo_priv.h"
#define KROUND kang_round_x86
#include "kang_isa.inc"
