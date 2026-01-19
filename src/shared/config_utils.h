#ifndef CONFIG_UTILS_H
#define CONFIG_UTILS_H

#include <stddef.h>

/* Build a config file path inside XDG_CONFIG_HOME/ck-core or ~/.config/ck-core */
void config_build_path(char *buf, size_t len, const char *filename);

/* Read an integer value from a simple "key value" config file.
 * Returns default_value if file/key is missing.
 */
int config_read_int(const char *filename, const char *key, int default_value);
int config_read_int_map(const char *filename, const char *key, int default_value);

/* Read a string value from a simple "key value" config file.
 * Returns a newly allocated string which the caller must free.
 * If missing, returns a copy of default_value (or NULL if default_value is NULL).
 */
char *config_read_string(const char *filename, const char *key, const char *default_value);

/* Write a single integer key/value pair to the config file, creating
 * the ck-core config directory if needed.
 */
void config_write_int(const char *filename, const char *key, int value);
void config_write_int_map(const char *filename, const char *key, int value);

/* Write a single string key/value pair to the config file, creating
 * the ck-core config directory if needed.
 */
void config_write_string(const char *filename, const char *key, const char *value);

#endif /* CONFIG_UTILS_H */
