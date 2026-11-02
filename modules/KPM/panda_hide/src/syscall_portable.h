#ifndef __PANDA_SYSCALL_PORTABLE_H__
#define __PANDA_SYSCALL_PORTABLE_H__

#include <ktypes.h>
#include <hook.h>

/* syscall_portable.c：两种装载形态（kernel / lkm）都成立的 syscall 挂钩。 */
uintptr_t panda_syscall_addr(const char *name);
hook_err_t panda_hook_syscall(const char *name, int narg, void *before, void *after, void *udata);
void panda_unhook_syscall(const char *name, void *before, void *after);

#endif
