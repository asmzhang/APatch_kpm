#ifndef MEMORY_CRC_PTRACE_HIDE_H
#define MEMORY_CRC_PTRACE_HIDE_H

long ptrace_hide_init(void);
long ptrace_hide_exit(void);

struct seq_file {
    char *buf;
    size_t size;
    size_t from;
    size_t count;
    //...
};

/*extern int (*ori_proc_pid_status)(struct seq_file *m, struct pid_namespace *ns, struct pid *pid, struct task_struct *task);
extern int (*ori_do_task_stat)(struct seq_file *m, struct pid_namespace *ns, struct pid *pid, struct task_struct *task, int whole);*/
extern void* ori_proc_pid_status;
extern void* ori_do_task_stat;

extern void *(*kf_vmalloc)(unsigned long size);
extern void (*kf_vfree)(const void *addr);

#endif
