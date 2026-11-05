/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd_encrypt.h - the commands encrypt-keygen and decrypt-key (argv[0] is the first argument after the command name). */
#ifndef PZ_CMD_ENCRYPT_H
#define PZ_CMD_ENCRYPT_H

int encrypt_keygen_main(int argc, char **argv);
int decrypt_key_main(int argc, char **argv);

#endif
