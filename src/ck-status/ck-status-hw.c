/*
 * ck-status-hw.c - Power/Battery, Wi-Fi/Network, Bluetooth & unread-lamp file
 *                  collectors and D-Bus system controls for ck-status.
 */

#define _GNU_SOURCE
#include "ck-status.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *sysfs_class_root(void) {
    const char *env = getenv("CK_STATUS_SYSFS_ROOT");
    return (env && env[0]) ? env : "/sys/class";
}

static bool read_trimmed_line(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz == 0) return false;
    out[0] = '\0';
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    if (!fgets(out, (int)out_sz, fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        out[--len] = '\0';
    }
    return true;
}

static long long read_ll_file(const char *path, long long fallback) {
    char buf[64];
    if (!read_trimmed_line(path, buf, sizeof(buf))) return fallback;
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    return (end && end != buf) ? v : fallback;
}

static void safe_copy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

/* -------------------------------------------------------------------------
 * 1. Battery & Power Supply (/sys/class/power_supply + platform_profile)
 * ------------------------------------------------------------------------- */
static void poll_battery(CkBatteryState *bat) {
    memset(bat, 0, sizeof(*bat));
    bat->capacity_pct = -1;
    bat->minutes_remaining = -1;
    safe_copy(bat->status_text, sizeof(bat->status_text), "AC Power");
    safe_copy(bat->power_profile, sizeof(bat->power_profile), "balanced");

    char pprof[64];
    if (read_trimmed_line("/sys/firmware/acpi/platform_profile", pprof, sizeof(pprof)) && pprof[0]) {
        safe_copy(bat->power_profile, sizeof(bat->power_profile), pprof);
    }

    char ps_dir[512];
    snprintf(ps_dir, sizeof(ps_dir), "%s/power_supply", sysfs_class_root());
    DIR *d = opendir(ps_dir);
    if (!d) {
        bat->ac_online = true;
        return;
    }

    long long total_energy_now = 0;
    long long total_energy_full = 0;
    long long total_power_now = 0;
    int cap_sum = 0;
    int bat_count = 0;
    bool saw_mains = false;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char base[768], type_path[832], type_buf[32];
        snprintf(base, sizeof(base), "%s/%s", ps_dir, ent->d_name);
        snprintf(type_path, sizeof(type_path), "%s/type", base);
        if (!read_trimmed_line(type_path, type_buf, sizeof(type_buf))) {
            if (strncmp(ent->d_name, "BAT", 3) == 0) {
                safe_copy(type_buf, sizeof(type_buf), "Battery");
            } else if (strncmp(ent->d_name, "AC", 2) == 0 || strncmp(ent->d_name, "ADP", 3) == 0) {
                safe_copy(type_buf, sizeof(type_buf), "Mains");
            } else {
                continue;
            }
        }

        if (strcasecmp(type_buf, "Mains") == 0 || strcasecmp(type_buf, "USB") == 0) {
            saw_mains = true;
            char online_path[832];
            snprintf(online_path, sizeof(online_path), "%s/online", base);
            if (read_ll_file(online_path, 0) > 0) {
                bat->ac_online = true;
            }
        } else if (strcasecmp(type_buf, "Battery") == 0) {
            char pres_path[832];
            snprintf(pres_path, sizeof(pres_path), "%s/present", base);
            if (read_ll_file(pres_path, 1) == 0) continue;

            bat->present = true;
            if (bat->bat_name[0] == '\0') {
                safe_copy(bat->bat_name, sizeof(bat->bat_name), ent->d_name);
            }
            char model_p[832], status_p[832], cap_p[832];
            snprintf(model_p, sizeof(model_p), "%s/model_name", base);
            snprintf(status_p, sizeof(status_p), "%s/status", base);
            snprintf(cap_p, sizeof(cap_p), "%s/capacity", base);

            char mbuf[64];
            if (bat->model_name[0] == '\0' && read_trimmed_line(model_p, mbuf, sizeof(mbuf))) {
                safe_copy(bat->model_name, sizeof(bat->model_name), mbuf);
            }
            char sbuf[32];
            if (read_trimmed_line(status_p, sbuf, sizeof(sbuf))) {
                safe_copy(bat->status_text, sizeof(bat->status_text), sbuf);
                if (strcasecmp(sbuf, "Charging") == 0) bat->charging = true;
                else if (strcasecmp(sbuf, "Discharging") == 0) bat->discharging = true;
                else if (strcasecmp(sbuf, "Full") == 0 || strcasecmp(sbuf, "Not charging") == 0) bat->full = true;
            }
            long long cap = read_ll_file(cap_p, -1);
            if (cap >= 0 && cap <= 100) {
                cap_sum += (int)cap;
                bat_count++;
            }

            char en_now_p[832], en_full_p[832], pw_now_p[832];
            snprintf(en_now_p, sizeof(en_now_p), "%s/energy_now", base);
            snprintf(en_full_p, sizeof(en_full_p), "%s/energy_full", base);
            snprintf(pw_now_p, sizeof(pw_now_p), "%s/power_now", base);
            long long en_now = read_ll_file(en_now_p, -1);
            long long en_full = read_ll_file(en_full_p, -1);
            long long pw_now = read_ll_file(pw_now_p, -1);
            if (en_now < 0) {
                char ch_now_p[832], ch_full_p[832], cur_now_p[832], volt_p[832];
                snprintf(ch_now_p, sizeof(ch_now_p), "%s/charge_now", base);
                snprintf(ch_full_p, sizeof(ch_full_p), "%s/charge_full", base);
                snprintf(cur_now_p, sizeof(cur_now_p), "%s/current_now", base);
                snprintf(volt_p, sizeof(volt_p), "%s/voltage_now", base);
                long long ch_n = read_ll_file(ch_now_p, -1);
                long long ch_f = read_ll_file(ch_full_p, -1);
                long long cur_n = read_ll_file(cur_now_p, -1);
                long long volt = read_ll_file(volt_p, 12000000LL);
                if (ch_n >= 0) en_now = (ch_n * (volt / 1000LL)) / 1000LL;
                if (ch_f >= 0) en_full = (ch_f * (volt / 1000LL)) / 1000LL;
                if (cur_n >= 0) pw_now = (cur_n * (volt / 1000LL)) / 1000LL;
            }
            if (en_now > 0) total_energy_now += en_now;
            if (en_full > 0) total_energy_full += en_full;
            if (pw_now > 0) total_power_now += pw_now;
        }
    }
    closedir(d);

    if (!bat->present) {
        bat->ac_online = true;
        safe_copy(bat->status_text, sizeof(bat->status_text), "Workstation AC");
        return;
    }

    if (!saw_mains && !bat->discharging) {
        bat->ac_online = true;
    }
    if (bat_count > 0) {
        bat->capacity_pct = cap_sum / bat_count;
    } else if (total_energy_full > 0) {
        bat->capacity_pct = (int)((total_energy_now * 100LL) / total_energy_full);
    }
    if (bat->capacity_pct > 100) bat->capacity_pct = 100;

    if (total_power_now > 0) {
        /* sysfs power_now is in microwatts */
        bat->power_mw = (int)(total_power_now / 1000LL);
        if (bat->discharging && total_energy_now > 0) {
            bat->minutes_remaining = (int)((total_energy_now * 60LL) / total_power_now);
        } else if (bat->charging && total_energy_full > total_energy_now) {
            bat->minutes_remaining = (int)(((total_energy_full - total_energy_now) * 60LL) / total_power_now);
        }
    }
}

/* -------------------------------------------------------------------------
 * 2. Network & Wi-Fi (NetworkManager D-Bus + /sys/class/net + /proc/net/wireless)
 * ------------------------------------------------------------------------- */
static DBusBusType hw_dbus_type(void) {
    const char *env = getenv("CK_STATUS_SYSTEM_BUS");
    if (env && strcmp(env, "session") == 0) return DBUS_BUS_SESSION;
    return DBUS_BUS_SYSTEM;
}

static bool dbus_get_bool_prop(DBusConnection *conn, const char *dest, const char *path,
                               const char *iface, const char *prop, bool *out_val) {
    DBusMessage *msg = dbus_message_new_method_call(dest, path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return false;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 350, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return false;
    }
    DBusMessageIter it, sub;
    bool ok = false;
    if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &sub);
        if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t b = FALSE;
            dbus_message_iter_get_basic(&sub, &b);
            *out_val = (b != FALSE);
            ok = true;
        }
    }
    dbus_message_unref(reply);
    return ok;
}

static bool dbus_set_bool_prop(DBusConnection *conn, const char *dest, const char *path,
                               const char *iface, const char *prop, bool val) {
    DBusMessage *msg = dbus_message_new_method_call(dest, path, "org.freedesktop.DBus.Properties", "Set");
    if (!msg) return false;
    DBusMessageIter it, sub;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &sub);
    dbus_bool_t b = val ? TRUE : FALSE;
    dbus_message_iter_append_basic(&sub, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&it, &sub);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 600, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(reply);
    return true;
}

static void poll_nm_wifi(CkNetState *net) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(hw_dbus_type(), &err);
    if (!conn) {
        dbus_error_free(&err);
        return;
    }
    bool w_en = false;
    if (!dbus_get_bool_prop(conn, "org.freedesktop.NetworkManager",
                            "/org/freedesktop/NetworkManager",
                            "org.freedesktop.NetworkManager",
                            "WirelessEnabled", &w_en)) {
        dbus_connection_unref(conn);
        return;
    }
    net->nm_available = true;
    net->wifi_enabled = w_en;

    /* Enumerate devices for wireless APs */
    DBusMessage *m_devs = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        "GetDevices"
    );
    if (m_devs) {
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(conn, m_devs, 350, &err);
        dbus_message_unref(m_devs);
        if (rep) {
            DBusMessageIter it, arr;
            if (dbus_message_iter_init(rep, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&it, &arr);
                while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH) {
                    const char *dev_path = NULL;
                    dbus_message_iter_get_basic(&arr, &dev_path);
                    /* Check if device has Wireless interface with AccessPoints */
                    DBusMessage *m_aps = dbus_message_new_method_call(
                        "org.freedesktop.NetworkManager", dev_path,
                        "org.freedesktop.NetworkManager.Device.Wireless", "GetAllAccessPoints"
                    );
                    if (m_aps) {
                        DBusError e2;
                        dbus_error_init(&e2);
                        DBusMessage *r_aps = dbus_connection_send_with_reply_and_block(conn, m_aps, 300, &e2);
                        dbus_message_unref(m_aps);
                        if (r_aps) {
                            net->wifi_present = true;
                            /* Query ActiveAccessPoint */
                            char active_ap[128] = {0};
                            DBusMessage *m_act = dbus_message_new_method_call(
                                "org.freedesktop.NetworkManager", dev_path,
                                "org.freedesktop.DBus.Properties", "Get"
                            );
                            const char *w_if = "org.freedesktop.NetworkManager.Device.Wireless";
                            const char *ap_prop = "ActiveAccessPoint";
                            dbus_message_append_args(m_act, DBUS_TYPE_STRING, &w_if, DBUS_TYPE_STRING, &ap_prop, DBUS_TYPE_INVALID);
                            DBusError e3;
                            dbus_error_init(&e3);
                            DBusMessage *r_act = dbus_connection_send_with_reply_and_block(conn, m_act, 250, &e3);
                            dbus_message_unref(m_act);
                            if (r_act) {
                                DBusMessageIter ia, iv;
                                if (dbus_message_iter_init(r_act, &ia) && dbus_message_iter_get_arg_type(&ia) == DBUS_TYPE_VARIANT) {
                                    dbus_message_iter_recurse(&ia, &iv);
                                    if (dbus_message_iter_get_arg_type(&iv) == DBUS_TYPE_OBJECT_PATH) {
                                        const char *ap_p = NULL;
                                        dbus_message_iter_get_basic(&iv, &ap_p);
                                        if (ap_p && strcmp(ap_p, "/") != 0) {
                                            safe_copy(active_ap, sizeof(active_ap), ap_p);
                                        }
                                    }
                                }
                                dbus_message_unref(r_act);
                            } else {
                                dbus_error_free(&e3);
                            }

                            DBusMessageIter iap, ap_arr;
                            if (dbus_message_iter_init(r_aps, &iap) && dbus_message_iter_get_arg_type(&iap) == DBUS_TYPE_ARRAY) {
                                dbus_message_iter_recurse(&iap, &ap_arr);
                                while (dbus_message_iter_get_arg_type(&ap_arr) == DBUS_TYPE_OBJECT_PATH &&
                                       net->ap_count < CK_STATUS_MAX_WIFI_APS) {
                                    const char *ap_path = NULL;
                                    dbus_message_iter_get_basic(&ap_arr, &ap_path);
                                    /* Read Ssid & Strength */
                                    DBusMessage *m_all = dbus_message_new_method_call(
                                        "org.freedesktop.NetworkManager", ap_path,
                                        "org.freedesktop.DBus.Properties", "GetAll"
                                    );
                                    const char *ap_if = "org.freedesktop.NetworkManager.AccessPoint";
                                    dbus_message_append_args(m_all, DBUS_TYPE_STRING, &ap_if, DBUS_TYPE_INVALID);
                                    DBusError e4;
                                    dbus_error_init(&e4);
                                    DBusMessage *r_all = dbus_connection_send_with_reply_and_block(conn, m_all, 250, &e4);
                                    dbus_message_unref(m_all);
                                    if (r_all) {
                                        CkWifiAp ap = {0};
                                        safe_copy(ap.ap_dbus_path, sizeof(ap.ap_dbus_path), ap_path);
                                        ap.in_use = (active_ap[0] && strcmp(active_ap, ap_path) == 0);
                                        DBusMessageIter idict, ient;
                                        if (dbus_message_iter_init(r_all, &idict) &&
                                            dbus_message_iter_get_arg_type(&idict) == DBUS_TYPE_ARRAY) {
                                            dbus_message_iter_recurse(&idict, &ient);
                                            while (dbus_message_iter_get_arg_type(&ient) == DBUS_TYPE_DICT_ENTRY) {
                                                DBusMessageIter kv, vv;
                                                const char *k = NULL;
                                                dbus_message_iter_recurse(&ient, &kv);
                                                dbus_message_iter_get_basic(&kv, &k);
                                                dbus_message_iter_next(&kv);
                                                dbus_message_iter_recurse(&kv, &vv);
                                                if (k && strcmp(k, "Strength") == 0 &&
                                                    dbus_message_iter_get_arg_type(&vv) == DBUS_TYPE_BYTE) {
                                                    unsigned char st = 0;
                                                    dbus_message_iter_get_basic(&vv, &st);
                                                    ap.signal_pct = (int)st;
                                                } else if (k && strcmp(k, "Ssid") == 0 &&
                                                           dbus_message_iter_get_arg_type(&vv) == DBUS_TYPE_ARRAY) {
                                                    DBusMessageIter ba;
                                                    dbus_message_iter_recurse(&vv, &ba);
                                                    int n_bytes = 0;
                                                    const unsigned char *bytes = NULL;
                                                    dbus_message_iter_get_fixed_array(&ba, &bytes, &n_bytes);
                                                    if (bytes && n_bytes > 0) {
                                                        size_t cp = (size_t)n_bytes < sizeof(ap.ssid) - 1 ? (size_t)n_bytes : sizeof(ap.ssid) - 1;
                                                        memcpy(ap.ssid, bytes, cp);
                                                        ap.ssid[cp] = '\0';
                                                    }
                                                } else if (k && (strcmp(k, "WpaFlags") == 0 || strcmp(k, "RsnFlags") == 0) &&
                                                           dbus_message_iter_get_arg_type(&vv) == DBUS_TYPE_UINT32) {
                                                    dbus_uint32_t fl = 0;
                                                    dbus_message_iter_get_basic(&vv, &fl);
                                                    if (fl != 0) ap.secured = true;
                                                }
                                                dbus_message_iter_next(&ient);
                                            }
                                        }
                                        dbus_message_unref(r_all);
                                        if (ap.ssid[0]) {
                                            net->aps[net->ap_count++] = ap;
                                            if (ap.in_use) {
                                                safe_copy(net->ssid, sizeof(net->ssid), ap.ssid);
                                                net->signal_pct = ap.signal_pct;
                                                net->connected = true;
                                            }
                                        }
                                    } else {
                                        dbus_error_free(&e4);
                                    }
                                    dbus_message_iter_next(&ap_arr);
                                }
                            }
                            dbus_message_unref(r_aps);
                        } else {
                            dbus_error_free(&e2);
                        }
                    }
                    dbus_message_iter_next(&arr);
                }
            }
            dbus_message_unref(rep);
        } else {
            dbus_error_free(&err);
        }
    }
    dbus_connection_unref(conn);
}

static void poll_network(CkNetState *net) {
    memset(net, 0, sizeof(*net));

    char net_dir[512];
    snprintf(net_dir, sizeof(net_dir), "%s/net", sysfs_class_root());
    DIR *d = opendir(net_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0) continue;
            char base[768], wdir[832], oper_p[832], oper[32];
            snprintf(base, sizeof(base), "%s/%s", net_dir, ent->d_name);
            snprintf(wdir, sizeof(wdir), "%s/wireless", base);
            snprintf(oper_p, sizeof(oper_p), "%s/operstate", base);
            struct stat st;
            bool is_wifi = (stat(wdir, &st) == 0) || (strncmp(ent->d_name, "wl", 2) == 0);
            if (is_wifi) {
                net->wifi_present = true;
                net->wifi_enabled = true;
            }
            bool up = false;
            if (read_trimmed_line(oper_p, oper, sizeof(oper))) {
                up = (strcmp(oper, "up") == 0 || strcmp(oper, "unknown") == 0);
            }
            if (up) {
                net->connected = true;
                if (!is_wifi) net->ethernet_up = true;
                if (net->primary_iface[0] == '\0' || is_wifi) {
                    safe_copy(net->primary_iface, sizeof(net->primary_iface), ent->d_name);
                    char rx_p[832], tx_p[832];
                    snprintf(rx_p, sizeof(rx_p), "%s/statistics/rx_bytes", base);
                    snprintf(tx_p, sizeof(tx_p), "%s/statistics/tx_bytes", base);
                    net->rx_bytes = (uint64_t)read_ll_file(rx_p, 0);
                    net->tx_bytes = (uint64_t)read_ll_file(tx_p, 0);
                }
            }
        }
        closedir(d);
    }

    /* Fallback signal strength from /proc/net/wireless if present */
    const char *proc_w = getenv("CK_STATUS_PROC_NET_WIRELESS");
    if (!proc_w || !proc_w[0]) proc_w = "/proc/net/wireless";
    FILE *wf = fopen(proc_w, "r");
    if (wf) {
        char line[256];
        int ln_no = 0;
        while (fgets(line, sizeof(line), wf)) {
            ln_no++;
            if (ln_no <= 2) continue;
            char ifn[32];
            int status = 0;
            double link_q = 0.0;
            if (sscanf(line, " %31[^:]: %d %lf", ifn, &status, &link_q) == 3) {
                net->wifi_present = true;
                int pct = (int)((link_q / 70.0) * 100.0 + 0.5);
                if (pct > 100) pct = 100;
                if (pct < 0) pct = 0;
                net->signal_pct = pct;
                if (net->ssid[0] == '\0') {
                    safe_copy(net->ssid, sizeof(net->ssid), ifn);
                }
            }
        }
        fclose(wf);
    }

    /* Enrich with NetworkManager D-Bus if running */
    poll_nm_wifi(net);

    /* Collect IPv4 / IPv6 addresses via getifaddrs() */
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0 && ifa) {
        for (struct ifaddrs *p = ifa; p != NULL; p = p->ifa_next) {
            if (!p->ifa_addr || (p->ifa_flags & IFF_LOOPBACK)) continue;
            if (!(p->ifa_flags & IFF_UP)) continue;
            if (net->primary_iface[0] && strcmp(p->ifa_name, net->primary_iface) != 0 &&
                net->ipv4[0] != '\0') {
                continue;
            }
            if (p->ifa_addr->sa_family == AF_INET && net->ipv4[0] == '\0') {
                struct sockaddr_in *s4 = (struct sockaddr_in *)p->ifa_addr;
                inet_ntop(AF_INET, &s4->sin_addr, net->ipv4, sizeof(net->ipv4));
                if (net->primary_iface[0] == '\0') {
                    safe_copy(net->primary_iface, sizeof(net->primary_iface), p->ifa_name);
                }
                net->connected = true;
            } else if (p->ifa_addr->sa_family == AF_INET6 && net->ipv6[0] == '\0') {
                struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)p->ifa_addr;
                inet_ntop(AF_INET6, &s6->sin6_addr, net->ipv6, sizeof(net->ipv6));
            }
        }
        freeifaddrs(ifa);
    }
}

bool ck_status_net_set_wifi_enabled(CkStatusModel *model, bool enabled) {
    if (!model) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(hw_dbus_type(), &err);
    if (!conn) {
        dbus_error_free(&err);
        return false;
    }
    bool ok = dbus_set_bool_prop(
        conn,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        "WirelessEnabled",
        enabled
    );
    dbus_connection_unref(conn);
    if (ok) model->net.wifi_enabled = enabled;
    return ok;
}

/* -------------------------------------------------------------------------
 * 3. Bluetooth (BlueZ org.bluez ObjectManager + /sys/class/bluetooth fallback)
 * ------------------------------------------------------------------------- */
static void poll_bluetooth(CkBtState *bt) {
    memset(bt, 0, sizeof(*bt));

    char bt_dir[512];
    snprintf(bt_dir, sizeof(bt_dir), "%s/bluetooth", sysfs_class_root());
    DIR *d = opendir(bt_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            bt->adapter_present = true;
            safe_copy(bt->adapter_name, sizeof(bt->adapter_name), ent->d_name);
            break;
        }
        closedir(d);
    }

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(hw_dbus_type(), &err);
    if (!conn) {
        dbus_error_free(&err);
        return;
    }
    DBusMessage *msg = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"
    );
    if (!msg) {
        dbus_connection_unref(conn);
        return;
    }
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(conn, msg, 350, &err);
    dbus_message_unref(msg);
    if (!rep) {
        dbus_error_free(&err);
        dbus_connection_unref(conn);
        return;
    }

    bt->bluez_available = true;
    DBusMessageIter root, obj_arr;
    if (dbus_message_iter_init(rep, &root) && dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&root, &obj_arr);
        while (dbus_message_iter_get_arg_type(&obj_arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter o_kv, if_arr;
            const char *obj_path = NULL;
            dbus_message_iter_recurse(&obj_arr, &o_kv);
            dbus_message_iter_get_basic(&o_kv, &obj_path);
            dbus_message_iter_next(&o_kv);
            dbus_message_iter_recurse(&o_kv, &if_arr);

            while (dbus_message_iter_get_arg_type(&if_arr) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter i_kv, prop_arr;
                const char *if_name = NULL;
                dbus_message_iter_recurse(&if_arr, &i_kv);
                dbus_message_iter_get_basic(&i_kv, &if_name);
                dbus_message_iter_next(&i_kv);
                dbus_message_iter_recurse(&i_kv, &prop_arr);

                if (if_name && strcmp(if_name, "org.bluez.Adapter1") == 0) {
                    bt->adapter_present = true;
                    safe_copy(bt->adapter_path, sizeof(bt->adapter_path), obj_path);
                    while (dbus_message_iter_get_arg_type(&prop_arr) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter p_kv, pv;
                        const char *pk = NULL;
                        dbus_message_iter_recurse(&prop_arr, &p_kv);
                        dbus_message_iter_get_basic(&p_kv, &pk);
                        dbus_message_iter_next(&p_kv);
                        dbus_message_iter_recurse(&p_kv, &pv);
                        if (pk && strcmp(pk, "Powered") == 0 && dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t b = FALSE;
                            dbus_message_iter_get_basic(&pv, &b);
                            bt->powered = (b != FALSE);
                        } else if (pk && strcmp(pk, "Alias") == 0 && dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_STRING) {
                            const char *s = NULL;
                            dbus_message_iter_get_basic(&pv, &s);
                            safe_copy(bt->adapter_name, sizeof(bt->adapter_name), s);
                        }
                        dbus_message_iter_next(&prop_arr);
                    }
                } else if (if_name && strcmp(if_name, "org.bluez.Device1") == 0 &&
                           bt->device_count < CK_STATUS_MAX_BT_DEVICES) {
                    CkBtDevice dev = {0};
                    safe_copy(dev.dbus_path, sizeof(dev.dbus_path), obj_path);
                    while (dbus_message_iter_get_arg_type(&prop_arr) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter p_kv, pv;
                        const char *pk = NULL;
                        dbus_message_iter_recurse(&prop_arr, &p_kv);
                        dbus_message_iter_get_basic(&p_kv, &pk);
                        dbus_message_iter_next(&p_kv);
                        dbus_message_iter_recurse(&p_kv, &pv);
                        if (pk && (strcmp(pk, "Alias") == 0 || (strcmp(pk, "Name") == 0 && dev.name[0] == '\0')) &&
                            dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_STRING) {
                            const char *s = NULL;
                            dbus_message_iter_get_basic(&pv, &s);
                            safe_copy(dev.name, sizeof(dev.name), s);
                        } else if (pk && strcmp(pk, "Address") == 0 && dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_STRING) {
                            const char *s = NULL;
                            dbus_message_iter_get_basic(&pv, &s);
                            safe_copy(dev.address, sizeof(dev.address), s);
                        } else if (pk && strcmp(pk, "Icon") == 0 && dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_STRING) {
                            const char *s = NULL;
                            dbus_message_iter_get_basic(&pv, &s);
                            safe_copy(dev.icon, sizeof(dev.icon), s);
                        } else if (pk && strcmp(pk, "Connected") == 0 && dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t b = FALSE;
                            dbus_message_iter_get_basic(&pv, &b);
                            dev.connected = (b != FALSE);
                        } else if (pk && strcmp(pk, "Paired") == 0 && dbus_message_iter_get_arg_type(&pv) == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t b = FALSE;
                            dbus_message_iter_get_basic(&pv, &b);
                            dev.paired = (b != FALSE);
                        }
                        dbus_message_iter_next(&prop_arr);
                    }
                    if (dev.name[0] == '\0') safe_copy(dev.name, sizeof(dev.name), dev.address);
                    if (dev.name[0] != '\0') bt->devices[bt->device_count++] = dev;
                }
                dbus_message_iter_next(&if_arr);
            }
            dbus_message_iter_next(&obj_arr);
        }
    }
    dbus_message_unref(rep);
    dbus_connection_unref(conn);
}

bool ck_status_bt_set_powered(CkStatusModel *model, bool powered) {
    if (!model || !model->bt.adapter_path[0]) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(hw_dbus_type(), &err);
    if (!conn) {
        dbus_error_free(&err);
        return false;
    }
    bool ok = dbus_set_bool_prop(conn, "org.bluez", model->bt.adapter_path, "org.bluez.Adapter1", "Powered", powered);
    dbus_connection_unref(conn);
    if (ok) model->bt.powered = powered;
    return ok;
}

bool ck_status_bt_set_device_connected(const char *device_dbus_path, bool connect) {
    if (!device_dbus_path || !device_dbus_path[0]) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(hw_dbus_type(), &err);
    if (!conn) {
        dbus_error_free(&err);
        return false;
    }
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", device_dbus_path, "org.bluez.Device1", connect ? "Connect" : "Disconnect"
    );
    if (!m) {
        dbus_connection_unref(conn);
        return false;
    }
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(conn, m, 2000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        dbus_connection_unref(conn);
        return false;
    }
    dbus_message_unref(rep);
    dbus_connection_unref(conn);
    return true;
}

static void poll_system_load(CkSystemLoadState *load) {
    if (!load) return;
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    load->cpu_cores = (nprocs > 0) ? (int)nprocs : 1;

    double avg[3] = {0.0, 0.0, 0.0};
    if (getloadavg(avg, 3) > 0) {
        load->load_1m = avg[0];
        load->load_5m = avg[1];
        load->load_15m = avg[2];
    }

    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp) && strncmp(line, "cpu ", 4) == 0) {
            unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) >= 4) {
                unsigned long long cur_idle = idle + iowait;
                unsigned long long cur_total = user + nice + sys + cur_idle + irq + softirq + steal;
                if (prev_total > 0 && cur_total > prev_total) {
                    unsigned long long d_tot = cur_total - prev_total;
                    unsigned long long d_idle = (cur_idle >= prev_idle) ? (cur_idle - prev_idle) : 0;
                    int pct = (int)(((d_tot - d_idle) * 100ULL + d_tot / 2ULL) / d_tot);
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    load->cpu_usage_pct = pct;
                } else if (load->cpu_cores > 0) {
                    int est = (int)((load->load_1m * 100.0) / (double)load->cpu_cores + 0.5);
                    if (est < 0) est = 0;
                    if (est > 100) est = 100;
                    load->cpu_usage_pct = est;
                }
                prev_total = cur_total;
                prev_idle = cur_idle;
            }
        }
        fclose(fp);
    }
}

void ck_status_hw_poll(CkStatusModel *model) {
    if (!model) return;
    poll_battery(&model->battery);
    poll_system_load(&model->load);
    poll_network(&model->net);
    poll_bluetooth(&model->bt);
}

/* -------------------------------------------------------------------------
 * 4. Notification Model & CDE Central Switch MONITOR_TYPE=file Sync
 * ------------------------------------------------------------------------- */
void ck_status_sync_unread_file(const CkStatusModel *model) {
    const char *home = getenv("HOME");
    if (!home || !home[0] || !model) return;
    char dt_dir[1024], unread_path[1088];
    snprintf(dt_dir, sizeof(dt_dir), "%s/.dt", home);
    mkdir(dt_dir, 0755);
    snprintf(unread_path, sizeof(unread_path), "%s/notifications.unread", dt_dir);

    if (model->unread_count > 0) {
        FILE *fp = fopen(unread_path, "w");
        if (fp) {
            fprintf(fp, "%d\n", model->unread_count);
            fclose(fp);
        }
    } else {
        unlink(unread_path);
    }
}

uint32_t ck_status_model_add_notification(
    CkStatusModel *model,
    uint32_t replaces_id,
    const char *app_name,
    const char *summary,
    const char *body,
    uint8_t urgency,
    int32_t expire_ms
) {
    if (!model) return 0;
    if (model->next_notif_id == 0) model->next_notif_id = 1;

    uint32_t id = (replaces_id > 0) ? replaces_id : model->next_notif_id++;
    int slot = -1;
    if (replaces_id > 0) {
        for (int i = 0; i < model->notif_count; ++i) {
            if (model->notifs[i].id == replaces_id) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        if (model->notif_count < CK_STATUS_MAX_NOTIFICATIONS) {
            /* Insert newest at index 0 */
            memmove(&model->notifs[1], &model->notifs[0], (size_t)model->notif_count * sizeof(CkNotification));
            model->notif_count++;
            slot = 0;
        } else {
            if (model->notifs[CK_STATUS_MAX_NOTIFICATIONS - 1].unread && model->unread_count > 0) {
                model->unread_count--;
            }
            memmove(&model->notifs[1], &model->notifs[0], (CK_STATUS_MAX_NOTIFICATIONS - 1) * sizeof(CkNotification));
            slot = 0;
        }
    } else if (model->notifs[slot].unread && model->unread_count > 0) {
        model->unread_count--;
    }

    CkNotification *n = &model->notifs[slot];
    memset(n, 0, sizeof(*n));
    n->id = id;
    safe_copy(n->app_name, sizeof(n->app_name), (app_name && app_name[0]) ? app_name : "Desktop");
    safe_copy(n->summary, sizeof(n->summary), (summary && summary[0]) ? summary : "(Notification)");
    safe_copy(n->body, sizeof(n->body), body ? body : "");
    n->urgency = urgency > 2 ? 1 : urgency;
    n->expire_ms = expire_ms;
    n->unread = true;

    time_t now = time(NULL);
    struct tm lt;
    if (localtime_r(&now, &lt)) {
        strftime(n->time_str, sizeof(n->time_str), "%H:%M:%S", &lt);
    } else {
        safe_copy(n->time_str, sizeof(n->time_str), "--:--:--");
    }

    model->unread_count++;
    ck_status_sync_unread_file(model);
    return id;
}

void ck_status_model_mark_all_read(CkStatusModel *model) {
    if (!model) return;
    for (int i = 0; i < model->notif_count; ++i) {
        model->notifs[i].unread = false;
    }
    model->unread_count = 0;
    ck_status_sync_unread_file(model);
}

void ck_status_model_clear_notifications(CkStatusModel *model) {
    if (!model) return;
    model->notif_count = 0;
    model->unread_count = 0;
    ck_status_sync_unread_file(model);
}
