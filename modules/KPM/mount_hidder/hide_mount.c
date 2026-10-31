/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 bmax121. All Rights Reserved.
 *
 * 上游：mount_hidder/hide_mount.c（110 行）。移植改动逐条列在
 * Developer/reports/2026-10-31-mount-hidder-port.md，摘要：
 *   1. 去掉 get_task_mm(current) 判空——上游每次 seq 输出都取一次 mm 引用
 *      却从不 mmput，读一次 /proc/self/mountinfo 就漏一个 mm 引用；
 *   2. 起始 count 用 count+1 存进 local.data0——上游直接存 count，count==0
 *      （缓冲区空、第一条挂载项）时 after 钩子把 data0 当"没记录"跳过，
 *      第一条永远过滤不掉；
 *   3. 去掉变长数组 char lstr[llen]，改为在 seq 缓冲区内用 strnstr 定长匹配，
 *      避免内核栈溢出；
 *   4. exit 补 return 0；卸载改用 hook_unwrap_remove(..., 1)；
 *   5. 路径表可由加载参数或 ctl0 维护，ctl0 同时打印状态。
 */

#include <log.h>
#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>
#include <hook.h>
#include <kallsyms.h>
#include <stdint.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <asm/current.h>
#include <linux/compiler.h>
#include "hide_mount.h"

KPM_NAME("mount_hidder");
KPM_VERSION(MYKPM_VERSION);
KPM_LICENSE("GPL v2");
KPM_AUTHOR("bmax121");
KPM_DESCRIPTION("hide mount entries from procfs (hide_mount upstream port)");

#define HM_BARRIER() asm volatile("dmb ish" ::: "memory")

/* 默认要隐藏的三条痕迹：Magisk 的 /debug_ramdisk、APatch/Magisk 的 dex2oat 重定向 */
static const char *hm_default_paths[] = {
    "/debug_ramdisk",
    "/apex/com.android.art/bin/dex2oat64",
    "/apex/com.android.art/bin/dex2oat32",
};

static void *hm_show_vfsmnt;
static void *hm_show_mountinfo;
static void *hm_show_vfsstat;

static char hm_paths[HM_MAX_PATHS][HM_MAX_PATH_LEN];
static int hm_path_count; /* 高水位：0..count-1 是槽位，槽内首字节为 0 表示空槽 */
static int hm_hook_count;

static int hm_add_path(const char *s, int len)
{
    int i, slot;

    if (!s || len <= 0 || len >= HM_MAX_PATH_LEN) return -1;
    if (s[0] != '/') return -1; /* 只接受绝对路径 */

    for (i = 0; i < hm_path_count; i++) {
        if (hm_paths[i][0] && strncmp(hm_paths[i], s, (size_t)len) == 0 && hm_paths[i][len] == '\0') {
            return 0; /* 已存在 */
        }
    }

    slot = -1;
    for (i = 0; i < hm_path_count; i++) {
        if (!hm_paths[i][0]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (hm_path_count >= HM_MAX_PATHS) return -1;
        slot = hm_path_count;
    }

    memcpy(hm_paths[slot], s, (size_t)len);
    hm_paths[slot][len] = '\0';

    /* 先写好整条字符串，再用屏障 + 更新 count 让钩子侧只看到完整数据 */
    HM_BARRIER();
    if (slot == hm_path_count) hm_path_count = slot + 1;

    return 0;
}

static int hm_del_path(const char *s, int len)
{
    int i;

    if (!s || len <= 0) return -1;

    for (i = 0; i < hm_path_count; i++) {
        if (hm_paths[i][0] && strncmp(hm_paths[i], s, (size_t)len) == 0 && hm_paths[i][len] == '\0') {
            hm_paths[i][0] = '\0';
            HM_BARRIER();
            return 0;
        }
    }
    return -1;
}

static void hm_clear_paths(void)
{
    int i;

    hm_path_count = 0;
    HM_BARRIER();
    for (i = 0; i < HM_MAX_PATHS; i++) hm_paths[i][0] = '\0';
    HM_BARRIER();
}

static int hm_add_cstr(const char *s)
{
    size_t len;

    if (!s) return -1;
    len = strlen(s);
    if (len == 0 || len >= HM_MAX_PATH_LEN) return -1;
    return hm_add_path(s, (int)len);
}

/* 加载参数：逗号 / 分号 / 空白分隔的绝对路径列表 */
static void hm_parse_args(const char *args)
{
    const char *p = args;

    if (!p) return;
    while (*p && hm_path_count < HM_MAX_PATHS) {
        const char *start;
        int len = 0;

        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;

        start = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') {
            p++;
            len++;
        }
        hm_add_path(start, len);
    }
}

static void hm_before(hook_fargs2_t *args, void *udata)
{
    struct seq_file *m;

    /* local 在 KP 的 transit 里是栈上未初始化内存，必须先写再读 */
    args->local.data0 = 0;

    m = (struct seq_file *)(uintptr_t)args->arg0;
    if (!m) return;

    args->local.data0 = (uint64_t)m->count + 1;
}

static void hm_after(hook_fargs2_t *args, void *udata)
{
    struct seq_file *m;
    size_t base, len;
    int i;

    if (!args->local.data0) return;
    if ((int32_t)args->ret) return; /* 原始 show 返回错误，缓冲区没有新增内容 */

    m = (struct seq_file *)(uintptr_t)args->arg0;
    if (!m || !m->buf) return;

    base = (size_t)(args->local.data0 - 1);
    if (m->count <= base) return;
    if (m->count > m->size) return; /* 缓冲计数异常，宁可不隐藏 */

    len = m->count - base;

    for (i = 0; i < hm_path_count; i++) {
        if (!hm_paths[i][0]) continue;
        const char *hit = strnstr(m->buf + base, hm_paths[i], len);
        size_t plen = kf_strlen(hm_paths[i]);

        /* AUD-b-8：mountinfo 里路径是空格分隔字段，要求前后都是字段边界，
         * 否则 "/data" 会误伤 "/data_mirror"。 */
        if (hit && (hit == m->buf + base || hit[-1] == ' ') &&
            (hit[plen] == ' ' || hit[plen] == '\0')) {
            m->count = base; /* 整条回滚，seq_read 会重新取下一个 */
            break;
        }
    }
}

static void hm_hook_one(void *addr)
{
    hook_err_t err;

    if (!addr) return;
    err = hook_wrap2(addr, hm_before, hm_after, 0);
    if (err == HOOK_NO_ERR) {
        hm_hook_count++;
    } else {
        logkd("hide_mount: hook_wrap2(%llx) failed: %d\n", (uint64_t)(uintptr_t)addr, (int)err);
    }
}

static void hm_unhook_one(void *addr)
{
    if (!addr) return;
    /* AUD-b-9：hook_unwrap_remove 返回 void（hook.h:319），无法判定失败，
     * 卸载是否干净由 load/unload 的 rc 观测。 */
    hook_unwrap_remove(addr, hm_before, hm_after, 1);
}

static long hide_mount_init(const char *args, const char *event, void *__user reserved)
{
    int i;

    hm_path_count = 0;
    hm_hook_count = 0;
    memset(hm_paths, 0, sizeof(hm_paths));

    hm_parse_args(args);

    if (hm_path_count == 0) {
        for (i = 0; i < (int)(sizeof(hm_default_paths) / sizeof(hm_default_paths[0])); i++) {
            hm_add_cstr(hm_default_paths[i]);
        }
    }

    hm_show_vfsmnt = (void *)(uintptr_t)kallsyms_lookup_name("show_vfsmnt");
    hm_show_mountinfo = (void *)(uintptr_t)kallsyms_lookup_name("show_mountinfo");
    hm_show_vfsstat = (void *)(uintptr_t)kallsyms_lookup_name("show_vfsstat");

    hm_hook_one(hm_show_vfsmnt);
    hm_hook_one(hm_show_mountinfo);
    hm_hook_one(hm_show_vfsstat);

    logkd("hide_mount: init paths=%d hooks=%d (vfsmnt=%llx mountinfo=%llx vfsstat=%llx)\n", hm_path_count,
          hm_hook_count, (uint64_t)(uintptr_t)hm_show_vfsmnt, (uint64_t)(uintptr_t)hm_show_mountinfo,
          (uint64_t)(uintptr_t)hm_show_vfsstat);

    if (hm_hook_count == 0) return -1; /* 三个 show_* 一个都找不到：明确失败，不留空壳 */

    return 0;
}

/*
 * ctl0 用法（APatch 管理器模块卡片上的「参数」，或 kpatch kpm ctl0）：
 *   （空）/ status     打印当前路径表与钩子数
 *   add:/some/path     追加一条（绝对路径，最长 79 字节，最多 8 条）
 *   del:/some/path     删除一条
 *   clear              清空
 * 回写内容：rc=<0|负> paths=<n> hooks=<n> <路径...>
 */
static long hide_mount_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[HM_STATUS_BUF];
    int n, rc = 0;
    int i;

    if (args && args[0] && strncmp(args, "status", 6) != 0) {
        /* AUD-b-9：命令前缀要整词匹配，避免 "clearXXX" 被当成 clear */
        if (strncmp(args, "add:", 4) == 0) {
            rc = hm_add_path(args + 4, (int)strlen(args + 4));
        } else if (strncmp(args, "del:", 4) == 0) {
            rc = hm_del_path(args + 4, (int)strlen(args + 4));
        } else if (strncmp(args, "clear", 5) == 0 &&
                   (args[5] == '\0' || args[5] == ' ' || args[5] == '\n')) {
            hm_clear_paths();
        } else {
            rc = -2; /* 未知命令 */
        }
    }

    n = snprintf(buf, sizeof(buf), "rc=%d paths=%d hooks=%d", rc, hm_path_count, hm_hook_count);

    for (i = 0; i < hm_path_count; i++) {
        int left;
        if (!hm_paths[i][0]) continue;
        left = (int)sizeof(buf) - n;
        if (left <= 1) break;
        n += snprintf(buf + n, (size_t)left, " %s", hm_paths[i]);
        if (n >= (int)sizeof(buf)) {
            n = (int)sizeof(buf) - 1;
            break;
        }
    }

    if (out_msg && outlen > 0) {
        int len = n;
        if (len > outlen - 1) len = outlen - 1;
        if (len > 0) compat_copy_to_user(out_msg, buf, (int)len);
    }

    return rc; /* add/del 失败要让调用方看得见，不要一律回 0 */
}

static long hide_mount_exit(void *__user reserved)
{
    hm_unhook_one(hm_show_vfsmnt);
    hm_unhook_one(hm_show_mountinfo);
    hm_unhook_one(hm_show_vfsstat);

    logkd("hide_mount: exit, unhooked %d\n", hm_hook_count);

    hm_hook_count = 0;
    return 0;
}

KPM_INIT(hide_mount_init);
KPM_CTL0(hide_mount_control0);
KPM_EXIT(hide_mount_exit);
