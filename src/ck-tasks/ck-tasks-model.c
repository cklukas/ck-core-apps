#include "ck-tasks-model.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <utmp.h>

static long g_clock_ticks = 0;
static int g_cpu_count = 1;
static unsigned long long g_prev_cpu_total = 0;
static unsigned long long g_prev_cpu_idle = 0;

typedef struct {
    pid_t pid;
    unsigned long long total_ticks;
    double last_uptime;
    int seen;
} ProcSample;

static ProcSample *g_proc_samples = NULL;
static int g_proc_samples_count = 0;
static int g_proc_samples_capacity = 0;

static int is_pid_dir(const char *name)
{
    if (!name || !name[0]) return 0;
    for (const char *p = name; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

static int read_proc_stat(pid_t pid, char *name, size_t name_len,
                          unsigned long long *utime, unsigned long long *stime,
                          int *threads)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char comm[64] = {0};
    char state = 0;
    unsigned long long ut = 0, st = 0;
    long num_threads = 0;

    int scanned = fscanf(fp,
                         "%*d (%63[^)]) %c "
                         "%*d %*d %*d %*d %*d "
                         "%*u %*u %*u %*u %*u "
                         "%llu %llu "
                         "%*d %*d %*d %*d %ld",
                         comm, &state, &ut, &st, &num_threads);
    fclose(fp);
    if (scanned < 5) return -1;

    if (name && name_len > 0) {
        (void)snprintf(name, name_len, "%s", comm);
    }
    if (utime) *utime = ut;
    if (stime) *stime = st;
    if (threads) *threads = (int)num_threads;
    return 0;
}

static int read_proc_status_uid(pid_t pid, uid_t *out_uid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[256];
    uid_t uid = 0;
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned int real_uid = 0;
            if (sscanf(line, "Uid:\t%u", &real_uid) == 1) {
                uid = (uid_t)real_uid;
                found = 1;
                break;
            }
        }
    }
    fclose(fp);
    if (!found) return -1;
    if (out_uid) *out_uid = uid;
    return 0;
}

static void read_proc_cmdline(pid_t pid, char *buffer, size_t len)
{
    if (!buffer || len == 0) return;
    buffer[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    size_t read = fread(buffer, 1, len - 1, fp);
    fclose(fp);
    if (read == 0) {
        buffer[0] = '\0';
        return;
    }
    for (size_t i = 0; i < read; ++i) {
        if (buffer[i] == '\0') {
            buffer[i] = ' ';
        }
    }
    buffer[read] = '\0';
}

static int read_proc_rss_mb(pid_t pid, double *out_mb)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    unsigned long size = 0;
    unsigned long rss = 0;
    int scanned = fscanf(fp, "%lu %lu", &size, &rss);
    fclose(fp);
    if (scanned < 2) return -1;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    if (out_mb) {
        *out_mb = (double)rss * (double)page_size / (1024.0 * 1024.0);
    }
    return 0;
}

static double read_uptime_seconds(void)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) return 0.0;
    double uptime = 0.0;
    if (fscanf(fp, "%lf", &uptime) != 1) uptime = 0.0;
    fclose(fp);
    return uptime;
}

static int read_cpu_totals(unsigned long long *out_total, unsigned long long *out_idle)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    char line[256];
    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    int scanned = sscanf(line, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                         &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (scanned < 4) return -1;
    unsigned long long idle_all = idle + iowait;
    unsigned long long non_idle = user + nice + system + irq + softirq + steal;
    unsigned long long total = idle_all + non_idle;
    if (out_total) *out_total = total;
    if (out_idle) *out_idle = idle_all;
    return 0;
}

static ProcSample *get_proc_sample(pid_t pid, int create)
{
    for (int i = 0; i < g_proc_samples_count; ++i) {
        if (g_proc_samples[i].pid == pid) {
            return &g_proc_samples[i];
        }
    }
    if (!create) return NULL;
    if (g_proc_samples_count >= g_proc_samples_capacity) {
        int new_capacity = g_proc_samples_capacity ? g_proc_samples_capacity * 2 : 256;
        ProcSample *resized = (ProcSample *)realloc(g_proc_samples, sizeof(ProcSample) * new_capacity);
        if (!resized) return NULL;
        g_proc_samples = resized;
        g_proc_samples_capacity = new_capacity;
    }
    ProcSample *sample = &g_proc_samples[g_proc_samples_count++];
    sample->pid = pid;
    sample->total_ticks = 0;
    sample->last_uptime = 0.0;
    sample->seen = 0;
    return sample;
}

void tasks_model_initialize(void)
{
    g_clock_ticks = sysconf(_SC_CLK_TCK);
    if (g_clock_ticks <= 0) g_clock_ticks = 100;
    g_cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_cpu_count <= 0) g_cpu_count = 1;
    read_cpu_totals(&g_prev_cpu_total, &g_prev_cpu_idle);
}

void tasks_model_shutdown(void)
{
    free(g_proc_samples);
    g_proc_samples = NULL;
    g_proc_samples_count = 0;
    g_proc_samples_capacity = 0;
}

int tasks_model_list_processes(TasksProcessEntry **out_entries, int *out_count)
{
    if (!out_entries || !out_count) return -1;
    DIR *dir = opendir("/proc");
    if (!dir) return -1;

    TasksProcessEntry *entries = NULL;
    int capacity = 0;
    int count = 0;
    double uptime = read_uptime_seconds();
    for (int i = 0; i < g_proc_samples_count; ++i) {
        g_proc_samples[i].seen = 0;
    }
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_pid_dir(ent->d_name)) continue;
        pid_t pid = (pid_t)atoi(ent->d_name);
        TasksProcessEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.pid = pid;
        unsigned long long ut = 0, st = 0;
        if (read_proc_stat(pid, entry.name, sizeof(entry.name), &ut, &st, &entry.threads) != 0) {
            continue;
        }
        uid_t uid = (uid_t)-1;
        int have_uid = (read_proc_status_uid(pid, &uid) == 0);
        if (have_uid) {
            struct passwd *pw = getpwuid(uid);
            if (pw && pw->pw_name) {
                strncpy(entry.user, pw->pw_name, sizeof(entry.user) - 1);
                entry.user[sizeof(entry.user) - 1] = '\0';
            }
        }
        if (entry.user[0] == '\0' && have_uid) {
            snprintf(entry.user, sizeof(entry.user), "%d", (int)uid);
        }
        if (entry.user[0] == '\0') {
            snprintf(entry.user, sizeof(entry.user), "unknown");
        }
        read_proc_cmdline(pid, entry.command, sizeof(entry.command));
        if (entry.command[0] == '\0') {
            snprintf(entry.command, sizeof(entry.command), "%s", entry.name);
        }
        read_proc_rss_mb(pid, &entry.memory_mb);
        unsigned long long total_ticks = ut + st;
        double cpu_percent = 0.0;
        if (uptime > 0.0 && g_clock_ticks > 0) {
            ProcSample *sample = get_proc_sample(pid, 1);
            if (sample && sample->last_uptime > 0.0 && uptime > sample->last_uptime &&
                total_ticks >= sample->total_ticks) {
                double delta_ticks = (double)(total_ticks - sample->total_ticks);
                double delta_seconds = delta_ticks / (double)g_clock_ticks;
                double interval = uptime - sample->last_uptime;
                if (interval > 0.0) {
                    cpu_percent = (delta_seconds / interval) * 100.0;
                }
            } else {
                double total_time = (double)total_ticks / (double)g_clock_ticks;
                cpu_percent = (total_time / uptime) * 100.0;
            }
            if (sample) {
                sample->total_ticks = total_ticks;
                sample->last_uptime = uptime;
                sample->seen = 1;
            }
        }
        entry.cpu_percent = cpu_percent;
        if (count >= capacity) {
            int new_capacity = capacity ? capacity * 2 : 256;
            TasksProcessEntry *resized = (TasksProcessEntry *)realloc(entries, sizeof(TasksProcessEntry) * new_capacity);
            if (!resized) {
                free(entries);
                closedir(dir);
                return -1;
            }
            entries = resized;
            capacity = new_capacity;
        }
        entries[count++] = entry;
    }
    closedir(dir);
    if (g_proc_samples_count > 0) {
        int write_index = 0;
        for (int i = 0; i < g_proc_samples_count; ++i) {
            if (!g_proc_samples[i].seen) continue;
            if (write_index != i) {
                g_proc_samples[write_index] = g_proc_samples[i];
            }
            write_index++;
        }
        g_proc_samples_count = write_index;
    }

    *out_entries = entries;
    *out_count = count;
    return 0;
}

void tasks_model_free_processes(TasksProcessEntry *entries, int count)
{
    (void)count;
    free(entries);
}

int tasks_model_get_system_stats(TasksSystemStats *out_stats)
{
    if (!out_stats) return -1;
    unsigned long long total = 0;
    unsigned long long idle = 0;
    if (read_cpu_totals(&total, &idle) != 0) return -1;

    unsigned long long total_diff = total - g_prev_cpu_total;
    unsigned long long idle_diff = idle - g_prev_cpu_idle;
    g_prev_cpu_total = total;
    g_prev_cpu_idle = idle;
    int cpu_percent = 0;
    if (total_diff > 0) {
        cpu_percent = (int)((double)(total_diff - idle_diff) / (double)total_diff * 100.0);
    }

    FILE *fp = fopen("/proc/meminfo", "r");
    unsigned long mem_total = 0;
    unsigned long mem_available = 0;
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) continue;
            if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) continue;
        }
        fclose(fp);
    }
    unsigned long mem_used = 0;
    int mem_percent = 0;
    if (mem_total > 0) {
        mem_used = mem_total > mem_available ? (mem_total - mem_available) : 0;
        mem_percent = (int)((double)mem_used / (double)mem_total * 100.0);
    }

    double load1 = 0.0, load5 = 0.0, load15 = 0.0;
    fp = fopen("/proc/loadavg", "r");
    if (fp) {
        fscanf(fp, "%lf %lf %lf", &load1, &load5, &load15);
        fclose(fp);
    }
    double scale = 100.0 / (double)g_cpu_count;
    int load1p = (int)(load1 * scale);
    int load5p = (int)(load5 * scale);
    int load15p = (int)(load15 * scale);

    out_stats->cpu_percent = cpu_percent;
    out_stats->memory_percent = mem_percent;
    out_stats->load1_percent = load1p;
    out_stats->load5_percent = load5p;
    out_stats->load15_percent = load15p;
    out_stats->mem_total_kb = mem_total;
    out_stats->mem_used_kb = mem_used;
    return 0;
}

#define TASKS_USER_UNKNOWN_TEXT "(unknown)"
#define TASKS_USER_LOCAL_TEXT "(local)"
#define TASKS_USER_IDLE_NA_TEXT "n/a"

static void tasks_model_format_login_time(char *buffer, size_t len, time_t seconds)
{
    if (!buffer || len == 0) return;
    if (seconds <= 0) {
        snprintf(buffer, len, "%s", TASKS_USER_UNKNOWN_TEXT);
        buffer[len - 1] = '\0';
        return;
    }
    struct tm tm_storage;
    struct tm *tm_info = localtime_r(&seconds, &tm_storage);
    if (!tm_info) {
        snprintf(buffer, len, "%s", TASKS_USER_UNKNOWN_TEXT);
        buffer[len - 1] = '\0';
        return;
    }
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", tm_info);
}

static long long tasks_model_get_idle_seconds(const char *line)
{
    if (!line || line[0] == '\0') return -1;
    char path[128] = {0};
    if (line[0] == '/') {
        snprintf(path, sizeof(path), "%s", line);
    } else {
        snprintf(path, sizeof(path), "/dev/%s", line);
    }
    struct stat st = {0};
    if (stat(path, &st) != 0) return -1;
    time_t now = time(NULL);
    if (now < st.st_atime) return 0;
    return (long long)(now - st.st_atime);
}

static void tasks_model_format_idle_time(char *buffer, size_t len, long long seconds)
{
    if (!buffer || len == 0) return;
    if (seconds < 0) {
        snprintf(buffer, len, "%s", TASKS_USER_IDLE_NA_TEXT);
        buffer[len - 1] = '\0';
        return;
    }
    long long hours = seconds / 3600;
    long long minutes = (seconds % 3600) / 60;
    long long secs = seconds % 60;
    if (hours > 0) {
        snprintf(buffer, len, "%lluh %02llum", hours, minutes);
    } else if (minutes > 0) {
        snprintf(buffer, len, "%llum %02llus", minutes, secs);
    } else {
        snprintf(buffer, len, "%llus", secs);
    }
    buffer[len - 1] = '\0';
}

static void tasks_model_copy_string(char *dst, const char *src, size_t len, const char *fallback)
{
    if (!dst || len == 0) return;
    if (src && src[0]) {
        strncpy(dst, src, len - 1);
        dst[len - 1] = '\0';
        return;
    }
    if (fallback) {
        strncpy(dst, fallback, len - 1);
        dst[len - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

int tasks_model_list_users(TasksUserEntry **out_entries, int *out_count)
{
    if (!out_entries || !out_count) return -1;
    TasksUserEntry *entries = NULL;
    int capacity = 0;
    int count = 0;
    int result = 0;

    setutent();
    struct utmp *ut = NULL;
    while ((ut = getutent()) != NULL) {
        if (ut->ut_type != USER_PROCESS) continue;
        if (count >= capacity) {
            int new_capacity = capacity ? capacity * 2 : 32;
            TasksUserEntry *resized = (TasksUserEntry *)realloc(entries, sizeof(TasksUserEntry) * new_capacity);
            if (!resized) {
                result = -1;
                break;
            }
            entries = resized;
            capacity = new_capacity;
        }
        TasksUserEntry *entry = &entries[count];
        memset(entry, 0, sizeof(*entry));
        tasks_model_copy_string(entry->user, ut->ut_user, sizeof(entry->user), TASKS_USER_UNKNOWN_TEXT);
        tasks_model_copy_string(entry->tty, ut->ut_line, sizeof(entry->tty), TASKS_USER_UNKNOWN_TEXT);
        tasks_model_copy_string(entry->host, ut->ut_host, sizeof(entry->host), TASKS_USER_LOCAL_TEXT);
        time_t login_seconds = ut->ut_time;
        tasks_model_format_login_time(entry->login_time, sizeof(entry->login_time), login_seconds);
        long long idle_seconds = tasks_model_get_idle_seconds(ut->ut_line);
        tasks_model_format_idle_time(entry->idle_time, sizeof(entry->idle_time), idle_seconds);
        entry->pid = ut->ut_pid;
        count++;
    }
    endutent();

    if (result != 0) {
        free(entries);
        entries = NULL;
        count = 0;
    }

    *out_entries = entries;
    *out_count = count;
    return result;
}

void tasks_model_free_users(TasksUserEntry *entries, int count)
{
    (void)count;
    free(entries);
}
