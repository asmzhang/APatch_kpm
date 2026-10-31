#ifndef MEMORY_CRC_PARSER_H
#define MEMORY_CRC_PARSER_H

#include "shared_data.h"

// 命令处理结果
#define PARSER_SUCCESS          0
#define PARSER_ERR_INVALID     (-1)
#define PARSER_ERR_UNKNOWN     (-2)
#define PARSER_ERR_PARAM       (-3)
#define PARSER_ERR_OPERATION   (-4)


bool parser_is_command(const char *str);

int parser_process_command(const char *cmd_str);

int parser_parse_command_args(const char *cmd_str, char *cmd, char *arg1, char *arg2);

void parser_show_help(void);

int parser_handle_clear_command(const char *arg1, const char *arg2);

int parser_handle_remove_command(const char *arg1, const char *arg2);

int parser_handle_dump_command(const char *arg1, const char *arg2);

int parser_handle_list_command(const char *arg1, const char *arg2);

int parser_handle_status_command(void);


#endif
