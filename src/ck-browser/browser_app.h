#ifndef CK_BROWSER_BROWSER_APP_H
#define CK_BROWSER_BROWSER_APP_H

#include <cstddef>
#include <string>
#include <vector>

struct BrowserPaths {
    std::string resources_path;
    std::string locales_path;
    std::string subprocess_path;
};
struct SessionData;

class BrowserApp {
public:
    static BrowserApp &instance();

    int run(int argc, char *argv[]);
    int run_main(int argc, char *argv[]);

    BrowserPaths discover_cef_paths() const;
    bool build_path_from_dir(const char *dir, const char *suffix, char *buffer, size_t buffer_len) const;
    void build_cwd_path(char *buffer, size_t buffer_len, const char *suffix) const;
    bool dir_has_files(const char *path) const;
    bool file_exists(const char *path) const;
    bool find_existing_path(char *buffer, size_t buffer_len, const char *suffix) const;
    void get_exe_path(char *buffer, size_t buffer_len) const;
    void report_cef_resource_status(const char *resources_path, const char *locales_path) const;
    void dump_cef_env_and_args(int argc, char *argv[]) const;
    bool parse_cache_suffix_arg(int argc, char *argv[], std::string *out_suffix) const;
    void build_cef_argv(int argc, char *argv[], std::vector<char *> *out_argv) const;
    bool has_opengl_support() const;
    void apply_gpu_switches(bool disable_gpu) const;
    SessionData *prepare_session(int &argc, char **argv) const;

private:
    BrowserApp() = default;
};

int ck_browser_run(int argc, char *argv[]);

#endif // CK_BROWSER_BROWSER_APP_H
