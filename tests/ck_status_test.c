/*
 * ck_status_test.c - Unit & session D-Bus tests for ck-status hardware collectors,
 *                    unread Switch lamp sync, org.freedesktop.Notifications, and
 *                    StatusNotifierWatcher / StatusNotifierItem ARGB32 + Activate.
 */

#define _GNU_SOURCE
#include "../src/ck-status/ck-status.h"

#include <assert.h>
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_str_file(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(val, f);
    fclose(f);
}

static int g_toasts = 0;
static void test_toast_cb(const CkNotification *n, void *ud) {
    (void)n; (void)ud;
    g_toasts++;
}

int main(void) {
    const char *tmp_base = getenv("TMPDIR");
    if (!tmp_base || !tmp_base[0]) tmp_base = "/var/tmp";
    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "%s/ck-status-test-XXXXXX", tmp_base);
    char *work = mkdtemp(tmpl);
    assert(work != NULL);

    char home[600], sysfs[600], ps_bat[660], ps_ac[660], net_wl[660], net_wl_w[700], net_stat[700], proc_w[660];
    snprintf(home, sizeof(home), "%s/home", work);
    snprintf(sysfs, sizeof(sysfs), "%s/sys_class", work);
    snprintf(ps_bat, sizeof(ps_bat), "%s/power_supply/BAT0", sysfs);
    snprintf(ps_ac, sizeof(ps_ac), "%s/power_supply/AC0", sysfs);
    snprintf(net_wl, sizeof(net_wl), "%s/net/wlan0", sysfs);
    snprintf(net_wl_w, sizeof(net_wl_w), "%s/wireless", net_wl);
    snprintf(net_stat, sizeof(net_stat), "%s/statistics", net_wl);
    snprintf(proc_w, sizeof(proc_w), "%s/wireless", work);

    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s' '%s' '%s' '%s' '%s/bluetooth/hci0'",
             home, ps_bat, ps_ac, net_wl_w, net_stat, sysfs);
    assert(system(cmd) == 0);

    write_str_file((snprintf(cmd, sizeof(cmd), "%s/type", ps_bat), cmd), "Battery\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/present", ps_bat), cmd), "1\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/status", ps_bat), cmd), "Discharging\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/capacity", ps_bat), cmd), "78\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/model_name", ps_bat), cmd), "ThinkPad-SPARC-Cell\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/energy_now", ps_bat), cmd), "39000000\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/energy_full", ps_bat), cmd), "50000000\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/power_now", ps_bat), cmd), "13000000\n");

    write_str_file((snprintf(cmd, sizeof(cmd), "%s/type", ps_ac), cmd), "Mains\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/online", ps_ac), cmd), "0\n");

    write_str_file((snprintf(cmd, sizeof(cmd), "%s/operstate", net_wl), cmd), "up\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/rx_bytes", net_stat), cmd), "1048576\n");
    write_str_file((snprintf(cmd, sizeof(cmd), "%s/tx_bytes", net_stat), cmd), "524288\n");
    write_str_file(proc_w,
                   "Inter-| sta-|   Quality        |   Discarded packets\n"
                   " face | tus | link level noise |  nwid  crypt   frag\n"
                   " wlan0: 0000   56.  -54.  -256        0      0      0\n");

    setenv("HOME", home, 1);
    setenv("CK_STATUS_SYSFS_ROOT", sysfs, 1);
    setenv("CK_STATUS_PROC_NET_WIRELESS", proc_w, 1);

    CkStatusModel model = {0};
    ck_status_hw_poll(&model);

    assert(model.battery.present == true);
    assert(model.battery.discharging == true);
    assert(model.battery.ac_online == false);
    assert(model.battery.capacity_pct == 78);
    assert(model.battery.power_mw == 13000);
    assert(model.battery.minutes_remaining == 180);
    assert(strcmp(model.battery.model_name, "ThinkPad-SPARC-Cell") == 0);

    assert(model.net.wifi_present == true);
    assert(model.net.connected == true);
    assert(model.net.signal_pct == 80);
    assert(model.bt.adapter_present == true);

    /* Unread notification file contract for CDE Switch MONITOR_TYPE=file */
    char unread_file[700];
    snprintf(unread_file, sizeof(unread_file), "%s/.dt/notifications.unread", home);
    struct stat st;
    assert(stat(unread_file, &st) != 0);

    uint32_t id1 = ck_status_model_add_notification(&model, 0, "Mail", "New Message", "Welcome to CDE Wayland", 1, 3000);
    assert(id1 == 1);
    assert(model.unread_count == 1);
    assert(stat(unread_file, &st) == 0 && st.st_size > 0);

    /* Replace existing notification ID */
    uint32_t id1b = ck_status_model_add_notification(&model, id1, "Mail", "2 New Messages", "Updated summary", 1, 3000);
    assert(id1b == id1);
    assert(model.notif_count == 1);
    assert(model.unread_count == 1);

    ck_status_model_mark_all_read(&model);
    assert(model.unread_count == 0);
    assert(stat(unread_file, &st) != 0);

    /* Live D-Bus round-trip if DBUS_SESSION_BUS_ADDRESS is active */
    if (getenv("DBUS_SESSION_BUS_ADDRESS")) {
        assert(ck_status_dbus_init(&model, test_toast_cb, NULL, NULL));

        pid_t child = fork();
        if (child == 0) {
            /* Child: send Notify() AND act as a StatusNotifierItem client */
            DBusError err;
            dbus_error_init(&err);
            DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
            assert(c != NULL);
            dbus_bus_request_name(c, "org.ckcore.TestSniApp", DBUS_NAME_FLAG_REPLACE_EXISTING, &err);

            /* 1) Send org.freedesktop.Notifications.Notify */
            DBusMessage *m = dbus_message_new_method_call(
                "org.freedesktop.Notifications",
                "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications",
                "Notify"
            );
            DBusMessageIter it, acts, hints;
            dbus_message_iter_init_append(m, &it);
            const char *app = "TestSender", *icon = "", *sum = "Hello Sun/IBM", *body = "Live D-Bus toast";
            dbus_uint32_t rep_id = 0;
            dbus_int32_t exp = 2500;
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &rep_id);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &sum);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &body);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &acts);
            dbus_message_iter_close_container(&it, &acts);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &hints);
            dbus_message_iter_close_container(&it, &hints);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &exp);
            DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 1000, &err);
            assert(r != NULL);
            dbus_message_unref(r);
            dbus_message_unref(m);

            /* 2) Register StatusNotifierItem and answer GetAll + Activate */
            DBusMessage *reg = dbus_message_new_method_call(
                "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem"
            );
            const char *svc = "org.ckcore.TestSniApp";
            dbus_message_append_args(reg, DBUS_TYPE_STRING, &svc, DBUS_TYPE_INVALID);
            dbus_connection_send(c, reg, NULL);
            dbus_connection_flush(c);
            dbus_message_unref(reg);

            bool activated = false;
            for (int tick = 0; tick < 60 && !activated; ++tick) {
                dbus_connection_read_write(c, 25);
                DBusMessage *in;
                while ((in = dbus_connection_pop_message(c)) != NULL) {
                    if (dbus_message_is_method_call(in, "org.freedesktop.DBus.Properties", "GetAll")) {
                        DBusMessage *rp = dbus_message_new_method_return(in);
                        DBusMessageIter rit, dict, ent, var, parr, pst, pbytes;
                        dbus_message_iter_init_append(rp, &rit);
                        dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "{sv}", &dict);

                        const char *k_id = "Id", *v_id = "test-sni";
                        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                        dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k_id);
                        dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &var);
                        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v_id);
                        dbus_message_iter_close_container(&ent, &var);
                        dbus_message_iter_close_container(&dict, &ent);

                        const char *k_tt = "Title", *v_tt = "Sync Agent";
                        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                        dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k_tt);
                        dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &var);
                        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v_tt);
                        dbus_message_iter_close_container(&ent, &var);
                        dbus_message_iter_close_container(&dict, &ent);

                        const char *k_px = "IconPixmap";
                        unsigned char raw_icon[4 * 4 * 4];
                        for (size_t q = 0; q < sizeof(raw_icon); q += 4) {
                            raw_icon[q + 0] = 0xff;
                            raw_icon[q + 1] = 0x22;
                            raw_icon[q + 2] = 0xaa;
                            raw_icon[q + 3] = 0x66;
                        }
                        dbus_int32_t iw = 4, ih = 4;
                        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
                        dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k_px);
                        dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "a(iiay)", &var);
                        dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(iiay)", &parr);
                        dbus_message_iter_open_container(&parr, DBUS_TYPE_STRUCT, NULL, &pst);
                        dbus_message_iter_append_basic(&pst, DBUS_TYPE_INT32, &iw);
                        dbus_message_iter_append_basic(&pst, DBUS_TYPE_INT32, &ih);
                        dbus_message_iter_open_container(&pst, DBUS_TYPE_ARRAY, "y", &pbytes);
                        const unsigned char *rbp = raw_icon;
                        dbus_message_iter_append_fixed_array(&pbytes, DBUS_TYPE_BYTE, &rbp, (int)sizeof(raw_icon));
                        dbus_message_iter_close_container(&pst, &pbytes);
                        dbus_message_iter_close_container(&parr, &pst);
                        dbus_message_iter_close_container(&var, &parr);
                        dbus_message_iter_close_container(&ent, &var);
                        dbus_message_iter_close_container(&dict, &ent);

                        dbus_message_iter_close_container(&rit, &dict);
                        dbus_connection_send(c, rp, NULL);
                        dbus_connection_flush(c);
                        dbus_message_unref(rp);
                    } else if (dbus_message_is_method_call(in, "org.kde.StatusNotifierItem", "Activate")) {
                        DBusMessage *rp = dbus_message_new_method_return(in);
                        dbus_connection_send(c, rp, NULL);
                        dbus_connection_flush(c);
                        dbus_message_unref(rp);
                        activated = true;
                    }
                    dbus_message_unref(in);
                }
            }
            dbus_connection_close(c);
            dbus_connection_unref(c);
            _exit(activated ? 0 : 2);
        }

        for (int i = 0; i < 40 && model.sni_count == 0; ++i) {
            ck_status_dbus_step();
            usleep(20000);
        }
        assert(g_toasts >= 1);
        assert(model.sni_count == 1);
        assert(strcmp(model.sni_items[0].title, "Sync Agent") == 0);
        assert(model.sni_items[0].icon_w == 4 && model.sni_items[0].icon_h == 4);
        assert(model.sni_items[0].argb_pixels != NULL);
        assert(model.sni_items[0].argb_pixels[0] == 0xff22aa66U);

        ck_status_dbus_sni_activate(&model.sni_items[0], 120, 840, 1);
        int wstatus = 0;
        waitpid(child, &wstatus, 0);
        assert(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

        /* Drain NameOwnerChanged after child disconnects */
        for (int i = 0; i < 20 && model.sni_count > 0; ++i) {
            ck_status_dbus_step();
            usleep(15000);
        }
        assert(model.sni_count == 0);
        ck_status_dbus_shutdown(&model);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", work);
    assert(system(cmd) == 0);
    printf("ck-status model & D-Bus tests passed\n");
    return 0;
}
