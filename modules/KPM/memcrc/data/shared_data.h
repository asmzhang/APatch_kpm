#ifndef MEMORY_CRC_SHARED_DATA_H
#define MEMORY_CRC_SHARED_DATA_H

#include <linux/spinlock.h>

#define SHARED_DATA_MAX_ENTRIES    1024

#define SHARED_DATA_MAX_STR_LEN    256

#define SHARED_DATA_MAX_PATH_LEN   256


enum shared_data_type {
    SHARED_DATA_TYPE_INVALID = 0,
    SHARED_DATA_TYPE_PATH_MAPPING,  // 类型1: UID + 原始路径 + 目标路径
    SHARED_DATA_TYPE_SINGLE_PATH,   // 类型2: 单个路径
    SHARED_DATA_TYPE_PTRACE_UID,    // 类型3: PTRACE UID
    SHARED_DATA_TYPE_ADDR_RANGE,    // 类型4: PID + 地址区间（隐藏 /proc/<pid>/maps 里的行）
};

#define SHARED_DATA_TYPE_MAX   SHARED_DATA_TYPE_ADDR_RANGE

struct path_mapping_data {
    uid_t uid;
    char src_path[SHARED_DATA_MAX_PATH_LEN];
    char dst_path[SHARED_DATA_MAX_PATH_LEN];
};

struct single_path_data {
    char path[SHARED_DATA_MAX_PATH_LEN];
};

struct ptrace_uid_data {
    uid_t uid;
};

/* 类型4：把 uid（0 表示所有进程）的 [start, end) 地址区间从 maps 里抹掉 */
struct addr_range_data {
    uid_t uid;
    unsigned long start;
    unsigned long end;
};

struct shared_data_entry {
    enum shared_data_type type;
    union {
        struct path_mapping_data mapping;
        struct single_path_data single;
        struct ptrace_uid_data ptrace_uid;
        struct addr_range_data range;
    } data;
};

#define SHARED_DATA_SUCCESS        0
#define SHARED_DATA_ERR_FULL      (-1)
#define SHARED_DATA_ERR_NOMEM     (-2)
#define SHARED_DATA_ERR_NOTFOUND  (-3)
#define SHARED_DATA_ERR_INVALID   (-4)
#define SHARED_DATA_ERR_TOOLONG   (-5)
#define SHARED_DATA_ERR_PARSE     (-6)

int shared_data_init(void);

void shared_data_exit(void);


int shared_data_add_path_mapping(uid_t uid, const char *src_path, const char *dst_path);

int shared_data_add_single_path(const char *path);

int shared_data_add_ptrace_uid(uid_t uid);

// 解析并添加数据
// 格式1: "MAP:uid:src_path:dst_path"
// 格式2: "PATH:path"
// 格式3: "PTRACE:uid"
// 格式4: "RANGE:uid:start:end"（start/end 十六进制，可带 0x；uid=0 表示所有进程）
//
// 无特权调用者只能改自己：MAP/PTRACE/RANGE 的 uid 必须等于
// caller_uid；PATH 是全局规则，只允许特权调用者添加。
int shared_data_add_parsed(const char *str);

int shared_data_add_parsed_scoped(const char *str, uid_t caller_uid, bool privileged);

int shared_data_get_entry_by_index(unsigned int index, struct shared_data_entry *entry);

int shared_data_get_indices_by_type(enum shared_data_type type, int *indices, int max_count);

int shared_data_find_path_mapping(uid_t uid, const char *src_path);

int shared_data_find_single_path(const char *path);

int shared_data_find_ptrace_uid(uid_t uid);

int shared_data_remove_by_type_index(enum shared_data_type type, unsigned int type_index);

int shared_data_get_type_entry_info(enum shared_data_type type, unsigned int type_index,
                                    unsigned int *global_index, struct shared_data_entry *entry);

int shared_data_remove_by_index(unsigned int index);

unsigned int shared_data_get_count(void);

unsigned int shared_data_get_count_by_type(enum shared_data_type type);

void shared_data_clear(void);

void shared_data_clear_by_type(enum shared_data_type type);

void shared_data_dump(void);

void shared_data_dump_by_type(enum shared_data_type type);

int shared_data_parse_path_mapping(const char *str, uid_t *uid, char *src_path, char *dst_path);

int shared_data_parse_ptrace_uid(const char *str, uid_t *uid);

int shared_data_format_path_mapping(uid_t uid, const char *src_path, const char *dst_path,
                                    char *buf, size_t buf_size);

int shared_data_format_ptrace_uid(uid_t uid, char *buf, size_t buf_size);

int shared_data_add_addr_range(uid_t uid, unsigned long start, unsigned long end);

int shared_data_find_addr_range(uid_t uid, unsigned long start, unsigned long end);

int shared_data_parse_addr_range(const char *str, uid_t *uid, unsigned long *start, unsigned long *end);

int shared_data_format_addr_range(uid_t uid, unsigned long start, unsigned long end,
                                  char *buf, size_t buf_size);

bool shared_data_is_initialized(void);

extern void (*kf_raw_spin_unlock_irqrestore)(spinlock_t *lock, unsigned long flags);
extern unsigned long (*kf_raw_spin_lock_irqsave)(spinlock_t *lock);

#endif