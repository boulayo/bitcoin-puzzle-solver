/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* encrypt_cmd.h - the commands encrypt-keygen and decrypt-key (argv[0] is the first argument after the command name). */
#ifndef PZ_ENCRYPT_CMD_H
#define PZ_ENCRYPT_CMD_H

int encrypt_keygen_main(int argc, char **argv);
int decrypt_key_main(int argc, char **argv);

#endif
