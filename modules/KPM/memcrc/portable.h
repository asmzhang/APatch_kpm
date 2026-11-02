#ifndef MEMCRC_PORTABLE_H
#define MEMCRC_PORTABLE_H

#include <ktypes.h>
#include <hook.h>

struct task_struct;

/*
 * 可移植层（0.2.3）
 * -----------------
 * KernelPatch 有两种装载环境：内核集成形态（kernel）与 LKM 形态（kernelpatch-lkm）。
 * 两者的符号面不同（见 docs/process/09-load-forms.md），LKM 兼容表里没有：
 *   - kf_strnstr / kf_strpbrk / kf_strsep / kf_strspn（<linux/string.h> 的 inline 转 kf_* 调用）
 *   - fp_wrap_syscalln / inline_wrap_syscalln（及其 unhook 对应物）
 *   - cred_offset（结构体数据符号）
 * 这里把它们换成两形态都有的等价物：手写字符串函数、syscalln_addr + hook_wrap、
 * current_uid 与「从 init_cred 结构推导 uid 偏移」。
 */

char *mc_strnstr(const char *s, const char *needle, size_t len);
size_t mc_strspn(const char *s, const char *accept);
char *mc_strpbrk(const char *s, const char *accept);
char *mc_strsep(char **sp, const char *delim);

uid_t mc_current_uid(void);
uid_t mc_task_uid(struct task_struct *task);

hook_err_t mc_hook_syscall(int nr, int narg, void *before, void *after, void *udata);
void mc_unhook_syscall(int nr, void *before, void *after);

#endif
