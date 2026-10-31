#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/printk.h>
#include "parser.h"
#include "shared_data.h"
#include "kstring.h"

bool parser_is_command(const char *str) {
    if (!str) {
        return false;
    }
    return (strncmp(str, "CMD:", 4) == 0);
}

// 解析命令参数
int parser_parse_command_args(const char *cmd_str, char *cmd, char *arg1, char *arg2) {
    char buffer[SHARED_DATA_MAX_STR_LEN];
    char *remaining;
    char *token;

    if (!cmd_str || !cmd || !arg1 || !arg2) {
        return PARSER_ERR_INVALID;
    }

    if (strncmp(cmd_str, "CMD:", 4) != 0) {
        return PARSER_ERR_INVALID;
    }

    strncpy(buffer, cmd_str + 4, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    remaining = buffer;

    token = strsep(&remaining, ":");
    if (token) {
        strncpy(cmd, token, 63);
        cmd[63] = '\0';
    } else {
        return PARSER_ERR_INVALID;
    }

    token = strsep(&remaining, ":");
    if (token) {
        strncpy(arg1, token, 63);
        arg1[63] = '\0';
    } else {
        arg1[0] = '\0';
        arg2[0] = '\0';
        return PARSER_SUCCESS;
    }

    if (remaining) {
        strncpy(arg2, remaining, 255);
        arg2[255] = '\0';
    } else {
        arg2[0] = '\0';
    }

    return PARSER_SUCCESS;
}

int parser_handle_clear_command(const char *arg1, const char *arg2) {
    enum shared_data_type type;

    if (strlen(arg1) == 0) {
        shared_data_clear();
        pr_info("[yuuki] [parser] All shared data cleared\n");
        return PARSER_SUCCESS;
    }
    else if (strcmp(arg1, "TYPE") == 0 && strlen(arg2) > 0) {
        type = (enum shared_data_type)kstring_simple_strtoul(arg2, NULL, 10);
        if (type >= 1 && type <= 3) {
            shared_data_clear_by_type(type);
            pr_info("[yuuki] [parser] Cleared shared data of type %d\n", type);
            return PARSER_SUCCESS;
        } else {
            pr_err("[yuuki] [parser] Invalid type: %d\n", type);
            return PARSER_ERR_PARAM;
        }
    }

    pr_err("[yuuki] [parser] Invalid CLEAR command format\n");
    return PARSER_ERR_PARAM;
}

// 处理REMOVE类命令
int parser_handle_remove_command(const char *arg1, const char *arg2) {
    unsigned int index;
    enum shared_data_type type;
    char type_str[16] = {0};
    char type_index_str[16] = {0};
    char *colon_pos;
    unsigned int type_index;
    int ret;

    if (strcmp(arg1, "INDEX") == 0 && strlen(arg2) > 0) {
        index = (unsigned int)kstring_simple_strtoul(arg2, NULL, 10);
        ret = shared_data_remove_by_index(index);
        if (ret == SHARED_DATA_SUCCESS) {
            pr_info("[yuuki] [parser] Removed entry at global index %u\n", index);
            return PARSER_SUCCESS;
        } else {
            pr_err("[yuuki] [parser] Failed to remove entry at index %u: %d\n", index, ret);
            return PARSER_ERR_OPERATION;
        }
    }
    else if (strcmp(arg1, "TYPE") == 0 && strlen(arg2) > 0) {
        colon_pos = strchr(arg2, ':');

        if (!colon_pos) {
            pr_err("[yuuki] [parser] Invalid format for REMOVE TYPE command. Use: CMD:REMOVE:TYPE:type_id:type_index\n");
            return PARSER_ERR_PARAM;
        }

        strncpy(type_str, arg2, colon_pos - arg2);
        type_str[colon_pos - arg2] = '\0';
        strcpy(type_index_str, colon_pos + 1);

        type = (enum shared_data_type)kstring_simple_strtoul(type_str, NULL, 10);
        type_index = (unsigned int)kstring_simple_strtoul(type_index_str, NULL, 10);

        if (type >= 1 && type <= 3) {
            ret = shared_data_remove_by_type_index(type, type_index);
            if (ret == SHARED_DATA_SUCCESS) {
                pr_info("[yuuki] [parser] Successfully removed type %d entry at type-index %u\n",
                        type, type_index);
                return PARSER_SUCCESS;
            } else if (ret == SHARED_DATA_ERR_NOTFOUND) {
                unsigned int count = shared_data_get_count_by_type(type);
                pr_err("[yuuki] [parser] Type %d only has %u entries, index %u is out of range\n",
                       type, count, type_index);
                return PARSER_ERR_PARAM;
            } else {
                pr_err("[yuuki] [parser] Failed to remove entry: %d\n", ret);
                return PARSER_ERR_OPERATION;
            }
        } else {
            pr_err("[yuuki] [parser] Invalid type: %d\n", type);
            return PARSER_ERR_PARAM;
        }
    }

    pr_err("[yuuki] [parser] Invalid REMOVE command format\n");
    return PARSER_ERR_PARAM;
}

// 处理DUMP类命令
int parser_handle_dump_command(const char *arg1, const char *arg2) {
    enum shared_data_type type;

    if (strlen(arg1) == 0) {
        shared_data_dump();
        pr_info("[yuuki] [parser] Dumped all shared data\n");
        return PARSER_SUCCESS;
    }
    else if (strcmp(arg1, "TYPE") == 0 && strlen(arg2) > 0) {
        type = (enum shared_data_type)kstring_simple_strtoul(arg2, NULL, 10);
        if (type >= 1 && type <= 3) {
            shared_data_dump_by_type(type);
            pr_info("[yuuki] [parser] Dumped shared data of type %d\n", type);
            return PARSER_SUCCESS;
        } else {
            pr_err("[yuuki] [parser] Invalid type: %d\n", type);
            return PARSER_ERR_PARAM;
        }
    }

    pr_err("[yuuki] [parser] Invalid DUMP command format\n");
    return PARSER_ERR_PARAM;
}

// 处理STATUS命令
int parser_handle_status_command(void) {
    pr_info("[yuuki] [parser] === Shared Data Status ===\n");
    pr_info("[yuuki] [parser] Total entries: %u\n", shared_data_get_count());
    pr_info("[yuuki] [parser] Path mappings: %u\n", shared_data_get_count_by_type(SHARED_DATA_TYPE_PATH_MAPPING));
    pr_info("[yuuki] [parser] Single paths: %u\n", shared_data_get_count_by_type(SHARED_DATA_TYPE_SINGLE_PATH));
    pr_info("[yuuki] [parser] PTRACE UIDs: %u\n", shared_data_get_count_by_type(SHARED_DATA_TYPE_PTRACE_UID));
    return PARSER_SUCCESS;
}

// 处理LIST类命令
int parser_handle_list_command(const char *arg1, const char *arg2) {
    enum shared_data_type type;
    unsigned int count, i;
    unsigned int global_index;
    struct shared_data_entry entry;
    int ret;

    if (strcmp(arg1, "TYPE") == 0 && strlen(arg2) > 0) {
        type = (enum shared_data_type)kstring_simple_strtoul(arg2, NULL, 10);
        if (type >= 1 && type <= 3) {
            count = shared_data_get_count_by_type(type);

            pr_info("[yuuki] [parser] === Type %d entries (%u total) ===\n", type, count);

            for (i = 0; i < count; i++) {
                ret = shared_data_get_type_entry_info(type, i, &global_index, &entry);
                if (ret == SHARED_DATA_SUCCESS) {
                    switch (type) {
                        case SHARED_DATA_TYPE_PATH_MAPPING:
                            pr_info("[yuuki] [parser] Type-index %u (global %u): MAP: %u:%s->%s\n", i, global_index,
                                    entry.data.mapping.uid,
                                    entry.data.mapping.src_path,
                                    entry.data.mapping.dst_path);
                            break;
                        case SHARED_DATA_TYPE_SINGLE_PATH:
                            pr_info("[yuuki] [parser] Type-index %u (global %u): PATH: %s\n", i, global_index,
                                    entry.data.single.path);
                            break;
                        case SHARED_DATA_TYPE_PTRACE_UID:
                            pr_info("[yuuki] [parser] Type-index %u (global %u): PTRACE: %u\n", i, global_index,
                                    entry.data.ptrace_uid.uid);
                            break;
                        default:
                            /* SHARED_DATA_TYPE_INVALID：上面已按 1..3 过滤 */
                            break;
                    }
                } else {
                    pr_err("[yuuki] [parser] Failed to get entry info for type %d, index %u: %d\n",
                           type, i, ret);
                }
            }
            return PARSER_SUCCESS;
        } else {
            pr_err("[yuuki] [parser] Invalid type: %d\n", type);
            return PARSER_ERR_PARAM;
        }
    }

    pr_err("[yuuki] [parser] Invalid LIST command format\n");
    return PARSER_ERR_PARAM;
}

// 显示帮助信息
void parser_show_help(void) {
    pr_info("[yuuki] [parser] === Available Commands ===\n");
    pr_info("[yuuki] [parser] CMD:STATUS - Show statistics\n");
    pr_info("[yuuki] [parser] CMD:DUMP - Print all data to dmesg\n");
    pr_info("[yuuki] [parser] CMD:DUMP:TYPE:N - Print type N data (1=MAP, 2=PATH, 3=PTRACE)\n");
    pr_info("[yuuki] [parser] CMD:LIST:TYPE:N - List type N entries with indices\n");
    pr_info("[yuuki] [parser] CMD:CLEAR - Clear all data\n");
    pr_info("[yuuki] [parser] CMD:CLEAR:TYPE:N - Clear all data of type N\n");
    pr_info("[yuuki] [parser] CMD:REMOVE:INDEX:N - Remove entry at global index N\n");
    pr_info("[yuuki] [parser] CMD:REMOVE:TYPE:type_id:type_index - Remove type_index-th entry of type_id\n");
    pr_info("[yuuki] [parser] CMD:HELP - Show this help\n");
    pr_info("[yuuki] [parser] === Data Formats ===\n");
    pr_info("[yuuki] [parser] MAP:uid:src_path:dst_path - Add path mapping\n");
    pr_info("[yuuki] [parser] PATH:path - Add single path\n");
    pr_info("[yuuki] [parser] PTRACE:uid - Add PTRACE UID\n");
}

// 主要的命令处理函数
int parser_process_command(const char *cmd_str) {
    char cmd[64] = {0};
    char arg1[64] = {0};
    char arg2[256] = {0};
    int ret;

    if (!cmd_str) {
        return PARSER_ERR_INVALID;
    }

    pr_info("[yuuki] [parser] Processing command: %s\n", cmd_str);

    ret = parser_parse_command_args(cmd_str, cmd, arg1, arg2);
    if (ret != PARSER_SUCCESS) {
        pr_err("[yuuki] [parser] Failed to parse command\n");
        return ret;
    }

    // 根据命令类型分发处理
    if (strcmp(cmd, "CLEAR") == 0) {
        return parser_handle_clear_command(arg1, arg2);
    }
    else if (strcmp(cmd, "REMOVE") == 0) {
        return parser_handle_remove_command(arg1, arg2);
    }
    else if (strcmp(cmd, "DUMP") == 0) {
        return parser_handle_dump_command(arg1, arg2);
    }
    else if (strcmp(cmd, "STATUS") == 0) {
        return parser_handle_status_command();
    }
    else if (strcmp(cmd, "LIST") == 0) {
        return parser_handle_list_command(arg1, arg2);
    }
    else if (strcmp(cmd, "HELP") == 0) {
        parser_show_help();
        return PARSER_SUCCESS;
    }

    pr_err("[yuuki] [parser] Unknown command: %s\n", cmd);
    return PARSER_ERR_UNKNOWN;
}
