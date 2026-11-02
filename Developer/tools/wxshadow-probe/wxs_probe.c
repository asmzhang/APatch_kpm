/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxs_probe —— wxshadow P0 探针（只为取证，不是模块实现）
 *
 * 目的：在 5.10(LKM) 与 6.1(kernel) 上验证两件后面全靠它的事：
 *   1) 能从 pid 走到目标 mm 的 leaf PTE（pgtable_entry + mm_struct_offset.pgd_offset）；
 *   2) 能把 PTE 里的物理地址换算成内核可读地址（线性映射），且读出来的内容
 *      与目标进程自己读到的完全一致。
 *
 * 换算方式不猜版本：kmalloc 一页 → 写魔数 → `at s1e1w` 取它的物理地址 →
 * linear_voffset = kva - pa → 再用 pa + linear_voffset 回读同一页做自校验。
 * 这样既不需要 KP 的私有全局变量（linear_voffset/memstart_addr 都没导出给 KPM），
 * 也不需要假设 PAGE_OFFSET：PAGE_OFFSET 只用于打印。
 *
 * ctl0：
 *   self                      —— 打印自校验结果与换算因子
 *   probe <pid> <hexva>       —— 打印该 va 的 pgd 项、leaf PTE、pa、以及前 32 字节
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
#include <linux/mm_types.h>
#include <pgtable.h>

KPM_NAME("wxs_probe");
KPM_VERSION("0.0.1");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("APatch_kpm");
KPM_DESCRIPTION("wxshadow P0 probe: pgd walk + phys/kva self check");

#define WXS_MAGIC "WXSPROBE-P0"
#define WXS_MAGIC_LEN 11

/* GFP_KERNEL = ___GFP_RECLAIM|___GFP_IO|___GFP_FS = 0x10|0x40|0x80 在 5.x/6.x 一致；
 * 这里直接用内核里的值，避免引入 gfp 头文件带来的版本差异。 */
#define WXS_GFP_KERNEL 0xcc0UL

static struct task_struct *(*kf_find_task_by_vpid)(pid_t nr);
static struct mm_struct *(*kf_get_task_mm)(struct task_struct *task);
static void (*kf_mmput)(struct mm_struct *mm);
static void *(*kf___kmalloc)(size_t size, unsigned long flags);
static void (*kf_kfree)(const void *p);

static uint64_t wxs_linear_voffset;
static int wxs_va_bits;
static uint64_t wxs_page_offset;
static int wxs_selfcheck;
static void *wxs_selfpage;

extern struct mm_struct_offset mm_struct_offset;

/* AT S1E1W：用当前页表把内核 VA 翻成物理地址（成功时 PAR[0]=0）。 */
static uint64_t wxs_at_s1e1w(uint64_t va)
{
    uint64_t par;

    asm volatile("at s1e1w, %0" ::"r"(va) : "memory");
    asm volatile("isb" ::: "memory");
    asm volatile("mrs %0, par_el1" : "=r"(par));
    if (par & 1)
        return 0;
    return par & 0x0000fffffffff000ULL;
}

static int wxs_read_va_bits(void)
{
    uint64_t tcr;
    int t1sz;
    int bits;

    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    t1sz = (int)((tcr >> 16) & 0x3f);
    if (t1sz == 0)
        return 48;
    bits = 64 - t1sz;
    if (bits < 32 || bits > 52)
        return 48;
    return bits;
}

static void wxs_selfcheck_do(void)
{
    uint64_t kva;
    uint64_t pa;

    wxs_selfpage = kf___kmalloc(4096, WXS_GFP_KERNEL);
    if (!wxs_selfpage) {
        logke("wxs_probe: kmalloc failed\n");
        return;
    }
    memcpy(wxs_selfpage, WXS_MAGIC, WXS_MAGIC_LEN);
    kva = (uint64_t)wxs_selfpage & ~0xfffULL;
    pa = wxs_at_s1e1w(kva);
    if (!pa) {
        logke("wxs_probe: at s1e1w failed\n");
        return;
    }
    wxs_linear_voffset = kva - pa;
    if (kf_memcmp((void *)(pa + wxs_linear_voffset), WXS_MAGIC, WXS_MAGIC_LEN) == 0)
        wxs_selfcheck = 1;
    logki("wxs_probe: selfcheck=%d linear_voffset=0x%llx va_bits=%d\n", wxs_selfcheck,
          (unsigned long long)wxs_linear_voffset, wxs_va_bits);
}

static int wxs_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* 解析十六进制或十进制；返回消耗的字符数，0 表示解析失败。 */
static int wxs_parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    int i = 0;
    int base = 10;
    int d;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; s[i]; i++) {
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
            break;
        d = wxs_hexval(s[i]);
        if (d < 0 || d >= base)
            break;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (i == 0)
        return 0;
    *out = v;
    return i;
}

static int wxs_dump_page(pid_t pid, uint64_t va, char *out, int outlen)
{
    struct task_struct *task;
    struct mm_struct *mm;
    uint64_t pgd;
    uint64_t *peg;
    uint64_t pte;
    uint64_t pa;
    const unsigned char *p;
    int i;
    int n;

    task = kf_find_task_by_vpid(pid);
    if (!task) {
        n = snprintf(out, outlen, "no such pid %d\n", pid);
        return n;
    }
    mm = kf_get_task_mm(task);
    if (!mm) {
        n = snprintf(out, outlen, "pid %d has no mm\n", pid);
        return n;
    }
    pgd = *(uint64_t *)((char *)mm + mm_struct_offset.pgd_offset);
    peg = pgtable_entry(pgd, va);
    if (!peg) {
        kf_mmput(mm);
        return snprintf(out, outlen, "pgd=0x%llx va=0x%llx: no leaf entry\n",
                        (unsigned long long)pgd, (unsigned long long)va);
    }
    pte = *peg;
    if (!(pte & PTE_VALID)) {
        kf_mmput(mm);
        return snprintf(out, outlen, "pte=0x%llx: invalid (page not resident)\n",
                        (unsigned long long)pte);
    }
    if ((pte & PTE_TYPE_MASK) != PTE_TYPE_PAGE) {
        kf_mmput(mm);
        return snprintf(out, outlen, "pte=0x%llx: not a 4K leaf (block/table)\n",
                        (unsigned long long)pte);
    }
    pa = (pte & 0x0000fffffffff000ULL) | (va & 0xfffULL);
    p = (const unsigned char *)(pa + wxs_linear_voffset);
    n = snprintf(out, outlen, "pid=%d va=0x%llx pgd=0x%llx pte=0x%llx pa=0x%llx kva=0x%llx data=",
                 pid, (unsigned long long)va, (unsigned long long)pgd, (unsigned long long)pte,
                 (unsigned long long)pa, (unsigned long long)(uint64_t)p);
    for (i = 0; i < 16 && n < outlen - 4; i++)
        n += snprintf(out + n, outlen - n, "%02x", p[i]);
    if (n < outlen - 1)
        out[n++] = '\n';
    out[n < outlen ? n : outlen - 1] = 0;
    kf_mmput(mm);
    return n;
}

static long wxs_probe_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[256];
    const char *p = args;
    uint64_t pid64 = 0;
    uint64_t va = 0;
    int adv;
    int n = 0;

    buf[0] = 0;
    if (!args) {
        snprintf(buf, sizeof(buf), "wxs_probe: no args\n");
        goto out;
    }
    while (*p == ' ')
        p++;

    if (!strncmp(p, "self", 4)) {
        snprintf(buf, sizeof(buf),
                 "wxs_probe v0.0.1 selfcheck=%d linear_voffset=0x%llx va_bits=%d page_offset=0x%llx pgd_off=%d\n",
                 wxs_selfcheck, (unsigned long long)wxs_linear_voffset, wxs_va_bits,
                 (unsigned long long)wxs_page_offset, (int)mm_struct_offset.pgd_offset);
        goto out;
    }

    if (strncmp(p, "probe", 5)) {
        snprintf(buf, sizeof(buf), "unknown cmd\n");
        goto out;
    }
    p += 5;
    while (*p == ' ')
        p++;
    adv = wxs_parse_u64(p, &pid64);
    if (!adv) {
        snprintf(buf, sizeof(buf), "bad pid\n");
        goto out;
    }
    p += adv;
    while (*p == ' ')
        p++;
    adv = wxs_parse_u64(p, &va);
    if (!adv) {
        snprintf(buf, sizeof(buf), "bad va\n");
        goto out;
    }
    n = wxs_dump_page((pid_t)pid64, va, buf, (int)sizeof(buf));
    (void)n;

out:
    /* 只能经 compat_copy_to_user 回写；直接给 out_msg[i] 赋值是内核态写用户地址，
     * 在开了 PAN 的 arm64 上会触发内核缺页（P0 第一次上机就是这么把 Pixel 6 打掉的）。 */
    if (out_msg && outlen > 0) {
        int len = (int)strlen(buf);
        if (len > outlen - 1)
            len = outlen - 1;
        compat_copy_to_user(out_msg, buf, len + 1);
    }
    return 0;
}

static long wxs_probe_init(const char *args, const char *event, void *__user reserved)
{
    kf_find_task_by_vpid = (void *)kallsyms_lookup_name("find_task_by_vpid");
    kf_get_task_mm = (void *)kallsyms_lookup_name("get_task_mm");
    kf_mmput = (void *)kallsyms_lookup_name("mmput");
    kf___kmalloc = (void *)kallsyms_lookup_name("__kmalloc");
    kf_kfree = (void *)kallsyms_lookup_name("kfree");
    if (!kf_find_task_by_vpid || !kf_get_task_mm || !kf_mmput || !kf___kmalloc || !kf_kfree) {
        logke("wxs_probe: symbol missing\n");
        return -1;
    }
    wxs_va_bits = wxs_read_va_bits();
    /* arm64: PAGE_OFFSET = ~0 << VA_BITS（39 → 0xffffff8000000000，48 → 0xffff000000000000） */
    wxs_page_offset = ~0ULL << wxs_va_bits;
    wxs_selfcheck_do();
    logki("wxs_probe: init done\n");
    return 0;
}

static long wxs_probe_exit(void *__user reserved)
{
    if (wxs_selfpage && kf_kfree)
        kf_kfree(wxs_selfpage);
    wxs_selfpage = 0;
    return 0;
}

KPM_INIT(wxs_probe_init);
KPM_CTL0(wxs_probe_control0);
KPM_EXIT(wxs_probe_exit);
