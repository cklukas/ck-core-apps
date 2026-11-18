/*
 * ck-status.h - System Status, Desktop Notifications, Laptop Hardware &
 *               StatusNotifier Host for CK Core CDE (Wayland & X11/XLibre).
 */

#ifndef CK_STATUS_H
#define CK_STATUS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define CK_STATUS_MAX_NOTIFICATIONS 64
#define CK_STATUS_MAX_SNI_ITEMS     24
#define CK_STATUS_MAX_WIFI_APS      16
#define CK_STATUS_MAX_BT_DEVICES    16

typedef enum {
    CK_TAB_OVERVIEW = 0,
    CK_TAB_NOTIFICATIONS = 1,
    CK_TAB_POWER = 2,
    CK_TAB_NETWORK = 3,
    CK_TAB_BLUETOOTH = 4,
    CK_TAB_COUNT = 5
} CkStatusTab;

typedef struct {
    bool present;
    bool ac_online;
    bool charging;
    bool discharging;
    bool full;
    int capacity_pct;       /* 0..100, or -1 if no battery */
    int power_mw;           /* milliwatts draw/charge if reported */
    int minutes_remaining;  /* >0 when known, else -1 */
    char bat_name[32];
    char model_name[64];
    char status_text[32];
    char power_profile[32]; /* low-power, balanced, performance, or n/a */
} CkBatteryState;

typedef struct {
    char ssid[64];
    int signal_pct;
    bool secured;
    bool in_use;
    char ap_dbus_path[128];
} CkWifiAp;

typedef struct {
    bool connected;
    bool ethernet_up;
    bool wifi_present;
    bool wifi_enabled;
    bool nm_available;
    char primary_iface[32];
    char ssid[64];
    int signal_pct;
    char ipv4[48];
    char ipv6[64];
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    int ap_count;
    CkWifiAp aps[CK_STATUS_MAX_WIFI_APS];
} CkNetState;

typedef struct {
    char dbus_path[128];
    char address[24];
    char name[64];
    char icon[32];
    bool paired;
    bool connected;
} CkBtDevice;

typedef struct {
    bool adapter_present;
    bool powered;
    bool bluez_available;
    char adapter_path[128];
    char adapter_name[64];
    int device_count;
    CkBtDevice devices[CK_STATUS_MAX_BT_DEVICES];
} CkBtState;

typedef struct {
    uint32_t id;
    char app_name[64];
    char summary[128];
    char body[512];
    uint8_t urgency; /* 0=Low, 1=Normal, 2=Critical */
    int32_t expire_ms;
    char time_str[16];
    bool unread;
} CkNotification;

typedef struct {
    char bus_name[128];
    char object_path[128];
    char owner[64];
    char id[64];
    char title[128];
    char status[24]; /* Passive, Active, NeedsAttention */
    char icon_name[64];
    int icon_w;
    int icon_h;
    uint32_t *argb_pixels; /* ARGB32 host-endian */
} CkSniItem;

typedef struct {
    double load_1m;
    double load_5m;
    double load_15m;
    int cpu_usage_pct;
    int cpu_cores;
} CkSystemLoadState;

typedef struct {
    CkBatteryState battery;
    CkSystemLoadState load;
    CkNetState net;
    CkBtState bt;

    bool dnd_enabled;
    uint32_t next_notif_id;
    int notif_count;
    int unread_count;
    CkNotification notifs[CK_STATUS_MAX_NOTIFICATIONS];

    int sni_count;
    CkSniItem sni_items[CK_STATUS_MAX_SNI_ITEMS];
} CkStatusModel;

/* Hardware & system state polling / actions (ck-status-hw.c) */
void ck_status_hw_poll(CkStatusModel *model);
bool ck_status_net_set_wifi_enabled(CkStatusModel *model, bool enabled);
bool ck_status_bt_set_powered(CkStatusModel *model, bool powered);
bool ck_status_bt_set_device_connected(const char *device_dbus_path, bool connect);
void ck_status_sync_unread_file(const CkStatusModel *model);

/* D-Bus Notifications + StatusNotifierWatcher service (ck-status-dbus.c) */
typedef void (*CkStatusToastFn)(const CkNotification *notif, void *userdata);
typedef void (*CkStatusChangedFn)(void *userdata);

bool ck_status_dbus_init(
    CkStatusModel *model,
    CkStatusToastFn on_toast,
    CkStatusChangedFn on_changed,
    void *userdata
);
void ck_status_dbus_step(void);
int  ck_status_dbus_get_fd(void);
void ck_status_dbus_sni_activate(const CkSniItem *item, int x, int y, int button);
void ck_status_dbus_raise_existing(const char *tab_name);
void ck_status_dbus_shutdown(CkStatusModel *model);

/* Model helpers (testable headless) */
uint32_t ck_status_model_add_notification(
    CkStatusModel *model,
    uint32_t replaces_id,
    const char *app_name,
    const char *summary,
    const char *body,
    uint8_t urgency,
    int32_t expire_ms
);
void ck_status_model_mark_all_read(CkStatusModel *model);
void ck_status_model_clear_notifications(CkStatusModel *model);

#endif /* CK_STATUS_H */
