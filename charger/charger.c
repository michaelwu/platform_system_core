/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define DEBUG_UEVENTS
#define CHARGER_KLOG_LEVEL 6

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <cutils/list.h>
#include <cutils/uevent.h>

#include "minui/minui.h"

#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

#define MSEC_PER_SEC            (1000LL)
#define NSEC_PER_MSEC           (1000000LL)

#define SCREEN_ON_TIME          (5 * MSEC_PER_SEC)
#define POWER_ON_KEY_TIME       (5 * MSEC_PER_SEC)
#define UNPLUGGED_SHUTDOWN_TIME (10 * MSEC_PER_SEC)

#define LOGE(x...) do { KLOG_ERROR("charger", x); } while (0)
#define LOGI(x...) do { KLOG_INFO("charger", x); } while (0)
#define LOGV(x...) do { KLOG_DEBUG("charger", x); } while (0)

struct key_state {
    bool pending;
    bool down;
    int64_t timestamp;
};

struct power_supply {
    struct listnode list;
    char name[256];
    char type[32];
    bool online;
    bool valid;
};

struct charger {
    int64_t next_screen_transition;
    int64_t next_key_check;
    int64_t next_pwr_check;
    bool screen_on;

    struct key_state keys[KEY_MAX + 1];
    gr_surface surf_charging;
    int uevent_fd;

    struct listnode supplies;
    int num_supplies;
    int num_supplies_online;

    struct power_supply *battery;
};

struct uevent {
    const char *action;
    const char *path;
    const char *subsystem;
    const char *ps_name;
    const char *ps_type;
    const char *ps_online;
};

static int char_width;
static int char_height;

struct charger charger_state;

/* current time in milliseconds */
static int64_t curr_time_ms(void)
{
    struct timespec tm;
    clock_gettime(CLOCK_MONOTONIC, &tm);
    return tm.tv_sec * MSEC_PER_SEC + (tm.tv_nsec / NSEC_PER_MSEC);
}

static void clear_screen(void)
{
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());
};

static int read_file(const char *path, char *buf, size_t sz)
{
    int fd;
    size_t cnt;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        goto err;

    cnt = read(fd, buf, sz - 1);
    if (cnt <= 0)
        goto err;
    buf[cnt] = '\0';
    if (buf[cnt - 1] == '\n') {
        cnt--;
        buf[cnt] = '\0';
    }

    close(fd);
    return cnt;

err:
    if (fd >= 0)
        close(fd);
    return -1;
}

static int read_file_int(const char *path, int *val)
{
    char buf[32];
    int ret;
    int tmp;
    char *end;

    ret = read_file(path, buf, sizeof(buf));
    if (ret < 0)
        return -1;

    tmp = strtol(buf, &end, 0);
    if (end == buf ||
        ((end < buf+sizeof(buf)) && (*end != '\n' && *end != '\0')))
        goto err;

    *val = tmp;
    return 0;

err:
    return -1;
}

static struct power_supply *find_supply(struct charger *charger,
                                        const char *name)
{
    struct listnode *node;
    struct power_supply *supply;

    list_for_each(node, &charger->supplies) {
        supply = node_to_item(node, struct power_supply, list);
        if (!strncmp(name, supply->name, sizeof(supply->name)))
            return supply;
    }
    return NULL;
}

static struct power_supply *add_supply(struct charger *charger,
                                       const char *name, const char *type,
                                       bool online)
{
    struct power_supply *supply;

    supply = calloc(1, sizeof(struct power_supply));
    if (!supply)
        return NULL;

    strlcpy(supply->name, name, sizeof(supply->name));
    strlcpy(supply->type, type, sizeof(supply->type));
    supply->online = online;
    list_add_tail(&charger->supplies, &supply->list);
    charger->num_supplies++;
    LOGV("... added %s %s %d\n", supply->name, supply->type, online);
    return supply;
}

static void remove_supply(struct charger *charger, struct power_supply *supply)
{
    if (!supply)
        return;
    list_remove(&supply->list);
    charger->num_supplies--;
    free(supply);
}

static void parse_uevent(const char *msg, struct uevent *uevent)
{
    uevent->action = "";
    uevent->path = "";
    uevent->subsystem = "";
    uevent->ps_name = "";
    uevent->ps_online = "";
    uevent->ps_type = "";

    /* currently ignoring SEQNUM */
    while (*msg) {
#ifdef DEBUG_UEVENTS
        LOGV("uevent str: %s\n", msg);
#endif
        if (!strncmp(msg, "ACTION=", 7)) {
            msg += 7;
            uevent->action = msg;
        } else if (!strncmp(msg, "DEVPATH=", 8)) {
            msg += 8;
            uevent->path = msg;
        } else if (!strncmp(msg, "SUBSYSTEM=", 10)) {
            msg += 10;
            uevent->subsystem = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_NAME=", 18)) {
            msg += 18;
            uevent->ps_name = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_ONLINE=", 20)) {
            msg += 20;
            uevent->ps_online = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_TYPE=", 18)) {
            msg += 18;
            uevent->ps_type = msg;
        }

        /* advance to after the next \0 */
        while (*msg++)
            ;
    }

    LOGV("event { '%s', '%s', '%s', '%s', '%s', '%s' }\n",
         uevent->action, uevent->path, uevent->subsystem,
         uevent->ps_name, uevent->ps_type, uevent->ps_online);
}

static void process_ps_uevent(struct charger *charger, struct uevent *uevent)
{
    int online;
    char ps_type[32];
    struct power_supply *supply = NULL;
    int i;
    bool was_online = false;
    bool battery = false;

    if (uevent->ps_type[0] == '\0') {
        char *path;
        int ret;

        if (uevent->path[0] == '\0')
            return;
        ret = asprintf(&path, "/sys/%s/type", uevent->path);
        if (ret <= 0)
            return;
        ret = read_file(path, ps_type, sizeof(ps_type));
        free(path);
        if (ret < 0)
            return;
    } else {
        strlcpy(ps_type, uevent->ps_type, sizeof(ps_type));
    }

    if (!strncmp(ps_type, "Battery", 7))
        battery = true;

    online = atoi(uevent->ps_online);
    supply = find_supply(charger, uevent->ps_name);
    if (supply) {
        was_online = supply->online;
        supply->online = online;
    }

    if (!strcmp(uevent->action, "add")) {
        if (!supply) {
            supply = add_supply(charger, uevent->ps_name, ps_type, online);
            if (!supply) {
                LOGE("cannot add supply '%s' (%s %d)\n", uevent->ps_name,
                     uevent->ps_type, online);
                return;
            }
            /* only pick up the first battery for now */
            if (battery && !charger->battery)
                charger->battery = supply;
        } else {
            LOGE("supply '%s' already exists..\n", uevent->ps_name);
        }
    } else if (!strcmp(uevent->action, "remove")) {
        if (supply) {
            if (charger->battery == supply)
                charger->battery = NULL;
            remove_supply(charger, supply);
            supply = NULL;
        }
    } else if (!strcmp(uevent->action, "change")) {
        if (!supply) {
            LOGE("power supply '%s' not found ('%s' %d)\n",
                 uevent->ps_name, ps_type, online);
            return;
        }
    } else {
        return;
    }

    /* allow battery to be managed in the supply list but make it not
     * contribute to online power supplies. */
    if (!battery) {
        if (was_online && !online)
            charger->num_supplies_online--;
        else if (supply && !was_online && online)
            charger->num_supplies_online++;
    }

    LOGI("power supply %s (%s) %s (action=%s num_online=%d num_supplies=%d)\n",
         uevent->ps_name, ps_type, battery ? "" : online ? "online" : "offline",
         uevent->action, charger->num_supplies_online, charger->num_supplies);
}

static void process_uevent(struct charger *charger, struct uevent *uevent)
{
    if (!strcmp(uevent->subsystem, "power_supply"))
        process_ps_uevent(charger, uevent);
}

#define UEVENT_MSG_LEN  1024
static int handle_uevent_fd(struct charger *charger, int fd)
{
    char msg[UEVENT_MSG_LEN+2];
    int n;

    if (fd < 0)
        return -1;

    while (true) {
        struct uevent uevent;

        n = uevent_kernel_multicast_recv(fd, msg, UEVENT_MSG_LEN);
        if (n <= 0)
            break;
        if (n >= UEVENT_MSG_LEN)   /* overflow -- discard */
            continue;

        msg[n] = '\0';
        msg[n+1] = '\0';

        parse_uevent(msg, &uevent);
        process_uevent(charger, &uevent);
    }

    return 0;
}

static int uevent_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;

    if (!(revents & POLLIN))
        return -1;
    return handle_uevent_fd(charger, fd);
}

/* force the kernel to regenerate the change events for the existing
 * devices, if valid */
static void do_coldboot(struct charger *charger, DIR *d, const char *event,
                        bool follow_links, int max_depth)
{
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY);
    if (fd >= 0) {
        write(fd, event, strlen(event));
        close(fd);
        handle_uevent_fd(charger, charger->uevent_fd);
    }

    while ((de = readdir(d)) && max_depth > 0) {
        DIR *d2;

        LOGV("looking at '%s'\n", de->d_name);

        if ((de->d_type != DT_DIR && !(de->d_type == DT_LNK && follow_links)) ||
           de->d_name[0] == '.') {
            LOGV("skipping '%s' type %d (depth=%d follow=%d)\n",
                 de->d_name, de->d_type, max_depth, follow_links);
            continue;
        }
        LOGV("can descend into '%s'\n", de->d_name);

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            LOGE("cannot openat %d '%s' (%d: %s)\n", dfd, de->d_name,
                 errno, strerror(errno));
            continue;
        }

        d2 = fdopendir(fd);
        if (d2 == 0)
            close(fd);
        else {
            LOGV("opened '%s'\n", de->d_name);
            do_coldboot(charger, d2, event, follow_links, max_depth - 1);
            closedir(d2);
        }
    }
}

static void coldboot(struct charger *charger, const char *path,
                     const char *event)
{
    char str[256];

    LOGV("doing coldboot '%s' in '%s'\n", event, path);
    DIR *d = opendir(path);
    if (d) {
        snprintf(str, sizeof(str), "%s\n", event);
        do_coldboot(charger, d, str, true, 1);
        closedir(d);
    }
}

static int draw_text(const char *str, int x, int y)
{
    int str_len_px = gr_measure(str);

    if (x < 0)
        x = (gr_fb_width() - str_len_px) / 2;
    if (y < 0)
        y = (gr_fb_height() - char_height) / 2;
    gr_text(x, y, str);

    return y + char_height;
}

static void android_green(void)
{
    gr_color(0xa4, 0xc6, 0x39, 255);
}

static void redraw_screen(struct charger *charger)
{
    int surf_height;
    int surf_width;
    int x;
    int y = 0;
    int batt_cap;
    int ret;
    char cap_string[128];
    char cap_path[256];

    clear_screen();

    if (charger->surf_charging) {
        surf_width = gr_get_width(charger->surf_charging);
        surf_height = gr_get_height(charger->surf_charging);
        x = (gr_fb_width() - surf_width) / 2 ;
        y = (gr_fb_height() - surf_height) / 2 ;

        gr_blit(charger->surf_charging, 0, 0,
                surf_width, surf_height,
                x, y);
        y += surf_height;
    } else {
        android_green();
        y = draw_text("Charging!", -1, -1);
    }

    cap_string[0] = '\0';
    if (charger->battery) {
        ret = snprintf(cap_path, sizeof(cap_path),
                       "/sys/class/power_supply/%s/capacity",
                       charger->battery->name);
        if (ret <= 0)
            goto done;
        ret = read_file_int(cap_path, &batt_cap);
        if (ret >= 0)
            snprintf(cap_string, sizeof(cap_string), "%d/100", batt_cap);
    }

    if (cap_string[0] == '\0')
        snprintf(cap_string, sizeof(cap_string), "?\?/100");

    y += 25;
    android_green();
    draw_text(cap_string, -1, y);

done:
    gr_flip();
}

static void update_screen_state(struct charger *charger, int64_t now,
                                bool force)
{
    if (!force && ((now < charger->next_screen_transition) ||
                   (charger->next_screen_transition == -1)))
        return;

    if (!charger->screen_on)
        charger->next_screen_transition = now + SCREEN_ON_TIME;
    else
        charger->next_screen_transition = -1;
    charger->screen_on = !charger->screen_on;

    gr_fb_blank(!charger->screen_on);
    if (charger->screen_on)
        redraw_screen(charger);
    LOGV("[%lld] screen %s\n", now, charger->screen_on ? "on" : "off");
}

static void update_input_state(struct charger *charger,
                               struct input_event *ev,
                               int64_t now)
{
    int down = !!ev->value;

    if (ev->type != EV_KEY || ev->code > KEY_MAX)
        return;

    /* only record the down even timestamp, as the amount
     * of time the key spent not being pressed is not useful */
    if (down)
        charger->keys[ev->code].timestamp = now;
    charger->keys[ev->code].down = down;
    charger->keys[ev->code].pending = true;
    if (down) {
        LOGV("[%lld] key[%d] down\n", now, ev->code);
    } else {
        int64_t duration = now - charger->keys[ev->code].timestamp;
        int64_t secs = duration / 1000;
        int64_t msecs = duration - secs * 1000;
        LOGV("[%lld] key[%d] up (was down for %lld.%lldsec)\n", now,
            ev->code, secs, msecs);
    }
}

static void set_next_key_check(struct charger *charger,
                               struct key_state *key,
                               int64_t timeout)
{
    int64_t then = key->timestamp + timeout;

    if (charger->next_key_check == -1 || then < charger->next_key_check)
        charger->next_key_check = then;
}

static void process_key(struct charger *charger, int code, int64_t now)
{
    struct key_state *key = &charger->keys[code];
    int64_t next_key_check;

    if (code == KEY_POWER) {
        if (key->down) {
            int64_t reboot_timeout = key->timestamp + POWER_ON_KEY_TIME;
            if (now >= reboot_timeout) {
                LOGI("[%lld] rebooting\n", now);
                android_reboot(ANDROID_RB_RESTART, 0, 0);
            } else {
                /* if the key is pressed but timeout hasn't expired,
                 * make sure we wake up at the right-ish time to check
                 */
                set_next_key_check(charger, key, POWER_ON_KEY_TIME);
            }
        } else {
            /* if the power key got released, force screen state cycle */
            if (key->pending)
                update_screen_state(charger, now, true);
        }
    }

    key->pending = false;
}

static void handle_input_state(struct charger *charger, int64_t now)
{
    process_key(charger, KEY_POWER, now);

    if (charger->next_key_check != -1 && now > charger->next_key_check)
        charger->next_key_check = -1;
}

static void handle_power_supply_state(struct charger *charger, int64_t now)
{
    if (charger->num_supplies_online == 0) {
        if (charger->next_pwr_check == -1) {
            charger->next_pwr_check = now + UNPLUGGED_SHUTDOWN_TIME;
            LOGI("[%lld] device unplugged: shutting down in %lld (@ %lld)\n",
                 now, UNPLUGGED_SHUTDOWN_TIME, charger->next_pwr_check);
        } else if (now >= charger->next_pwr_check) {
            LOGI("[%lld] shutting down\n", now);
            android_reboot(ANDROID_RB_POWEROFF, 0, 0);
        } else {
            /* otherwise we already have a shutdown timer scheduled */
        }
    } else {
        /* online supply present, reset shutdown timer if set */
        if (charger->next_pwr_check != -1) {
            LOGI("[%lld] device plugged in: shutdown cancelled\n", now);
            update_screen_state(charger, now, true);
        }
        charger->next_pwr_check = -1;
    }
}

static void wait_next_event(struct charger *charger, int64_t now)
{
    int64_t next_event = INT64_MAX;
    int64_t timeout;
    struct input_event ev;
    int ret;

    LOGV("[%lld] next screen: %lld next key: %lld next pwr: %lld\n", now,
         charger->next_screen_transition, charger->next_key_check,
         charger->next_pwr_check);

    /* TODO: right now it's just screen on/off and keys, but later I'm sure
     *       there will be animations */
    if (charger->next_screen_transition != -1)
        next_event = charger->next_screen_transition;
    if (charger->next_key_check != -1 && charger->next_key_check < next_event)
        next_event = charger->next_key_check;
    if (charger->next_pwr_check != -1 && charger->next_pwr_check < next_event)
        next_event = charger->next_pwr_check;

    if (next_event != -1 && next_event != INT64_MAX)
        timeout = max(0, next_event - now);
    else
        timeout = -1;
    LOGV("[%lld] blocking (%lld)\n", now, timeout);
    ret = ev_wait((int)timeout);
    if (!ret)
        ev_dispatch();
}

static int input_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;
    struct input_event ev;
    int ret;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;
    update_input_state(charger, &ev, curr_time_ms());
    return 0;
}

static void event_loop(struct charger *charger)
{
    int ret;

    while (true) {
        int64_t now = curr_time_ms();

        LOGV("[%lld] event_loop()\n", now);
        handle_input_state(charger, now);
        update_screen_state(charger, now, false);
        handle_power_supply_state(charger, now);

        wait_next_event(charger, now);
    }
}

int main(int argc, char **argv)
{
    int ret;
    struct charger *charger = &charger_state;
    int64_t now = curr_time_ms() - 1;
    int fd;

    list_init(&charger->supplies);

    klog_init();
    klog_set_level(CHARGER_KLOG_LEVEL);

    gr_init();
    gr_font_size(&char_width, &char_height);

    ev_init(input_callback, charger);

    fd = uevent_open_socket(64*1024, true);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        ev_add_fd(fd, uevent_callback, charger);
    }
    charger->uevent_fd = fd;
    coldboot(charger, "/sys/class/power_supply", "add");

    ret = res_create_surface("charging", &charger->surf_charging);
    if (ret < 0) {
        LOGE("Cannot load image\n");
        charger->surf_charging = NULL;
    }

    gr_fb_blank(true);

    charger->next_screen_transition = now - 1;
    charger->next_key_check = -1;
    charger->next_pwr_check = -1;
    charger->screen_on = false;

    event_loop(charger);

    return 0;
}