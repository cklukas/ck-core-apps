#ifndef SESSION_UTILS_H
#define SESSION_UTILS_H

#include <Xm/Xm.h>
#include <Dt/Session.h>

/* Simple key/value store for session data */
typedef struct SessionKV {
    char *key;
    char *value;
    struct SessionKV *next;
} SessionKV;

typedef struct {
    char *session_id; /* duplicated from argv */
    SessionKV *items;
} SessionData;

/* Parse "-session <id>" from argv; returns strdup'd id or NULL. */
char *session_parse_argument(int *argc, char **argv);

/* Create/free session data. session_id may be NULL. */
SessionData *session_data_create(const char *session_id);
void session_data_free(SessionData *data);

/* Accessors */
const char *session_data_get(SessionData *data, const char *key);
int session_data_get_int(SessionData *data, const char *key, int default_value);
int session_data_has(SessionData *data, const char *key);
void session_data_set(SessionData *data, const char *key, const char *value);
void session_data_set_int(SessionData *data, const char *key, int value);

/* Load/save session file via CDE. session_save also sets restart command. */
Boolean session_load(Widget toplevel, SessionData *data);
void session_save(Widget toplevel, SessionData *data, const char *exec_path);

/* Helpers for geometry capture/restore */
void session_capture_geometry(Widget toplevel, SessionData *data,
                              const char *key_x, const char *key_y,
                              const char *key_w, const char *key_h);
Boolean session_apply_geometry(Widget toplevel, SessionData *data,
                               const char *key_x, const char *key_y,
                               const char *key_w, const char *key_h);

#endif /* SESSION_UTILS_H */
