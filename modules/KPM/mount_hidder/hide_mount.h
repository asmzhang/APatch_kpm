/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 bmax121. All Rights Reserved.
 *
 * 上游：mount_hidder/hide_mount.h（只有 TARGET_PATH_NUM / MAX_PATH_LEN 与 seq_file 快照）
 */

#ifndef _HIDE_MOUNT_H_
#define _HIDE_MOUNT_H_

/* 默认三条，可由加载参数覆盖；上限放宽到 8 条以适配不同 ROM */
#define HM_MAX_PATHS 8
#define HM_MAX_PATH_LEN 80

#define HM_STATUS_BUF 320

/*
 * linux/seq_file.h 的前五个字段。KernelPatch 的头文件里只有 `struct seq_file;`
 * 前向声明（没有完整定义），这里按 4.x~6.x 一直稳定的布局补全；我们只用到
 * buf / size / count 三个字段，pad_until 之后不再引用。
 */
struct seq_file {
    char *buf;
    size_t size;
    size_t from;
    size_t count;
    size_t pad_until;
};

#endif
