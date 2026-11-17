/*
 * ck-status-dbus.c - FreeDesktop org.freedesktop.Notifications daemon,
 *                    org.kde/freedesktop.StatusNotifierWatcher + Host,
 *                    and org.ckcore.Status single-instance IPC.
 */

#define _GNU_SOURCE
#include "ck-status.h"

#include <arpa/inet.h>
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DBusConnection *g_bus = NULL;
static CkStatusModel *g_model = NULL;
static CkStatusToastFn g_on_toast = NULL;
static CkStatusChangedFn g_on_changed = NULL;
static void *g_userdata = NULL;
static void (*g_raise_cb)(const char *tab, void *userdata) = NULL;

void ck_status_set_raise_callback(void (*cb)(const char *tab, void *userdata)) {
    g_raise_cb = cb;
}

static void safe_str(char *dst, size_t sz, const char *src) {
    if (!dst || sz == 0) return;
    snprintf(dst, sz, "%s", src ? src : "");
}

static void free_sni_item(CkSniItem *it) {
    if (!it) return;
    free(it->argb_pixels);
    it->argb_pixels = NULL;
    memset(it, 0, sizeof(*it));
}

static void fetch_sni_properties(CkSniItem *item) {
    if (!g_bus || !item || !item->bus_name[0]) return;

    const char *ifaces[2] = {
        "org.kde.StatusNotifierItem",
        "org.freedesktop.StatusNotifierItem"
    };

    for (int a = 0; a < 2; ++a) {
        DBusMessage *m = dbus_message_new_method_call(
            item->bus_name,
            item->object_path,
            "org.freedesktop.DBus.Properties",
            "GetAll"
        );
        if (!m) continue;
        dbus_message_append_args(m, DBUS_TYPE_STRING, &ifaces[a], DBUS_TYPE_INVALID);
        DBusError err;
        dbus_error_init(&err);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_bus, m, 300, &err);
        dbus_message_unref(m);
        if (!rep) {
            dbus_error_free(&err);
            continue;
        }

        DBusMessageIter root, dict;
        if (dbus_message_iter_init(rep, &root) && dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&root, &dict);
            while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter kv, var;
                const char *key = NULL;
                dbus_message_iter_recurse(&dict, &kv);
                dbus_message_iter_get_basic(&kv, &key);
                dbus_message_iter_next(&kv);
                dbus_message_iter_recurse(&kv, &var);
                int vtype = dbus_message_iter_get_arg_type(&var);

                if (key && strcmp(key, "Id") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    safe_str(item->id, sizeof(item->id), s);
                } else if (key && strcmp(key, "Title") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    safe_str(item->title, sizeof(item->title), s);
                } else if (key && strcmp(key, "Status") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    safe_str(item->status, sizeof(item->status), s);
                } else if (key && strcmp(key, "IconName") == 0 && vtype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&var, &s);
                    safe_str(item->icon_name, sizeof(item->icon_name), s);
                } else if (key && strcmp(key, "IconPixmap") == 0 && vtype == DBUS_TYPE_ARRAY) {
                    /* Signature a(iiay): pick pixmap closest to 24x24 */
                    DBusMessageIter parr;
                    dbus_message_iter_recurse(&var, &parr);
                    int best_w = 0, best_h = 0, best_dist = 999999;
                    const unsigned char *best_bytes = NULL;
                    int best_nbytes = 0;

                    while (dbus_message_iter_get_arg_type(&parr) == DBUS_TYPE_STRUCT) {
                        DBusMessageIter st, barr;
                        dbus_int32_t pw = 0, ph = 0;
                        dbus_message_iter_recurse(&parr, &st);
                        if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32) {
                            dbus_message_iter_get_basic(&st, &pw);
                            dbus_message_iter_next(&st);
                        }
                        if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32) {
                            dbus_message_iter_get_basic(&st, &ph);
                            dbus_message_iter_next(&st);
                        }
                        if (pw > 0 && ph > 0 && pw <= 256 && ph <= 256 &&
                            dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY) {
                            dbus_message_iter_recurse(&st, &barr);
                            const unsigned char *raw = NULL;
                            int nraw = 0;
                            dbus_message_iter_get_fixed_array(&barr, &raw, &nraw);
                            if (raw && nraw >= pw * ph * 4) {
                                int dist = abs((int)pw - 24) + abs((int)ph - 24);
                                if (dist < best_dist) {
                                    best_dist = dist;
                                    best_w = (int)pw;
                                    best_h = (int)ph;
                                    best_bytes = raw;
                                    best_nbytes = nraw;
                                }
                            }
                        }
                        dbus_message_iter_next(&parr);
                    }

                    if (best_bytes && best_w > 0 && best_h > 0 && best_nbytes >= best_w * best_h * 4) {
                        uint32_t *px = (uint32_t *)malloc((size_t)best_w * (size_t)best_h * sizeof(uint32_t));
                        if (px) {
                            /* SNI IconPixmap bytes are network byte order ARGB32 */
                            for (int i = 0; i < best_w * best_h; ++i) {
                                const unsigned char *p = best_bytes + i * 4;
                                px[i] = ((uint32_t)p[0] << 24) |
                                        ((uint32_t)p[1] << 16) |
                                        ((uint32_t)p[2] << 8)  |
                                        ((uint32_t)p[3]);
                            }
                            free(item->argb_pixels);
                            item->argb_pixels = px;
                            item->icon_w = best_w;
                            item->icon_h = best_h;
                        }
                    }
                }
                dbus_message_iter_next(&dict);
            }
        }
        dbus_message_unref(rep);
        if (item->id[0] || item->title[0] || item->argb_pixels) break;
    }

    if (!item->title[0]) safe_str(item->title, sizeof(item->title), item->id[0] ? item->id : item->bus_name);
    if (!item->status[0]) safe_str(item->status, sizeof(item->status), "Active");
}

static void emit_sni_signal(const char *sig_name, const char *full_item) {
    if (!g_bus || !full_item) return;
    const char *ifaces[2] = {
        "org.kde.StatusNotifierWatcher",
        "org.freedesktop.StatusNotifierWatcher"
    };
    for (int i = 0; i < 2; ++i) {
        DBusMessage *sig = dbus_message_new_signal("/StatusNotifierWatcher", ifaces[i], sig_name);
        if (sig) {
            dbus_message_append_args(sig, DBUS_TYPE_STRING, &full_item, DBUS_TYPE_INVALID);
            dbus_connection_send(g_bus, sig, NULL);
            dbus_message_unref(sig);
        }
    }
}

static void register_sni_item(const char *arg, const char *sender) {
    if (!g_model || !arg || !arg[0]) return;

    char bus_name[128] = {0};
    char obj_path[128] = {0};
    if (arg[0] == '/') {
        if (!sender || !sender[0]) return;
        safe_str(bus_name, sizeof(bus_name), sender);
        safe_str(obj_path, sizeof(obj_path), arg);
    } else {
        const char *slash = strchr(arg, '/');
        if (slash) {
            size_t blen = (size_t)(slash - arg);
            if (blen >= sizeof(bus_name)) blen = sizeof(bus_name) - 1;
            memcpy(bus_name, arg, blen);
            bus_name[blen] = '\0';
            safe_str(obj_path, sizeof(obj_path), slash);
        } else {
            safe_str(bus_name, sizeof(bus_name), arg);
            safe_str(obj_path, sizeof(obj_path), "/StatusNotifierItem");
        }
    }

    int slot = -1;
    for (int i = 0; i < g_model->sni_count; ++i) {
        if (strcmp(g_model->sni_items[i].bus_name, bus_name) == 0 &&
            strcmp(g_model->sni_items[i].object_path, obj_path) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_model->sni_count >= CK_STATUS_MAX_SNI_ITEMS) return;
        slot = g_model->sni_count++;
        memset(&g_model->sni_items[slot], 0, sizeof(CkSniItem));
    }

    CkSniItem *it = &g_model->sni_items[slot];
    safe_str(it->bus_name, sizeof(it->bus_name), bus_name);
    safe_str(it->object_path, sizeof(it->object_path), obj_path);
    safe_str(it->owner, sizeof(it->owner), sender ? sender : bus_name);
    fetch_sni_properties(it);

    char combined[260];
    snprintf(combined, sizeof(combined), "%s%s", it->bus_name, it->object_path);
    emit_sni_signal("StatusNotifierItemRegistered", combined);

    if (g_on_changed) g_on_changed(g_userdata);
}

static void remove_sni_by_owner(const char *gone_name) {
    if (!g_model || !gone_name || !gone_name[0]) return;
    bool changed = false;
    for (int i = g_model->sni_count - 1; i >= 0; --i) {
        CkSniItem *it = &g_model->sni_items[i];
        if (strcmp(it->bus_name, gone_name) == 0 || strcmp(it->owner, gone_name) == 0) {
            char combined[260];
            snprintf(combined, sizeof(combined), "%s%s", it->bus_name, it->object_path);
            free_sni_item(it);
            for (int j = i; j < g_model->sni_count - 1; ++j) {
                g_model->sni_items[j] = g_model->sni_items[j + 1];
            }
            memset(&g_model->sni_items[g_model->sni_count - 1], 0, sizeof(CkSniItem));
            g_model->sni_count--;
            emit_sni_signal("StatusNotifierItemUnregistered", combined);
            changed = true;
        }
    }
    if (changed && g_on_changed) g_on_changed(g_userdata);
}

static void append_registered_items_variant(DBusMessageIter *parent) {
    DBusMessageIter var, arr;
    dbus_message_iter_open_container(parent, DBUS_TYPE_VARIANT, "as", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
    if (g_model) {
        for (int i = 0; i < g_model->sni_count; ++i) {
            char combined[260];
            snprintf(combined, sizeof(combined), "%s%s",
                     g_model->sni_items[i].bus_name, g_model->sni_items[i].object_path);
            const char *p = combined;
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &p);
        }
    }
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(parent, &var);
}

static DBusHandlerResult handle_dbus_message(DBusConnection *conn, DBusMessage *msg, void *user_data) {
    (void)user_data;
    const char *path = dbus_message_get_path(msg);
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    /* Track disconnected StatusNotifierItem owners & NewIcon/NewTitle/NewStatus signals */
    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
        if (iface && strcmp(iface, "org.freedesktop.DBus") == 0 &&
            member && strcmp(member, "NameOwnerChanged") == 0) {
            const char *name = NULL, *old_o = NULL, *new_o = NULL;
            if (dbus_message_get_args(msg, NULL,
                                      DBUS_TYPE_STRING, &name,
                                      DBUS_TYPE_STRING, &old_o,
                                      DBUS_TYPE_STRING, &new_o,
                                      DBUS_TYPE_INVALID)) {
                if (new_o && new_o[0] == '\0') {
                    remove_sni_by_owner(name);
                    if (old_o && old_o[0]) remove_sni_by_owner(old_o);
                }
            }
        } else if (member && (strcmp(member, "NewIcon") == 0 ||
                              strcmp(member, "NewTitle") == 0 ||
                              strcmp(member, "NewStatus") == 0)) {
            const char *sender = dbus_message_get_sender(msg);
            if (sender && g_model) {
                for (int i = 0; i < g_model->sni_count; ++i) {
                    if (strcmp(g_model->sni_items[i].owner, sender) == 0 ||
                        strcmp(g_model->sni_items[i].bus_name, sender) == 0) {
                        fetch_sni_properties(&g_model->sni_items[i]);
                        if (g_on_changed) g_on_changed(g_userdata);
                    }
                }
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL || !member) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    /* 1. Single-instance raise / tab switch: org.ckcore.Status */
    if (path && strcmp(path, "/org/ckcore/Status") == 0 &&
        member && strcmp(member, "RaiseTab") == 0) {
        const char *tab = "overview";
        dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &tab, DBUS_TYPE_INVALID);
        if (g_raise_cb) g_raise_cb(tab, g_userdata);
        DBusMessage *rep = dbus_message_new_method_return(msg);
        if (rep) {
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* 2. org.freedesktop.Notifications */
    if (path && strcmp(path, "/org/freedesktop/Notifications") == 0) {
        if (strcmp(member, "GetCapabilities") == 0) {
            DBusMessage *rep = dbus_message_new_method_return(msg);
            DBusMessageIter it, arr;
            dbus_message_iter_init_append(rep, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
            const char *caps[] = {"body", "actions", "persistence"};
            for (size_t i = 0; i < 3; ++i) {
                dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &caps[i]);
            }
            dbus_message_iter_close_container(&it, &arr);
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (strcmp(member, "GetServerInformation") == 0) {
            DBusMessage *rep = dbus_message_new_method_return(msg);
            const char *nm = "CK Core CDE Notification Service";
            const char *vn = "CK Core";
            const char *vr = "1.0";
            const char *sp = "1.2";
            dbus_message_append_args(
                rep,
                DBUS_TYPE_STRING, &nm,
                DBUS_TYPE_STRING, &vn,
                DBUS_TYPE_STRING, &vr,
                DBUS_TYPE_STRING, &sp,
                DBUS_TYPE_INVALID
            );
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (strcmp(member, "Notify") == 0) {
            DBusMessageIter it;
            const char *app_name = "";
            dbus_uint32_t replaces_id = 0;
            const char *app_icon = "";
            const char *summary = "";
            const char *body = "";
            dbus_int32_t expire_ms = 5000;
            uint8_t urgency = 1;

            if (dbus_message_iter_init(msg, &it)) {
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&it, &app_name);
                    dbus_message_iter_next(&it);
                }
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&it, &replaces_id);
                    dbus_message_iter_next(&it);
                }
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&it, &app_icon);
                    (void)app_icon;
                    dbus_message_iter_next(&it);
                }
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&it, &summary);
                    dbus_message_iter_next(&it);
                }
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&it, &body);
                    dbus_message_iter_next(&it);
                }
                /* Skip actions array */
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_next(&it);
                }
                /* Parse hints a{sv} for urgency */
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter hdict;
                    dbus_message_iter_recurse(&it, &hdict);
                    while (dbus_message_iter_get_arg_type(&hdict) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter hkv, hvar;
                        const char *hk = NULL;
                        dbus_message_iter_recurse(&hdict, &hkv);
                        dbus_message_iter_get_basic(&hkv, &hk);
                        dbus_message_iter_next(&hkv);
                        dbus_message_iter_recurse(&hkv, &hvar);
                        if (hk && strcmp(hk, "urgency") == 0 &&
                            dbus_message_iter_get_arg_type(&hvar) == DBUS_TYPE_BYTE) {
                            unsigned char u = 1;
                            dbus_message_iter_get_basic(&hvar, &u);
                            urgency = u;
                        }
                        dbus_message_iter_next(&hdict);
                    }
                    dbus_message_iter_next(&it);
                }
                if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32) {
                    dbus_message_iter_get_basic(&it, &expire_ms);
                }
            }

            uint32_t assigned = ck_status_model_add_notification(
                g_model, replaces_id, app_name, summary, body, urgency, expire_ms
            );
            if (g_on_toast && g_model && (!g_model->dnd_enabled || urgency >= 2) && g_model->notif_count > 0) {
                g_on_toast(&g_model->notifs[0], g_userdata);
            }
            if (g_on_changed) g_on_changed(g_userdata);

            DBusMessage *rep = dbus_message_new_method_return(msg);
            dbus_uint32_t ret_id = assigned;
            dbus_message_append_args(rep, DBUS_TYPE_UINT32, &ret_id, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (strcmp(member, "CloseNotification") == 0) {
            dbus_uint32_t nid = 0;
            dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &nid, DBUS_TYPE_INVALID);
            DBusMessage *rep = dbus_message_new_method_return(msg);
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);

            DBusMessage *sig = dbus_message_new_signal(
                "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed"
            );
            if (sig) {
                dbus_uint32_t reason = 3;
                dbus_message_append_args(sig, DBUS_TYPE_UINT32, &nid, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
                dbus_connection_send(conn, sig, NULL);
                dbus_message_unref(sig);
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    /* 3. StatusNotifierWatcher (/StatusNotifierWatcher) */
    if (path && strcmp(path, "/StatusNotifierWatcher") == 0) {
        if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
            const char *svc = NULL;
            if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &svc, DBUS_TYPE_INVALID) && svc) {
                register_sni_item(svc, dbus_message_get_sender(msg));
            }
            DBusMessage *rep = dbus_message_new_method_return(msg);
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
            DBusMessage *rep = dbus_message_new_method_return(msg);
            dbus_connection_send(conn, rep, NULL);
            dbus_message_unref(rep);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (iface && strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
            if (strcmp(member, "Get") == 0) {
                const char *p_if = NULL, *prop = NULL;
                dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &p_if, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
                DBusMessage *rep = dbus_message_new_method_return(msg);
                DBusMessageIter it, var;
                dbus_message_iter_init_append(rep, &it);
                if (prop && strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
                    append_registered_items_variant(&it);
                } else if (prop && strcmp(prop, "IsStatusNotifierHostRegistered") == 0) {
                    dbus_bool_t b = TRUE;
                    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
                    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
                    dbus_message_iter_close_container(&it, &var);
                } else {
                    dbus_int32_t ver = 0;
                    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "i", &var);
                    dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &ver);
                    dbus_message_iter_close_container(&it, &var);
                }
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (strcmp(member, "GetAll") == 0) {
                DBusMessage *rep = dbus_message_new_method_return(msg);
                DBusMessageIter it, dict, ent, var;
                dbus_message_iter_init_append(rep, &it);
                dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);

                const char *k1 = "RegisteredStatusNotifierItems";
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k1);
                append_registered_items_variant(&ent);
                dbus_message_iter_close_container(&dict, &ent);

                const char *k2 = "IsStatusNotifierHostRegistered";
                dbus_bool_t b = TRUE;
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k2);
                dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "b", &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
                dbus_message_iter_close_container(&ent, &var);
                dbus_message_iter_close_container(&dict, &ent);

                const char *k3 = "ProtocolVersion";
                dbus_int32_t ver = 0;
                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k3);
                dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "i", &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &ver);
                dbus_message_iter_close_container(&ent, &var);
                dbus_message_iter_close_container(&dict, &ent);

                dbus_message_iter_close_container(&it, &dict);
                dbus_connection_send(conn, rep, NULL);
                dbus_message_unref(rep);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool ck_status_dbus_init(
    CkStatusModel *model,
    CkStatusToastFn on_toast,
    CkStatusChangedFn on_changed,
    void *userdata
) {
    g_model = model;
    g_on_toast = on_toast;
    g_on_changed = on_changed;
    g_userdata = userdata;

    DBusError err;
    dbus_error_init(&err);
    g_bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_bus) {
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(g_bus, FALSE);

    dbus_connection_add_filter(g_bus, handle_dbus_message, NULL, NULL);
    dbus_bus_add_match(g_bus, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'", NULL);
    dbus_bus_add_match(g_bus, "type='signal',interface='org.kde.StatusNotifierItem'", NULL);
    dbus_bus_add_match(g_bus, "type='signal',interface='org.freedesktop.StatusNotifierItem'", NULL);

    const char *names[] = {
        "org.ckcore.Status",
        "org.freedesktop.Notifications",
        "org.kde.StatusNotifierWatcher",
        "org.freedesktop.StatusNotifierWatcher",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        DBusError e2;
        dbus_error_init(&e2);
        dbus_bus_request_name(g_bus, names[i], DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE, &e2);
        dbus_error_free(&e2);
    }

    char host_name[96];
    snprintf(host_name, sizeof(host_name), "org.kde.StatusNotifierHost-%d", (int)getpid());
    dbus_bus_request_name(g_bus, host_name, DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL);

    /* Emit StatusNotifierHostRegistered so waiting tray clients register immediately */
    const char *ifaces[2] = {"org.kde.StatusNotifierWatcher", "org.freedesktop.StatusNotifierWatcher"};
    for (int i = 0; i < 2; ++i) {
        DBusMessage *sig = dbus_message_new_signal("/StatusNotifierWatcher", ifaces[i], "StatusNotifierHostRegistered");
        if (sig) {
            dbus_connection_send(g_bus, sig, NULL);
            dbus_message_unref(sig);
        }
    }
    dbus_connection_flush(g_bus);
    return true;
}

void ck_status_dbus_step(void) {
    if (!g_bus) return;
    dbus_connection_read_write(g_bus, 0);
    while (dbus_connection_dispatch(g_bus) == DBUS_DISPATCH_DATA_REMAINS) {
        /* drain queued messages */
    }
}

int ck_status_dbus_get_fd(void) {
    if (!g_bus) return -1;
    int fd = -1;
    if (dbus_connection_get_unix_fd(g_bus, &fd)) return fd;
    return -1;
}

void ck_status_dbus_sni_activate(const CkSniItem *item, int x, int y, int button) {
    if (!g_bus || !item || !item->bus_name[0]) return;
    const char *method = "Activate";
    if (button == 2) method = "SecondaryActivate";
    else if (button == 3) method = "ContextMenu";

    const char *ifaces[2] = {"org.kde.StatusNotifierItem", "org.freedesktop.StatusNotifierItem"};
    for (int i = 0; i < 2; ++i) {
        DBusMessage *m = dbus_message_new_method_call(item->bus_name, item->object_path, ifaces[i], method);
        if (!m) continue;
        dbus_int32_t dx = x, dy = y;
        dbus_message_append_args(m, DBUS_TYPE_INT32, &dx, DBUS_TYPE_INT32, &dy, DBUS_TYPE_INVALID);
        DBusError err;
        dbus_error_init(&err);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_bus, m, 300, &err);
        dbus_message_unref(m);
        if (rep) {
            dbus_message_unref(rep);
            break;
        }
        dbus_error_free(&err);
    }
}

void ck_status_dbus_raise_existing(const char *tab_name) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        dbus_error_free(&err);
        return;
    }
    DBusMessage *m = dbus_message_new_method_call(
        "org.ckcore.Status", "/org/ckcore/Status", "org.ckcore.Status", "RaiseTab"
    );
    if (m) {
        const char *t = (tab_name && tab_name[0]) ? tab_name : "overview";
        dbus_message_append_args(m, DBUS_TYPE_STRING, &t, DBUS_TYPE_INVALID);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(conn, m, 300, &err);
        dbus_message_unref(m);
        if (rep) dbus_message_unref(rep);
        else dbus_error_free(&err);
    }
    dbus_connection_unref(conn);
}

void ck_status_dbus_shutdown(CkStatusModel *model) {
    if (model) {
        for (int i = 0; i < model->sni_count; ++i) {
            free_sni_item(&model->sni_items[i]);
        }
        model->sni_count = 0;
    }
    if (g_bus) {
        dbus_connection_unref(g_bus);
        g_bus = NULL;
    }
}
