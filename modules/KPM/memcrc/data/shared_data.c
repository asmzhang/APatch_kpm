#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <baselib.h>
#include "shared_data.h"
#include "kstring.h"

struct internal_entry {
    struct shared_data_entry entry;
    bool valid;
};

static struct {
    struct internal_entry entries[SHARED_DATA_MAX_ENTRIES];
    unsigned int count;
    unsigned int count_by_type[4];
    spinlock_t lock;
    bool initialized;
} shared_data = {
        .count = 0,
        .initialized = false,
};

static int find_free_slot(void) {
    int i;
    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (!shared_data.entries[i].valid) {
            return i;
        }
    }
    return -1;
}

/* 上游遗留、当前无调用点；保留以便与上游对读，用 unused 抑制告警 */
static char *trim_spaces(char *str) __attribute__((unused));
static char *trim_spaces(char *str) {
    char *end;

    while (*str && isspace(*str)) str++;

    end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) *end-- = '\0';

    return str;
}


int shared_data_init(void) {
    if (shared_data.initialized) {
        pr_warn("shared_data: already initialized\n");
        return SHARED_DATA_SUCCESS;
    }

    spin_lock_init(&shared_data.lock);
    memset(shared_data.entries, 0, sizeof(shared_data.entries));
    memset(shared_data.count_by_type, 0, sizeof(shared_data.count_by_type));
    shared_data.count = 0;
    shared_data.initialized = true;

    pr_info("shared_data: initialized successfully with type support\n");
    return SHARED_DATA_SUCCESS;
}

void shared_data_exit(void) {
    unsigned long flags;

    if (!shared_data.initialized) {
        return;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    memset(shared_data.entries, 0, sizeof(shared_data.entries));
    memset(shared_data.count_by_type, 0, sizeof(shared_data.count_by_type));
    shared_data.count = 0;
    shared_data.initialized = false;

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    pr_info("shared_data: cleanup completed\n");
}

int shared_data_add_path_mapping(uid_t uid, const char *src_path, const char *dst_path) {
    unsigned long flags;
    int slot;

    if (!shared_data.initialized) {
        return SHARED_DATA_ERR_INVALID;
    }

    if (!src_path || !dst_path) {
        return SHARED_DATA_ERR_INVALID;
    }

    if (strlen(src_path) >= SHARED_DATA_MAX_PATH_LEN ||
        strlen(dst_path) >= SHARED_DATA_MAX_PATH_LEN) {
        return SHARED_DATA_ERR_TOOLONG;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    if (shared_data.count >= SHARED_DATA_MAX_ENTRIES) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_FULL;
    }

    slot = find_free_slot();
    if (slot < 0) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_FULL;
    }

    /* 填充数据 */
    shared_data.entries[slot].entry.type = SHARED_DATA_TYPE_PATH_MAPPING;
    shared_data.entries[slot].entry.data.mapping.uid = uid;
    strncpy(shared_data.entries[slot].entry.data.mapping.src_path,
            src_path, SHARED_DATA_MAX_PATH_LEN - 1);
    strncpy(shared_data.entries[slot].entry.data.mapping.dst_path,
            dst_path, SHARED_DATA_MAX_PATH_LEN - 1);
    shared_data.entries[slot].valid = true;

    shared_data.count++;
    shared_data.count_by_type[SHARED_DATA_TYPE_PATH_MAPPING]++;

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    pr_info("shared_data: added path mapping [%u]: %u:%s->%s\n",
            slot, uid, src_path, dst_path);

    return SHARED_DATA_SUCCESS;
}

int shared_data_add_single_path(const char *path) {
    unsigned long flags;
    int slot;

    if (!shared_data.initialized || !path) {
        return SHARED_DATA_ERR_INVALID;
    }

    if (strlen(path) >= SHARED_DATA_MAX_PATH_LEN) {
        return SHARED_DATA_ERR_TOOLONG;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    if (shared_data.count >= SHARED_DATA_MAX_ENTRIES) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_FULL;
    }

    slot = find_free_slot();
    if (slot < 0) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_FULL;
    }

    shared_data.entries[slot].entry.type = SHARED_DATA_TYPE_SINGLE_PATH;
    strncpy(shared_data.entries[slot].entry.data.single.path,
            path, SHARED_DATA_MAX_PATH_LEN - 1);
    shared_data.entries[slot].valid = true;

    shared_data.count++;
    shared_data.count_by_type[SHARED_DATA_TYPE_SINGLE_PATH]++;

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    pr_info("shared_data: added single path [%u]: %s\n", slot, path);

    return SHARED_DATA_SUCCESS;
}

bool shared_data_is_initialized(void) {
    return shared_data.initialized;
}

int shared_data_parse_path_mapping(const char *str, uid_t *uid, char *src_path, char *dst_path) {
    char buffer[SHARED_DATA_MAX_STR_LEN];
    char *token, *saveptr;
    char *uid_str;
    unsigned long uid_val;

    if (!str || !uid || !src_path || !dst_path) {
        return SHARED_DATA_ERR_INVALID;
    }

    strncpy(buffer, str, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    uid_str = kstring_strtok_r(buffer, ":", &saveptr);
    if (!uid_str) {
        return SHARED_DATA_ERR_PARSE;
    }

    uid_str = kstring_trim(uid_str);

    if (kstring_strtoul(uid_str, 10, &uid_val) != 0) {
        return SHARED_DATA_ERR_PARSE;
    }
    *uid = (uid_t)uid_val;

    token = kstring_strtok_r(NULL, ":", &saveptr);
    if (!token) {
        return SHARED_DATA_ERR_PARSE;
    }
    token = kstring_trim(token);
    strncpy(src_path, token, SHARED_DATA_MAX_PATH_LEN - 1);
    src_path[SHARED_DATA_MAX_PATH_LEN - 1] = '\0';

    token = kstring_strtok_r(NULL, ":", &saveptr);
    if (!token) {
        return SHARED_DATA_ERR_PARSE;
    }
    token = kstring_trim(token);
    strncpy(dst_path, token, SHARED_DATA_MAX_PATH_LEN - 1);
    dst_path[SHARED_DATA_MAX_PATH_LEN - 1] = '\0';

    return SHARED_DATA_SUCCESS;
}

int shared_data_add_parsed(const char *str) {
    char buffer[SHARED_DATA_MAX_STR_LEN];
    uid_t uid;
    char src_path[SHARED_DATA_MAX_PATH_LEN];
    char dst_path[SHARED_DATA_MAX_PATH_LEN];

    if (!str) {
        return SHARED_DATA_ERR_INVALID;
    }

    strncpy(buffer, str, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    if (strncmp(buffer, "MAP:", 4) == 0) {
        if (shared_data_parse_path_mapping(buffer + 4, &uid, src_path, dst_path) == 0) {
            return shared_data_add_path_mapping(uid, src_path, dst_path);
        }
        return SHARED_DATA_ERR_PARSE;
    }

    if (strncmp(buffer, "PATH:", 5) == 0) {
        return shared_data_add_single_path(buffer + 5);
    }

    if (strncmp(buffer, "PTRACE:", 7) == 0) {
        if (shared_data_parse_ptrace_uid(buffer + 7, &uid) == 0) {
            return shared_data_add_ptrace_uid(uid);
        }
        return SHARED_DATA_ERR_PARSE;
    }

    pr_err("shared_data: Unsupported format: %s\n", str);
    return SHARED_DATA_ERR_PARSE;
}

int shared_data_get_entry_by_index(unsigned int index, struct shared_data_entry *entry) {
    unsigned long flags;

    if (!shared_data.initialized || !entry) {
        return SHARED_DATA_ERR_INVALID;
    }

    if (index >= SHARED_DATA_MAX_ENTRIES) {
        return SHARED_DATA_ERR_INVALID;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    if (!shared_data.entries[index].valid) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_NOTFOUND;
    }

    /* 使用 kstring_memcpy 替代结构体赋值 */
    kstring_memcpy(entry, &shared_data.entries[index].entry, sizeof(struct shared_data_entry));

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return SHARED_DATA_SUCCESS;
}

int shared_data_find_path_mapping(uid_t uid, const char *src_path) {
    unsigned long flags;
    int i, result = -1;

    if (!shared_data.initialized || !src_path) {
        return -1;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == SHARED_DATA_TYPE_PATH_MAPPING &&
            shared_data.entries[i].entry.data.mapping.uid == uid &&
            strcmp(shared_data.entries[i].entry.data.mapping.src_path, src_path) == 0) {
            result = i;
            break;
        }
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return result;
}

int shared_data_remove_by_index(unsigned int index) {
    unsigned long flags;
    enum shared_data_type type;

    if (!shared_data.initialized) {
        return SHARED_DATA_ERR_INVALID;
    }

    if (index >= SHARED_DATA_MAX_ENTRIES) {
        return SHARED_DATA_ERR_INVALID;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    if (!shared_data.entries[index].valid) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_NOTFOUND;
    }

    type = shared_data.entries[index].entry.type;
    shared_data.entries[index].valid = false;
    memset(&shared_data.entries[index].entry, 0, sizeof(struct shared_data_entry));

    shared_data.count--;
    if (type > 0 && type <= 3) {
        shared_data.count_by_type[type]--;
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return SHARED_DATA_SUCCESS;
}

int shared_data_remove_by_type_index(enum shared_data_type type, unsigned int type_index) {
    unsigned long flags;
    int i, found_count = 0;
    int target_global_index = -1;
    enum shared_data_type entry_type;

    if (!shared_data.initialized) {
        return SHARED_DATA_ERR_INVALID;
    }

    if (type < 1 || type > 3) {
        return SHARED_DATA_ERR_INVALID;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == type) {
            if (found_count == type_index) {
                target_global_index = i;
                break;
            }
            found_count++;
        }
    }

    if (target_global_index == -1) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_NOTFOUND;
    }

    entry_type = shared_data.entries[target_global_index].entry.type;
    shared_data.entries[target_global_index].valid = false;
    memset(&shared_data.entries[target_global_index].entry, 0, sizeof(struct shared_data_entry));

    shared_data.count--;
    if (entry_type > 0 && entry_type <= 3) {
        shared_data.count_by_type[entry_type]--;
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    pr_info("shared_data: removed type %d entry at type-index %u (global index %d)\n",
            type, type_index, target_global_index);

    return SHARED_DATA_SUCCESS;
}

int shared_data_get_type_entry_info(enum shared_data_type type, unsigned int type_index,
                                    unsigned int *global_index, struct shared_data_entry *entry) {
    unsigned long flags;
    int i, found_count = 0;
    int target_global_index = -1;

    if (!shared_data.initialized || !global_index) {
        return SHARED_DATA_ERR_INVALID;
    }

    if (type < 1 || type > 3) {
        return SHARED_DATA_ERR_INVALID;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    /* 查找指定类型的第 type_index 个条目 */
    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == type) {
            if (found_count == type_index) {
                target_global_index = i;
                break;
            }
            found_count++;
        }
    }

    if (target_global_index == -1) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_NOTFOUND;
    }

    *global_index = target_global_index;

    if (entry) {
        kstring_memcpy(entry, &shared_data.entries[target_global_index].entry,
                       sizeof(struct shared_data_entry));
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return SHARED_DATA_SUCCESS;
}

unsigned int shared_data_get_count(void) {
    unsigned long flags;
    unsigned int count;

    if (!shared_data.initialized) {
        return 0;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);
    count = shared_data.count;
    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return count;
}

unsigned int shared_data_get_count_by_type(enum shared_data_type type) {
    unsigned long flags;
    unsigned int count;

    if (!shared_data.initialized || type < 1 || type > 3) {
        return 0;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);
    count = shared_data.count_by_type[type];
    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return count;
}

void shared_data_dump(void) {
    unsigned long flags;
    int i;

    if (!shared_data.initialized) {
        pr_info("shared_data: not initialized\n");
        return;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    pr_info("shared_data: total entries = %u\n", shared_data.count);
    pr_info("  Path mappings: %u\n", shared_data.count_by_type[SHARED_DATA_TYPE_PATH_MAPPING]);
    pr_info("  Single paths: %u\n", shared_data.count_by_type[SHARED_DATA_TYPE_SINGLE_PATH]);
    pr_info("  PTRACE UIDs: %u\n", shared_data.count_by_type[SHARED_DATA_TYPE_PTRACE_UID]);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid) {
            switch (shared_data.entries[i].entry.type) {
                case SHARED_DATA_TYPE_PATH_MAPPING:
                    pr_info("  [%d] MAP: %u:%s->%s\n", i,
                            shared_data.entries[i].entry.data.mapping.uid,
                            shared_data.entries[i].entry.data.mapping.src_path,
                            shared_data.entries[i].entry.data.mapping.dst_path);
                    break;
                case SHARED_DATA_TYPE_SINGLE_PATH:
                    pr_info("  [%d] PATH: %s\n", i,
                            shared_data.entries[i].entry.data.single.path);
                    break;
                case SHARED_DATA_TYPE_PTRACE_UID:
                    pr_info("  [%d] PTRACE: %u\n", i,
                            shared_data.entries[i].entry.data.ptrace_uid.uid);
                    break;
                default:
                    pr_info("  [%d] UNKNOWN TYPE: %d\n", i,
                            shared_data.entries[i].entry.type);
            }
        }
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
}

void shared_data_clear(void) {
    unsigned long flags;

    if (!shared_data.initialized) {
        return;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    memset(shared_data.entries, 0, sizeof(shared_data.entries));
    memset(shared_data.count_by_type, 0, sizeof(shared_data.count_by_type));
    shared_data.count = 0;

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
}

void shared_data_clear_by_type(enum shared_data_type type) {
    unsigned long flags;
    int i;

    if (!shared_data.initialized || type < 1 || type > 3) {
        return;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == type) {
            shared_data.entries[i].valid = false;
            memset(&shared_data.entries[i].entry, 0, sizeof(struct shared_data_entry));
            shared_data.count--;
            shared_data.count_by_type[type]--;
        }
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    pr_info("shared_data: cleared all entries of type %d\n", type);
}

int shared_data_get_indices_by_type(enum shared_data_type type, int *indices, int max_count) {
    unsigned long flags;
    int i, count = 0;

    if (!shared_data.initialized || !indices || max_count <= 0) {
        return 0;
    }

    if (type < 1 || type > 3) {
        return 0;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES && count < max_count; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == type) {
            indices[count++] = i;
        }
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return count;
}

int shared_data_find_single_path(const char *path) {
    unsigned long flags;
    int i, result = -1;

    if (!shared_data.initialized || !path) {
        return -1;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == SHARED_DATA_TYPE_SINGLE_PATH &&
            strcmp(shared_data.entries[i].entry.data.single.path, path) == 0) {
            result = i;
            break;
        }
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return result;
}

void shared_data_dump_by_type(enum shared_data_type type) {
    unsigned long flags;
    int i, type_index = 0;

    if (!shared_data.initialized) {
        pr_info("shared_data: not initialized\n");
        return;
    }

    if (type < 1 || type > 3) {
        pr_info("shared_data: invalid type %d\n", type);
        return;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    pr_info("shared_data: entries of type %d:\n", type);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == type) {
            switch (type) {
                case SHARED_DATA_TYPE_PATH_MAPPING:
                    pr_info("  Type-index %d (global %d): MAP: %u:%s->%s\n", type_index, i,
                            shared_data.entries[i].entry.data.mapping.uid,
                            shared_data.entries[i].entry.data.mapping.src_path,
                            shared_data.entries[i].entry.data.mapping.dst_path);
                    break;
                case SHARED_DATA_TYPE_SINGLE_PATH:
                    pr_info("  Type-index %d (global %d): PATH: %s\n", type_index, i,
                            shared_data.entries[i].entry.data.single.path);
                    break;
                case SHARED_DATA_TYPE_PTRACE_UID:
                    pr_info("  Type-index %d (global %d): PTRACE: %u\n", type_index, i,
                            shared_data.entries[i].entry.data.ptrace_uid.uid);
                    break;
                default:
                    /* SHARED_DATA_TYPE_INVALID：上面已按 1..3 过滤，走不到这里 */
                    break;
            }
            type_index++;
        }
    }

    pr_info("Total of type %d: %u\n", type, shared_data.count_by_type[type]);

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
}

int shared_data_format_path_mapping(uid_t uid, const char *src_path, const char *dst_path,
                                    char *buf, size_t buf_size) {
    int len;

    if (!src_path || !dst_path || !buf || buf_size == 0) {
        return SHARED_DATA_ERR_INVALID;
    }

    len = snprintf(buf, buf_size, "%u:%s:%s", uid, src_path, dst_path);

    if (len >= buf_size) {
        return SHARED_DATA_ERR_TOOLONG;
    }

    return SHARED_DATA_SUCCESS;
}

int shared_data_add_ptrace_uid(uid_t uid) {
    unsigned long flags;
    int slot;

    if (!shared_data.initialized) {
        return SHARED_DATA_ERR_INVALID;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    if (shared_data.count >= SHARED_DATA_MAX_ENTRIES) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_FULL;
    }

    slot = find_free_slot();
    if (slot < 0) {
        kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);
        return SHARED_DATA_ERR_FULL;
    }

    shared_data.entries[slot].entry.type = SHARED_DATA_TYPE_PTRACE_UID;
    shared_data.entries[slot].entry.data.ptrace_uid.uid = uid;
    shared_data.entries[slot].valid = true;

    shared_data.count++;
    shared_data.count_by_type[SHARED_DATA_TYPE_PTRACE_UID]++;

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    pr_info("shared_data: added PTRACE UID [%u]: %u\n", slot, uid);

    return SHARED_DATA_SUCCESS;
}

int shared_data_find_ptrace_uid(uid_t uid) {
    unsigned long flags;
    int i, result = -1;

    if (!shared_data.initialized) {
        return -1;
    }

    flags = kf_raw_spin_lock_irqsave(&shared_data.lock);

    for (i = 0; i < SHARED_DATA_MAX_ENTRIES; i++) {
        if (shared_data.entries[i].valid &&
            shared_data.entries[i].entry.type == SHARED_DATA_TYPE_PTRACE_UID &&
            shared_data.entries[i].entry.data.ptrace_uid.uid == uid) {
            result = i;
            break;
        }
    }

    kf_raw_spin_unlock_irqrestore(&shared_data.lock, flags);

    return result;
}

int shared_data_parse_ptrace_uid(const char *str, uid_t *uid) {
    unsigned long uid_val;
    char buffer[32];
    char *trimmed;

    if (!str || !uid) {
        return SHARED_DATA_ERR_INVALID;
    }

    strncpy(buffer, str, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    trimmed = kstring_trim(buffer);

    if (kstring_strtoul(trimmed, 10, &uid_val) != 0) {
        return SHARED_DATA_ERR_PARSE;
    }

    *uid = (uid_t)uid_val;
    return SHARED_DATA_SUCCESS;
}

int shared_data_format_ptrace_uid(uid_t uid, char *buf, size_t buf_size) {
    int len;

    if (!buf || buf_size == 0) {
        return SHARED_DATA_ERR_INVALID;
    }

    len = snprintf(buf, buf_size, "%u", uid);

    if (len >= buf_size) {
        return SHARED_DATA_ERR_TOOLONG;
    }

    return SHARED_DATA_SUCCESS;
}