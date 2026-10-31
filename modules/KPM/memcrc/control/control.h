#ifndef MEMORY_CRC_CONTROL_H
#define MEMORY_CRC_CONTROL_H

#include <ktypes.h>

long syscall_init();
long syscall_exit();

/* ALLOW:/DENY:/ALLOWLIST 三个命令；返回 0 表示不是本模块的命令 */
int control_handle_command(const char *cmd, char *out, unsigned long out_size);

int control_allow_uid(uid_t uid);
int control_deny_uid(uid_t uid);
int control_dump_allow(char *buf, unsigned long size);

#endif
