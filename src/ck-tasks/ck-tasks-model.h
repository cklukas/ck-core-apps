#ifndef CK_TASKS_MODEL_H
#define CK_TASKS_MODEL_H

#include <sys/types.h>
#include <limits.h>

typedef struct {
    char name[64];
    pid_t pid;
    double cpu_percent;
    double memory_mb;
    int threads;
    char user[32];
    char command[PATH_MAX];
} TasksProcessEntry;

typedef struct {
    int cpu_percent;
    int memory_percent;
    int load1_percent;
    int load5_percent;
    int load15_percent;
    unsigned long mem_total_kb;
    unsigned long mem_used_kb;
} TasksSystemStats;

typedef struct {
    char user[32];
    char tty[32];
    char host[64];
    char login_time[64];
    char idle_time[32];
    long long idle_seconds;
    pid_t pid;
} TasksUserEntry;

void tasks_model_initialize(void);
void tasks_model_shutdown(void);

int tasks_model_list_processes(TasksProcessEntry **out_entries, int *out_count);
void tasks_model_free_processes(TasksProcessEntry *entries, int count);
int tasks_model_get_system_stats(TasksSystemStats *out_stats);
int tasks_model_list_users(TasksUserEntry **out_entries, int *out_count);
void tasks_model_free_users(TasksUserEntry *entries, int count);

#endif /* CK_TASKS_MODEL_H */
