#ifndef CK_TASKS_MODEL_H
#define CK_TASKS_MODEL_H

#include <sys/types.h>

typedef struct {
    char name[64];
    pid_t pid;
    double cpu_percent;
    double memory_mb;
    int threads;
    char user[32];
} TasksProcessEntry;

typedef struct {
    int cpu_percent;
    int memory_percent;
    int load1_percent;
    int load5_percent;
    int load15_percent;
} TasksSystemStats;

void tasks_model_initialize(void);
void tasks_model_shutdown(void);

int tasks_model_list_processes(TasksProcessEntry **out_entries, int *out_count, int max_entries);
void tasks_model_free_processes(TasksProcessEntry *entries, int count);
int tasks_model_get_system_stats(TasksSystemStats *out_stats);

#endif /* CK_TASKS_MODEL_H */
