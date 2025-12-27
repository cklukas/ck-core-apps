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
        entry->idle_seconds = idle_seconds;
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

static int tasks_model_path_exists(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    return stat(path, &st) == 0;
}

static int tasks_model_is_dir(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static void tasks_model_set_init_info(TasksInitInfo *info, const char *name, const char *detail)
{
    if (!info) return;
    char name_buf[sizeof(info->init_name)];
    char detail_buf[sizeof(info->init_detail)];
    name_buf[0] = '\0';
    detail_buf[0] = '\0';

    if (name) {
        strncpy(name_buf, name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
    }
    if (detail) {
        strncpy(detail_buf, detail, sizeof(detail_buf) - 1);
        detail_buf[sizeof(detail_buf) - 1] = '\0';
    }

    strncpy(info->init_name, name_buf, sizeof(info->init_name) - 1);
    info->init_name[sizeof(info->init_name) - 1] = '\0';
    strncpy(info->init_detail, detail_buf, sizeof(info->init_detail) - 1);
    info->init_detail[sizeof(info->init_detail) - 1] = '\0';
}

static void tasks_model_rstrip(char *value)
{
    if (!value) return;
    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) {
        value[len - 1] = '\0';
        len--;
    }
}

static char *tasks_model_lstrip(char *value)
{
    if (!value) return value;
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    return value;
}

static TasksServiceInfoField *tasks_model_add_service_info(TasksServiceEntry *entry,
                                                           const char *key,
                                                           const char *value)
{
    if (!entry || !key || !key[0]) return NULL;
    if (entry->info_count >= TASKS_SERVICE_INFO_MAX_FIELDS) return NULL;
    TasksServiceInfoField *field = &entry->info_fields[entry->info_count++];
    memset(field, 0, sizeof(*field));
    snprintf(field->key, sizeof(field->key), "%s", key);
    if (value && value[0]) {
        snprintf(field->value, sizeof(field->value), "%s", value);
    }
    return field;
}

static void tasks_model_append_service_info(TasksServiceInfoField *field, const char *value)
{
    if (!field || !value || !value[0]) return;
    size_t current_len = strlen(field->value);
    size_t remaining = sizeof(field->value) - current_len - 1;
    if (remaining == 0) return;
    if (current_len > 0 && remaining > 1) {
        strncat(field->value, " ", remaining);
        current_len = strlen(field->value);
        remaining = sizeof(field->value) - current_len - 1;
    }
    strncat(field->value, value, remaining);
}

static void tasks_model_parse_init_info(const char *path, TasksServiceEntry *entry)
{
    if (!path || !path[0] || !entry) return;
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[512];
    int in_block = 0;
    TasksServiceInfoField *current = NULL;
    while (fgets(line, sizeof(line), fp)) {
        if (!in_block) {
            if (strstr(line, "### BEGIN INIT INFO")) {
                in_block = 1;
            }
            continue;
        }
        if (strstr(line, "### END INIT INFO")) break;
        char *hash = strchr(line, '#');
        if (!hash) continue;
        char *content = hash + 1;
        content = tasks_model_lstrip(content);
        tasks_model_rstrip(content);
        if (!content[0]) continue;
        char *colon = strchr(content, ':');
        if (colon) {
            *colon = '\0';
            char *key = content;
            tasks_model_rstrip(key);
            char *value = tasks_model_lstrip(colon + 1);
            tasks_model_rstrip(value);
            current = tasks_model_add_service_info(entry, key, value);
        } else if (current) {
            tasks_model_append_service_info(current, content);
        }
    }
    fclose(fp);
}

static void tasks_model_set_service_path(char *dest, size_t dest_len, const char *path)
{
    if (!dest || dest_len == 0) return;
    if (path && path[0]) {
        snprintf(dest, dest_len, "%s", path);
        dest[dest_len - 1] = '\0';
    } else {
        dest[0] = '\0';
    }
}

static void tasks_model_resolve_symlink_path(const char *dir, const char *link_target,
                                             char *out_path, size_t out_len)
{
    if (!out_path || out_len == 0) return;
    out_path[0] = '\0';
    if (!link_target || !link_target[0]) return;
    char combined[PATH_MAX];
    if (link_target[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", link_target);
    } else if (dir && dir[0]) {
        snprintf(combined, sizeof(combined), "%s/%s", dir, link_target);
    } else {
        snprintf(combined, sizeof(combined), "%s", link_target);
    }
    combined[sizeof(combined) - 1] = '\0';
    char resolved[PATH_MAX];
    if (realpath(combined, resolved)) {
        tasks_model_set_service_path(out_path, out_len, resolved);
    } else {
        tasks_model_set_service_path(out_path, out_len, combined);
    }
}

static void tasks_model_safe_copy(char *dest, size_t dest_len, const char *src)
{
    if (!dest || dest_len == 0) return;
    if (src && src[0]) {
        size_t max_copy = dest_len - 1;
        size_t copy_len = strnlen(src, max_copy);
        if (copy_len > 0) {
            memcpy(dest, src, copy_len);
        }
        dest[copy_len] = '\0';
    } else {
        dest[0] = '\0';
    }
}

static int tasks_model_read_file_line(const char *path, char *buffer, size_t len)
{
    if (!path || !buffer || len == 0) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buffer, len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    buffer[len - 1] = '\0';
    size_t l = strlen(buffer);
    while (l > 0 && (buffer[l - 1] == '\n' || buffer[l - 1] == '\r')) {
        buffer[l - 1] = '\0';
        l--;
    }
    return 0;
}

static int tasks_model_get_runlevel(char *out_level, size_t out_len)
{
    if (!out_level || out_len == 0) return -1;
    out_level[0] = '\0';
#ifndef RUN_LVL
    return -1;
#else
    setutent();
    struct utmp *ut = NULL;
    while ((ut = getutent()) != NULL) {
        if (ut->ut_type != RUN_LVL) continue;
        unsigned int raw = (unsigned int)ut->ut_pid;
        unsigned int lvl = raw & 0xffU;
        if (lvl <= 6U) {
            snprintf(out_level, out_len, "%u", lvl);
        } else if (lvl >= (unsigned int)'0' && lvl <= (unsigned int)'6') {
            snprintf(out_level, out_len, "%c", (int)lvl);
        } else {
            snprintf(out_level, out_len, "%u", lvl);
        }
        break;
    }
    endutent();
    return out_level[0] ? 0 : -1;
#endif
}

static int tasks_model_is_systemd(void)
{
    if (tasks_model_path_exists("/run/systemd/system")) return 1;
    char comm[64] = {0};
    if (tasks_model_read_file_line("/proc/1/comm", comm, sizeof(comm)) == 0) {
        if (strcmp(comm, "systemd") == 0) return 1;
    }
    return 0;
}

static void tasks_model_detect_systemd_target(TasksInitInfo *info)
{
    if (!info) return;
    const char *paths[] = {
        "/run/systemd/system/default.target",
        "/etc/systemd/system/default.target",
        "/usr/lib/systemd/system/default.target",
        "/lib/systemd/system/default.target",
    };
    char linkbuf[PATH_MAX] = {0};
    const char *base = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        ssize_t len = readlink(paths[i], linkbuf, sizeof(linkbuf) - 1);
        if (len <= 0) continue;
        linkbuf[len] = '\0';
        base = strrchr(linkbuf, '/');
        base = base ? base + 1 : linkbuf;
        break;
    }
    if (base && base[0]) {
        char detail[128];
        snprintf(detail, sizeof(detail), "target: %s", base);
        tasks_model_set_init_info(info, info->init_name, detail);
    }
}

typedef struct {
    char name[128];
    int running;
    int enabled;
} SystemdServiceInfo;

static int tasks_model_service_index(SystemdServiceInfo *items, int count, const char *name)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(items[i].name, name) == 0) return i;
    }
    return -1;
}

static void tasks_model_add_systemd_service(SystemdServiceInfo **items, int *count, int *cap,
                                            const char *name, int running, int enabled)
{
    if (!items || !count || !cap || !name || !name[0]) return;
    int idx = tasks_model_service_index(*items, *count, name);
    if (idx >= 0) {
        if (running) (*items)[idx].running = 1;
        if (enabled) (*items)[idx].enabled = 1;
        return;
    }
    if (*count >= *cap) {
        int new_cap = *cap ? (*cap * 2) : 64;
        SystemdServiceInfo *resized = (SystemdServiceInfo *)realloc(*items, sizeof(SystemdServiceInfo) * new_cap);
        if (!resized) return;
        *items = resized;
        *cap = new_cap;
    }
    SystemdServiceInfo *item = &(*items)[(*count)++];
    memset(item, 0, sizeof(*item));
    snprintf(item->name, sizeof(item->name), "%s", name);
    item->running = running ? 1 : 0;
    item->enabled = enabled ? 1 : 0;
}

static int tasks_model_is_service_file(const char *name)
{
    if (!name) return 0;
    size_t len = strlen(name);
    return len > 8 && strcmp(name + (len - 8), ".service") == 0;
}

static void tasks_model_collect_systemd_dir(const char *dir, int running,
                                            SystemdServiceInfo **items, int *count, int *cap)
{
    if (!dir || !items || !count || !cap) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!tasks_model_is_service_file(ent->d_name)) continue;
        char name[128];
        snprintf(name, sizeof(name), "%s", ent->d_name);
        char *suffix = strstr(name, ".service");
        if (suffix) *suffix = '\0';
        tasks_model_add_systemd_service(items, count, cap, name, running, 0);
    }
    closedir(dp);
}

static void tasks_model_collect_systemd_enabled(SystemdServiceInfo **items, int *count, int *cap)
{
    const char *base = "/etc/systemd/system";
    DIR *dp = opendir(base);
    if (!dp) return;
    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 6 || strcmp(ent->d_name + (len - 6), ".wants") != 0) continue;
        char wants_dir[PATH_MAX];
        snprintf(wants_dir, sizeof(wants_dir), "%s/%s", base, ent->d_name);
        DIR *wdp = opendir(wants_dir);
        if (!wdp) continue;
        struct dirent *went = NULL;
        while ((went = readdir(wdp)) != NULL) {
            if (went->d_name[0] == '.') continue;
            if (!tasks_model_is_service_file(went->d_name)) continue;
            char name[128];
            snprintf(name, sizeof(name), "%s", went->d_name);
            char *suffix = strstr(name, ".service");
            if (suffix) *suffix = '\0';
            tasks_model_add_systemd_service(items, count, cap, name, 0, 1);
        }
        closedir(wdp);
    }
    closedir(dp);
}

static int tasks_model_compare_service_name(const void *a, const void *b)
{
    const SystemdServiceInfo *sa = (const SystemdServiceInfo *)a;
    const SystemdServiceInfo *sb = (const SystemdServiceInfo *)b;
    return strcoll(sa->name, sb->name);
}

static int tasks_model_compare_service_entries(const void *a, const void *b)
{
    const TasksServiceEntry *sa = (const TasksServiceEntry *)a;
    const TasksServiceEntry *sb = (const TasksServiceEntry *)b;
    if (sa->order[0] && !sb->order[0]) return -1;
    if (!sa->order[0] && sb->order[0]) return 1;
    if (sa->order[0] && sb->order[0]) {
        int oa = atoi(sa->order);
        int ob = atoi(sb->order);
        if (oa != ob) return oa - ob;
    }
    return strcoll(sa->name, sb->name);
}

static int tasks_model_list_systemd_services(TasksServiceEntry **out_entries, int *out_count, TasksInitInfo *info)
{
    if (!out_entries || !out_count) return -1;
    SystemdServiceInfo *items = NULL;
    int count = 0;
    int cap = 0;
    tasks_model_collect_systemd_dir("/run/systemd/system", 1, &items, &count, &cap);
    tasks_model_collect_systemd_dir("/etc/systemd/system", 0, &items, &count, &cap);
    tasks_model_collect_systemd_dir("/usr/lib/systemd/system", 0, &items, &count, &cap);
    tasks_model_collect_systemd_dir("/lib/systemd/system", 0, &items, &count, &cap);
    tasks_model_collect_systemd_enabled(&items, &count, &cap);

    if (count > 1) {
        qsort(items, count, sizeof(SystemdServiceInfo), tasks_model_compare_service_name);
    }

    TasksServiceEntry *entries = NULL;
    if (count > 0) {
        entries = (TasksServiceEntry *)calloc((size_t)count, sizeof(TasksServiceEntry));
    }
    for (int i = 0; i < count; ++i) {
        TasksServiceEntry *entry = &entries[i];
        tasks_model_safe_copy(entry->name, sizeof(entry->name), items[i].name);
        entry->order[0] = '\0';
        if (items[i].running) {
            snprintf(entry->state, sizeof(entry->state), "running");
        } else if (items[i].enabled) {
            snprintf(entry->state, sizeof(entry->state), "enabled");
        } else {
            snprintf(entry->state, sizeof(entry->state), "disabled");
        }
    }
    free(items);

    tasks_model_set_init_info(info, "systemd", NULL);
    tasks_model_detect_systemd_target(info);

    *out_entries = entries;
    *out_count = count;
    return 0;
}

static int tasks_model_list_sysv_services(TasksServiceEntry **out_entries, int *out_count, TasksInitInfo *info,
                                         int include_disabled)
{
    if (!out_entries || !out_count) return -1;
    char runlevel[8] = {0};
    char detail[64] = {0};
    const char *init_name = "sysv";
    if (tasks_model_get_runlevel(runlevel, sizeof(runlevel)) == 0) {
        snprintf(detail, sizeof(detail), "runlevel %s", runlevel);
    }
    tasks_model_set_init_info(info, init_name, detail[0] ? detail : NULL);

    char rc_dir[PATH_MAX] = {0};
    if (runlevel[0]) {
        snprintf(rc_dir, sizeof(rc_dir), "/etc/rc%s.d", runlevel);
        if (!tasks_model_is_dir(rc_dir)) {
            snprintf(rc_dir, sizeof(rc_dir), "/etc/rc.d/rc%s.d", runlevel);
        }
    }
    int have_rc = runlevel[0] && tasks_model_is_dir(rc_dir);
    const char *fallback_dir = tasks_model_is_dir("/etc/init.d") ? "/etc/init.d" : NULL;

    TasksServiceEntry *entries = NULL;
    int count = 0;
    int cap = 0;
    char **enabled_names = NULL;
    int enabled_count = 0;
    int enabled_cap = 0;
    DIR *dp = NULL;
    if (have_rc) {
        dp = opendir(rc_dir);
        if (dp) {
            struct dirent *ent = NULL;
            while ((ent = readdir(dp)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                if (ent->d_name[0] != 'S') continue;
                if (!isdigit((unsigned char)ent->d_name[1])) continue;
                char rc_path[PATH_MAX];
                snprintf(rc_path, sizeof(rc_path), "%s/%s", rc_dir, ent->d_name);
                rc_path[sizeof(rc_path) - 1] = '\0';

                char linkbuf[PATH_MAX] = {0};
                ssize_t linklen = readlink(rc_path, linkbuf, sizeof(linkbuf) - 1);
                const char *name = NULL;
                if (linklen > 0) {
                    linkbuf[linklen] = '\0';
                    const char *base = strrchr(linkbuf, '/');
                    name = base ? base + 1 : linkbuf;
                }
                if (!name || !name[0]) {
                    const char *fallback_name = ent->d_name + 1;
                    while (*fallback_name && isdigit((unsigned char)*fallback_name)) ++fallback_name;
                    if (*fallback_name) name = fallback_name;
                }
                if (!name || !name[0]) continue;

                int order = atoi(ent->d_name + 1);
                if (count >= cap) {
                    int new_cap = cap ? cap * 2 : 64;
                    TasksServiceEntry *resized = (TasksServiceEntry *)realloc(entries, sizeof(TasksServiceEntry) * new_cap);
                    if (!resized) break;
                    entries = resized;
                    cap = new_cap;
                }
                TasksServiceEntry *entry = &entries[count++];
                memset(entry, 0, sizeof(*entry));
                snprintf(entry->order, sizeof(entry->order), "%d", order);
                tasks_model_safe_copy(entry->name, sizeof(entry->name), name);
                snprintf(entry->state, sizeof(entry->state), "enabled");
                tasks_model_set_service_path(entry->symlink_path, sizeof(entry->symlink_path), rc_path);
                if (linklen > 0) {
                    tasks_model_resolve_symlink_path(rc_dir, linkbuf,
                                                     entry->filename_path, sizeof(entry->filename_path));
                } else if (fallback_dir) {
                    char script_path[PATH_MAX];
                    snprintf(script_path, sizeof(script_path), "%s/%s", fallback_dir, entry->name);
                    script_path[sizeof(script_path) - 1] = '\0';
                    tasks_model_set_service_path(entry->filename_path, sizeof(entry->filename_path), script_path);
                }
                if (entry->filename_path[0]) {
                    tasks_model_parse_init_info(entry->filename_path, entry);
                }

                if (enabled_count >= enabled_cap) {
                    int new_cap = enabled_cap ? enabled_cap * 2 : 64;
                    char **resized = (char **)realloc(enabled_names, sizeof(char *) * new_cap);
                    if (!resized) break;
                    enabled_names = resized;
                    enabled_cap = new_cap;
                }
                enabled_names[enabled_count++] = strdup(entry->name);
            }
            closedir(dp);
        }
    }

    if (include_disabled && fallback_dir) {
        dp = opendir(fallback_dir);
        if (dp) {
            struct dirent *ent = NULL;
            while ((ent = readdir(dp)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char script_path[PATH_MAX];
                snprintf(script_path, sizeof(script_path), "%s/%s", fallback_dir, ent->d_name);
                script_path[sizeof(script_path) - 1] = '\0';

                struct stat st = {0};
                if (stat(script_path, &st) != 0) continue;
                if (!S_ISREG(st.st_mode)) continue;
                if (access(script_path, X_OK) != 0) continue;
                int already_enabled = 0;
                for (int i = 0; i < enabled_count; ++i) {
                    if (enabled_names[i] && strcmp(enabled_names[i], ent->d_name) == 0) {
                        already_enabled = 1;
                        break;
                    }
                }
                if (already_enabled) continue;
                if (count >= cap) {
                    int new_cap = cap ? cap * 2 : 64;
                    TasksServiceEntry *resized = (TasksServiceEntry *)realloc(entries, sizeof(TasksServiceEntry) * new_cap);
                    if (!resized) break;
                    entries = resized;
                    cap = new_cap;
                }
                TasksServiceEntry *entry = &entries[count++];
                memset(entry, 0, sizeof(*entry));
                entry->order[0] = '\0';
                tasks_model_safe_copy(entry->name, sizeof(entry->name), ent->d_name);
                snprintf(entry->state, sizeof(entry->state), "disabled");
                tasks_model_set_service_path(entry->filename_path, sizeof(entry->filename_path), script_path);
                entry->symlink_path[0] = '\0';
                tasks_model_parse_init_info(entry->filename_path, entry);
            }
            closedir(dp);
        }
    }

    if (count > 1) {
        qsort(entries, count, sizeof(TasksServiceEntry), tasks_model_compare_service_entries);
    }

    for (int i = 0; i < enabled_count; ++i) {
        free(enabled_names[i]);
    }
    free(enabled_names);

    *out_entries = entries;
    *out_count = count;
    return 0;
}

static int tasks_model_list_bsd_services(TasksServiceEntry **out_entries, int *out_count, TasksInitInfo *info)
{
    if (!out_entries || !out_count) return -1;
    tasks_model_set_init_info(info, "bsd rc", NULL);

    const char *dir = "/etc/rc.d";
    if (!tasks_model_is_dir(dir)) {
        *out_entries = NULL;
        *out_count = 0;
        return 0;
    }
    DIR *dp = opendir(dir);
    if (!dp) {
        *out_entries = NULL;
        *out_count = 0;
        return 0;
    }
    TasksServiceEntry *entries = NULL;
    int count = 0;
    int cap = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (count >= cap) {
            int new_cap = cap ? cap * 2 : 64;
            TasksServiceEntry *resized = (TasksServiceEntry *)realloc(entries, sizeof(TasksServiceEntry) * new_cap);
            if (!resized) break;
            entries = resized;
            cap = new_cap;
        }
        TasksServiceEntry *entry = &entries[count++];
        memset(entry, 0, sizeof(*entry));
        entry->order[0] = '\0';
        entry->state[0] = '\0';
        tasks_model_safe_copy(entry->name, sizeof(entry->name), ent->d_name);
        char script_path[PATH_MAX];
        snprintf(script_path, sizeof(script_path), "%s/%s", dir, ent->d_name);
        script_path[sizeof(script_path) - 1] = '\0';
        tasks_model_set_service_path(entry->filename_path, sizeof(entry->filename_path), script_path);
        tasks_model_parse_init_info(entry->filename_path, entry);
    }
    closedir(dp);

    if (count > 1) {
        qsort(entries, count, sizeof(TasksServiceEntry), tasks_model_compare_service_entries);
    }
    *out_entries = entries;
    *out_count = count;
    return 0;
}

int tasks_model_list_services(TasksServiceEntry **out_entries, int *out_count, TasksInitInfo *out_info,
                              int include_disabled_sysv)
{
    if (!out_entries || !out_count) return -1;
    if (out_info) {
        out_info->init_name[0] = '\0';
        out_info->init_detail[0] = '\0';
    }
    if (tasks_model_is_systemd()) {
        return tasks_model_list_systemd_services(out_entries, out_count, out_info);
    }
    if (tasks_model_is_dir("/etc/rc.d") && tasks_model_path_exists("/etc/rc.conf")) {
        return tasks_model_list_bsd_services(out_entries, out_count, out_info);
    }
    return tasks_model_list_sysv_services(out_entries, out_count, out_info, include_disabled_sysv);
}

void tasks_model_free_services(TasksServiceEntry *entries, int count)
{
    (void)count;
    free(entries);
}
