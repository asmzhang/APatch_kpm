#include <linux/printk.h>
#include <kputils.h>
#include <uapi/asm-generic/errno-base.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "komm.h"
#include "shared_data.h"
#include "parser.h"
#include "../control/control.h"

static int convert_parser_error(int parser_error) {
    switch (parser_error) {
        case PARSER_SUCCESS:
            return 0;
        case PARSER_ERR_INVALID:
        case PARSER_ERR_PARAM:
            return -EINVAL;
        case PARSER_ERR_UNKNOWN:
            return -ENOENT;
        case PARSER_ERR_OPERATION:
            return -EIO;
        default:
            return -EINVAL;
    }
}

static ssize_t my_write(struct file *file, const char __user *buf, size_t count, loff_t *pos) {
    char yuuki[SHARED_DATA_MAX_STR_LEN];
    int ret;
    ssize_t actual_count;

    actual_count = (count < sizeof(yuuki) - 1) ? count : (sizeof(yuuki) - 1);

    if (compat_strncpy_from_user(yuuki, buf, actual_count) < 0) {
        pr_err("[yuuki] strncpy failed\n");
        return -EFAULT;
    }

    yuuki[actual_count] = '\0';

    if (actual_count > 0 && yuuki[actual_count - 1] == '\n') {
        yuuki[actual_count - 1] = '\0';
    }

    pr_info("[yuuki] Received: '%s'\n", yuuki);

    if (parser_is_command(yuuki)) {
        if (!control_is_privileged(control_current_uid())) {
            pr_warn("[yuuki] command denied for uid %u\n", control_current_uid());
            return -EPERM;
        }
        pr_info("[yuuki] Processing command\n");
        ret = parser_process_command(yuuki);

        if (ret != PARSER_SUCCESS) {
            pr_err("[yuuki] Command processing failed: %d\n", ret);
            return convert_parser_error(ret);
        }

        pr_info("[yuuki] Command processed successfully\n");
        return count;
    }

    pr_info("[yuuki] Processing as data\n");
    ret = shared_data_add_parsed_scoped(yuuki, control_current_uid(),
                                       control_is_privileged(control_current_uid()));

    if (ret < 0) {
        switch (ret) {
            case SHARED_DATA_ERR_FULL:
                pr_err("[yuuki] shared_data is full\n");
                return -ENOSPC;
            case SHARED_DATA_ERR_NOMEM:
                pr_err("[yuuki] No memory for shared_data\n");
                return -ENOMEM;
            case SHARED_DATA_ERR_TOOLONG:
                pr_err("[yuuki] String too long for shared_data\n");
                return -E2BIG;
            case SHARED_DATA_ERR_PARSE:
                pr_err("[yuuki] Invalid data format. Use MAP:uid:src:dst, PATH:path, or PTRACE:uid\n");
                return -EINVAL;
            default:
                pr_err("[yuuki] Failed to add to shared_data: %d\n", ret);
                return -EINVAL;
        }
    }

    pr_info("[yuuki] Successfully added to shared_data. Total: %u (MAP:%u, PATH:%u, PTRACE:%u)\n",
            shared_data_get_count(),
            shared_data_get_count_by_type(SHARED_DATA_TYPE_PATH_MAPPING),
            shared_data_get_count_by_type(SHARED_DATA_TYPE_SINGLE_PATH),
            shared_data_get_count_by_type(SHARED_DATA_TYPE_PTRACE_UID));

    return count;
}

#define MC_READ_MAX (64 * 1024)

static ssize_t my_read(struct file *file, char __user *buf, size_t count, loff_t *pos) {
    char *kernel_buf;
    size_t total_len = 0;
    int i, ret;
    struct shared_data_entry entry;
    unsigned int global_index;
    char temp[512];
    int temp_len;
    unsigned int map_count, path_count, ptrace_count;
    unsigned int range_count, regs_count;

    if (*pos > 0) {
        return 0;
    }

    if (!shared_data_is_initialized()) {
        pr_warn("[yuuki] shared_data not yet initialized\n");
        return -EAGAIN;
    }

    if (count > MC_READ_MAX) {
        count = MC_READ_MAX;
    }

    kernel_buf = kf_vmalloc(count);
    if (!kernel_buf) {
        return -ENOMEM;
    }
    memset(kernel_buf, 0, count);

    map_count = shared_data_get_count_by_type(SHARED_DATA_TYPE_PATH_MAPPING);
    path_count = shared_data_get_count_by_type(SHARED_DATA_TYPE_SINGLE_PATH);
    ptrace_count = shared_data_get_count_by_type(SHARED_DATA_TYPE_PTRACE_UID);
    range_count = shared_data_get_count_by_type(SHARED_DATA_TYPE_ADDR_RANGE);
    regs_count = shared_data_get_count_by_type(SHARED_DATA_TYPE_PTRACE_REGS);

    temp_len = snprintf(temp, sizeof(temp), "[MAPPINGS:%u]\n", map_count);
    if (total_len + temp_len < count) {
        memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
        total_len += temp_len;
    } else {
        goto buffer_full;
    }

    for (i = 0; i < map_count && total_len < count - 512; i++) {
        ret = shared_data_get_type_entry_info(SHARED_DATA_TYPE_PATH_MAPPING, i,
                                              &global_index, &entry);
        if (ret == SHARED_DATA_SUCCESS) {
            temp_len = snprintf(temp, sizeof(temp), "%u:%s:%s\n",
                                entry.data.mapping.uid,
                                entry.data.mapping.src_path,
                                entry.data.mapping.dst_path);

            if (total_len + temp_len < count) {
                memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
                total_len += temp_len;
            } else {
                goto buffer_full;
            }
        }
    }

    temp_len = snprintf(temp, sizeof(temp), "[PATHS:%u]\n", path_count);
    if (total_len + temp_len < count) {
        memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
        total_len += temp_len;
    } else {
        goto buffer_full;
    }

    for (i = 0; i < path_count && total_len < count - 512; i++) {
        ret = shared_data_get_type_entry_info(SHARED_DATA_TYPE_SINGLE_PATH, i,
                                              &global_index, &entry);
        if (ret == SHARED_DATA_SUCCESS) {
            temp_len = snprintf(temp, sizeof(temp), "%s\n",
                                entry.data.single.path);

            if (total_len + temp_len < count) {
                memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
                total_len += temp_len;
            } else {
                goto buffer_full;
            }
        }
    }

    temp_len = snprintf(temp, sizeof(temp), "[PTRACE:%u]\n", ptrace_count);
    if (total_len + temp_len < count) {
        memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
        total_len += temp_len;
    } else {
        goto buffer_full;
    }

    for (i = 0; i < ptrace_count && total_len < count - 512; i++) {
        ret = shared_data_get_type_entry_info(SHARED_DATA_TYPE_PTRACE_UID, i,
                                              &global_index, &entry);
        if (ret == SHARED_DATA_SUCCESS) {
            temp_len = snprintf(temp, sizeof(temp), "%u\n",
                                entry.data.ptrace_uid.uid);

            if (total_len + temp_len < count) {
                memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
                total_len += temp_len;
            } else {
                goto buffer_full;
            }
        }
    }

    temp_len = snprintf(temp, sizeof(temp), "[RANGES:%u]\n", range_count);
    if (total_len + temp_len < count) {
        memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
        total_len += temp_len;
    } else {
        goto buffer_full;
    }

    for (i = 0; i < range_count && total_len < count - 512; i++) {
        ret = shared_data_get_type_entry_info(SHARED_DATA_TYPE_ADDR_RANGE, i,
                                              &global_index, &entry);
        if (ret == SHARED_DATA_SUCCESS) {
            temp_len = snprintf(temp, sizeof(temp), "%u:%lx:%lx\n",
                                entry.data.range.uid,
                                entry.data.range.start,
                                entry.data.range.end);

            if (total_len + temp_len < count) {
                memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
                total_len += temp_len;
            } else {
                goto buffer_full;
            }
        }
    }

    temp_len = snprintf(temp, sizeof(temp), "[REGS:%u]\n", regs_count);
    if (total_len + temp_len < count) {
        memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
        total_len += temp_len;
    } else {
        goto buffer_full;
    }

    for (i = 0; i < regs_count && total_len < count - 512; i++) {
        ret = shared_data_get_type_entry_info(SHARED_DATA_TYPE_PTRACE_REGS, i,
                                              &global_index, &entry);
        if (ret == SHARED_DATA_SUCCESS) {
            temp_len = snprintf(temp, sizeof(temp), "%u\n",
                                entry.data.ptrace_uid.uid);

            if (total_len + temp_len < count) {
                memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
                total_len += temp_len;
            } else {
                goto buffer_full;
            }
        }
    }

    temp_len = snprintf(temp, sizeof(temp), "[END]\n");
    if (total_len + temp_len < count) {
        memcpy(kernel_buf + total_len, temp, (size_t)temp_len);
        total_len += temp_len;
    }

    buffer_full:

    if (total_len == 0 || (map_count == 0 && path_count == 0 && ptrace_count == 0 &&
                           range_count == 0 && regs_count == 0)) {
        temp_len = snprintf(kernel_buf, count,
                            "[MAPPINGS:0]\n[PATHS:0]\n[PTRACE:0]\n[RANGES:0]\n[REGS:0]\n[END]\n");
        total_len = temp_len;
    }

    if (!compat_copy_to_user(buf, kernel_buf, total_len)) {
        kf_vfree(kernel_buf);
        pr_err("[yuuki] Failed to copy to user\n");
        return -EFAULT;
    }

    kf_vfree(kernel_buf);

    *pos += total_len;

    pr_info("[yuuki] Read %zu bytes (mappings:%u, paths:%u, ptrace:%u)\n",
            total_len, map_count, path_count, ptrace_count);

    return total_len;
}

static int on_misc_open(struct inode *inode, struct file *file) {
    uid_t uid = control_current_uid();

    if (!control_is_privileged(uid)) {
        pr_warn("[yuuki] /dev/%s open denied for uid %u\n", DEVICE_NAME, uid);
        return -EPERM;
    }
    return 0;
}

static int on_misc_release(struct inode *inode, struct file *file) {
    //pr_info("[yuuki] Device closed\n");
    return 0;
}

static const struct file_operations my_fops = {
        .owner = THIS_MODULE,
        .read = my_read,
        .write = my_write,
        .open = on_misc_open,
        .release = on_misc_release,
};

static struct miscdevice yuuki_misc = {
        .minor = MISC_DYNAMIC_MINOR,
        .name = DEVICE_NAME,
        .fops = &my_fops,
        /* 0600：普通进程连 open 都到不了；受信 uid 由 on_misc_open 再校验一次（AUD-a-2） */
        .mode = 0600,
};

long ioctl_init() {
    int ret;

    ret = shared_data_init();
    if (ret) {
        pr_err("[yuuki] Failed to initialize shared data module\n");
        return ret;
    }

    ret = misc_register(&yuuki_misc);
    if (ret) {
        pr_err("[yuuki] Failed to register misc device\n");
        shared_data_exit();
        return ret;
    }

    pr_info("[yuuki] komm loaded successfully\n");
    return 0;
}

long ioctl_exit() {
    misc_deregister(&yuuki_misc);
    shared_data_exit();
    pr_info("[yuuki] komm unloaded\n");
    return 0;
}
