#include "browser_app.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <include/cef_command_line.h>

extern "C" {
#include "../shared/session_utils.h"
}

// Forward declaration of the legacy runner until it is migrated into BrowserApp.
int ck_browser_run(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    return BrowserApp::instance().run(argc, argv);
}

BrowserApp &BrowserApp::instance()
{
    static BrowserApp app;
    return app;
}

int BrowserApp::run(int argc, char *argv[])
{
    return run_main(argc, argv);
}

int BrowserApp::run_main(int argc, char *argv[])
{
    return ck_browser_run(argc, argv);
}

bool BrowserApp::build_path_from_dir(const char *dir, const char *suffix, char *buffer, size_t buffer_len) const
{
    if (!buffer || buffer_len == 0) return false;
    buffer[0] = '\0';
    if (!dir || dir[0] == '\0') return false;
    size_t dir_len = strnlen(dir, PATH_MAX);
    if (suffix && suffix[0]) {
        size_t suffix_len = strlen(suffix);
        if (dir_len + 1 + suffix_len + 1 > buffer_len) {
            return false;
        }
        memcpy(buffer, dir, dir_len);
        buffer[dir_len] = '/';
        memcpy(buffer + dir_len + 1, suffix, suffix_len);
        buffer[dir_len + 1 + suffix_len] = '\0';
    } else {
        if (dir_len + 1 > buffer_len) {
            return false;
        }
        memcpy(buffer, dir, dir_len);
        buffer[dir_len] = '\0';
    }
    return true;
}

void BrowserApp::build_cwd_path(char *buffer, size_t buffer_len, const char *suffix) const
{
    if (!buffer || buffer_len == 0) return;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        buffer[0] = '\0';
        return;
    }
    if (!build_path_from_dir(cwd, suffix, buffer, buffer_len)) {
        buffer[0] = '\0';
    }
}

bool BrowserApp::dir_has_files(const char *path) const
{
    if (!path || path[0] == '\0') return false;
    DIR *dir = opendir(path);
    if (!dir) return false;
    struct dirent *entry = NULL;
    bool has_files = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        has_files = true;
        break;
    }
    closedir(dir);
    return has_files;
}

bool BrowserApp::file_exists(const char *path) const
{
    if (!path || path[0] == '\0') return false;
    return access(path, R_OK) == 0;
}

void BrowserApp::get_exe_path(char *buffer, size_t buffer_len) const
{
    if (!buffer || buffer_len == 0) return;
    ssize_t len = readlink("/proc/self/exe", buffer, buffer_len - 1);
    if (len < 0) {
        buffer[0] = '\0';
        return;
    }
    buffer[len] = '\0';
}

bool BrowserApp::find_existing_path(char *buffer, size_t buffer_len, const char *suffix) const
{
    if (!buffer || buffer_len == 0 || !suffix || suffix[0] == '\0') return false;
    buffer[0] = '\0';
    char candidate[PATH_MAX];
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        if (build_path_from_dir(cwd, suffix, candidate, sizeof(candidate)) &&
            dir_has_files(candidate)) {
            strncpy(buffer, candidate, buffer_len);
            buffer[buffer_len - 1] = '\0';
            return true;
        }
    }
    char exe_path[PATH_MAX];
    get_exe_path(exe_path, sizeof(exe_path));
    if (exe_path[0] != '\0') {
        std::filesystem::path parent = std::filesystem::path(exe_path).parent_path();
        std::filesystem::path previous;
        while (!parent.empty() && parent != previous) {
            std::string base = parent.string();
            if (build_path_from_dir(base.c_str(), suffix, candidate, sizeof(candidate)) &&
                dir_has_files(candidate)) {
                strncpy(buffer, candidate, buffer_len);
                buffer[buffer_len - 1] = '\0';
                return true;
            }
            previous = parent;
            parent = parent.parent_path();
        }
    }
    return false;
}

BrowserPaths BrowserApp::discover_cef_paths() const
{
    BrowserPaths paths;
    char resources_path[PATH_MAX];
    if (!find_existing_path(resources_path, sizeof(resources_path),
                            "third_party/cef/resources")) {
        build_cwd_path(resources_path, sizeof(resources_path), "third_party/cef/resources");
    }
    if (resources_path[0]) {
        paths.resources_path = resources_path;
    }

    char locales_path[PATH_MAX];
    if (!find_existing_path(locales_path, sizeof(locales_path),
                            "third_party/cef/locales")) {
        build_cwd_path(locales_path, sizeof(locales_path), "third_party/cef/locales");
    }
    if (!dir_has_files(locales_path)) {
        if (!find_existing_path(locales_path, sizeof(locales_path),
                                "third_party/cef/resources/locales")) {
            build_cwd_path(locales_path, sizeof(locales_path),
                           "third_party/cef/resources/locales");
        }
    }
    if (locales_path[0]) {
        paths.locales_path = locales_path;
    }

    char exe_path[PATH_MAX];
    get_exe_path(exe_path, sizeof(exe_path));
    if (exe_path[0]) {
        paths.subprocess_path = exe_path;
    }

    return paths;
}

void BrowserApp::report_cef_resource_status(const char *resources_path, const char *locales_path) const
{
    const char *resource_dir = resources_path ? resources_path : "(null)";
    const char *locales_dir = locales_path ? locales_path : "(null)";
    fprintf(stderr, "[ck-browser] CEF resources_dir_path=%s\n", resource_dir);
    fprintf(stderr, "[ck-browser] CEF locales_dir_path=%s\n", locales_dir);
    if (resources_path && resources_path[0]) {
        char icu_path[PATH_MAX];
        size_t res_len = strnlen(resources_path, sizeof(icu_path));
        const char *suffix = "/icudtl.dat";
        size_t suffix_len = strlen(suffix);
        if (res_len + suffix_len + 1 <= sizeof(icu_path)) {
            memcpy(icu_path, resources_path, res_len);
            memcpy(icu_path + res_len, suffix, suffix_len);
            icu_path[res_len + suffix_len] = '\0';
        } else {
            icu_path[0] = '\0';
        }
        if (!icu_path[0] || !file_exists(icu_path)) {
            fprintf(stderr, "[ck-browser] Missing ICU data file: %s\n", icu_path);
        } else {
            int fd = open(icu_path, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "[ck-browser] Unable to open ICU data file: %s\n", icu_path);
            } else {
                close(fd);
            }
        }
    }
}

void BrowserApp::dump_cef_env_and_args(int argc, char *argv[]) const
{
    fprintf(stderr, "[ck-browser] argv:\n");
    for (int i = 0; i < argc; ++i) {
        fprintf(stderr, "  argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");
    }
    const char *envs[] = {
        "ICU_DATA",
        "ICU_DATA_FILE",
        "CHROME_VERSION_EXTRA",
        "LD_LIBRARY_PATH",
        "CEF_RESOURCE_PATH",
        "CEF_LOCALES_PATH",
        NULL
    };
    fprintf(stderr, "[ck-browser] env:\n");
    for (const char **env = envs; *env; ++env) {
        const char *val = getenv(*env);
        fprintf(stderr, "  %s=%s\n", *env, val ? val : "(unset)");
    }
}

bool BrowserApp::parse_cache_suffix_arg(int argc, char *argv[], std::string *out_suffix) const
{
    if (!out_suffix) return false;
    out_suffix->clear();
    const char *prefix = "--ck-cache-suffix=";
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) continue;
        if (strncmp(arg, prefix, strlen(prefix)) == 0) {
            *out_suffix = std::string(arg + strlen(prefix));
            break;
        }
        if (strcmp(arg, "--ck-cache-suffix") == 0 && i + 1 < argc && argv[i + 1]) {
            *out_suffix = std::string(argv[i + 1]);
            break;
        }
    }
    if (out_suffix->empty()) return false;
    for (char &c : *out_suffix) {
        bool ok = ((c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   c == '-' ||
                   c == '_');
        if (!ok) c = '_';
    }
    return true;
}

void BrowserApp::build_cef_argv(int argc, char *argv[], std::vector<char *> *out_argv) const
{
    if (!out_argv) return;
    out_argv->clear();
    if (argc <= 0 || !argv) return;
    out_argv->reserve((size_t)argc);
    out_argv->push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) continue;

        if (strcmp(arg, "-session") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }

        const char *open_url_prefix = "--ck-open-url=";
        if (strncmp(arg, open_url_prefix, strlen(open_url_prefix)) == 0) {
            continue;
        }
        if (strcmp(arg, "--ck-open-url") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }

        const char *cache_prefix = "--ck-cache-suffix=";
        if (strncmp(arg, cache_prefix, strlen(cache_prefix)) == 0) {
            continue;
        }
        if (strcmp(arg, "--ck-cache-suffix") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }

        out_argv->push_back(argv[i]);
    }
}

bool BrowserApp::has_opengl_support() const
{
    const char *libs[] = {"libGL.so.1", "libGL.so", NULL};
    for (const char **it = libs; *it; ++it) {
        void *handle = dlopen(*it, RTLD_LAZY | RTLD_LOCAL);
        if (!handle) continue;
        void *sym = dlsym(handle, "glXGetCurrentContext");
        dlclose(handle);
        if (sym) return true;
    }
    return false;
}

void BrowserApp::apply_gpu_switches(bool disable_gpu) const
{
    if (!disable_gpu) return;
    CefRefPtr<CefCommandLine> global = CefCommandLine::GetGlobalCommandLine();
    if (!global) return;
    global->AppendSwitch("disable-gpu");
    global->AppendSwitch("disable-software-rasterizer");
    global->AppendSwitch("disable-gpu-compositing");
}

SessionData *BrowserApp::prepare_session(int &argc, char **argv) const
{
    char *session_id = session_parse_argument(&argc, argv);
    SessionData *data = session_data_create(session_id);
    free(session_id);
    return data;
}
