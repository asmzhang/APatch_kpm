#ifndef MEMCRC_COMPAT_H
#define MEMCRC_COMPAT_H

#include <hook.h>

/*
 * 上游 memcrc 面向的是旧版 KernelPatch，那时 hook_err_t 里有一个
 * "未挂钩"哨兵 HOOK_NOT_HOOK。现行 KP 的 hook_err_t 只有 HOOK_NO_ERR = 0
 * 与若干错误码（HOOK_BAD_ADDRESS 4095 等），没有这个值。
 * 这里补一个不会与任何真实返回码冲突的哨兵，语义保持"当前没有装钩子"。
 */
#ifndef HOOK_NOT_HOOK
#define HOOK_NOT_HOOK ((hook_err_t)-1)
#endif

#endif
