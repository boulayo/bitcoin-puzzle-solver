/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bsgs_x86.c - baby/giant group routine compiled with BMI2/ADX (see Makefile). */
#include "bsgs_priv.h"
#define BS_GROUP bs_group_x86
#include "bsgs_isa.inc"
