#include "ck-tasks-model.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long g_clock_ticks = 0;
static int g_cpu_count = 1;
static unsigned long long g_prev_cpu_total = 0;
static unsigned long long g_prev_cpu_idle = 0;

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
}

int tasks_model_list_processes(TasksProcessEntry **out_entries, int *out_count, int max_entries)
{
    if (!out_entries || !out_count || max_entries <= 0) return -1;
    DIR *dir = opendir("/proc");
    if (!dir) return -1;

    TasksProcessEntry *entries = (TasksProcessEntry *)calloc(max_entries, sizeof(TasksProcessEntry));
    if (!entries) {
        closedir(dir);
        return -1;
    }

    double uptime = read_uptime_seconds();
    int count = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL && count < max_entries) {
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
        read_proc_rss_mb(pid, &entry.memory_mb);
        if (uptime > 0.0) {
            double total_time = (double)(ut + st) / (double)g_clock_ticks;
            entry.cpu_percent = (total_time / uptime) * 100.0;
        }
        entries[count++] = entry;
    }
    closedir(dir);

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
    int mem_percent = 0;
    if (mem_total > 0) {
        unsigned long used = mem_total > mem_available ? (mem_total - mem_available) : 0;
        mem_percent = (int)((double)used / (double)mem_total * 100.0);
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
    return 0;
}
