/*
 * Command-line tool to manage and monitor the rlite stack.
 *
 * Copyright (C) 2015-2016 Nextworks
 * Author: Michal Koutenský <koutak.m@gmail.com>
 *
 * This file is part of rlite.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

#include "rlite/list.h"
#include "rlite/utils.h"
#include "wpa-supplicant/wpa_ctrl.h"

/* clang-format off */
/*
 * Usage examples:
 *
 * How to scan available networks:
 * $ ./test-wifi -i wlp3s0
 * How to connect to a WPA2 network:
 * $ ./test-wifi -i wlp3s0 -a network_ssid -p network_password
 * How to terminate wpa_supplicant before exiting:
 * $ ./test-wifi -i wlp3s0 -t
 *
 * test-wifi expects a config file to be located at /etc/wpa_supplicant/rlite.conf
 * Example of wpa_supplicant configuration
 *
ctrl_interface=/var/run/wpa_supplicant
#ctrl_interface_group=wheel
eapol_version=1
ap_scan=1
fast_reauth=1
update_config=1
 *
 */
/* clang-format on */

#define RL_WIFI_F_WEP 0x1
#define RL_WIFI_F_WPS 0x2
#define RL_WIFI_F_ESS 0x4
#define RL_WPA_F_ACTIVE 0x1
#define RL_WPA_F_PSK 0x2
#define RL_WPA_F_CCMP 0x4
#define RL_WPA_F_TKIP 0x8
#define RL_WPA_F_PREAUTH 0x10

#define RL_WIFI_SSID_LEN 129

#define RL_WPA_SUPPLICANT_CONF_PATH "/etc/wpa_supplicant/rlite.conf"
#define RL_WPA_SUPPLICANT_PID_PATH "/run/wpa_supplicant.pid"
#define RL_WIFI_DRIVER "nl80211"

struct wifi_network {
    struct list_head list;
    char bssid[18];
    unsigned int freq;
    int signal;
    uint32_t wifi_flags;
    uint32_t wpa1_flags;
    uint32_t wpa2_flags;
    char ssid[RL_WIFI_SSID_LEN];
};

static char *
create_ctrl_path(const char *ctrl_dir, const char *inf)
{
    /* parameter to wpa_ctrl_open needs to be path in config + interface */
    const size_t len_dir  = strlen(ctrl_dir);
    const size_t len_inf  = strlen(inf);
    const size_t len = len_dir + len_inf + 2;
    char *ctrl_path = malloc(len);
    if (!ctrl_path) {
        PE("Out of memory\n");
        return NULL;
    }
    snprintf(ctrl_path, len, "%s/%s", ctrl_dir, inf);
    return ctrl_path;
}

static char *
get_ctrl_dir_from_config(const char *config)
{
    const int buf_size = 128;
    char buffer[buf_size];
    char *ctrl_dir = NULL;
    FILE *config_fd;

    config_fd = fopen(config, "r");
    if (!config_fd) {
        PE("Could not open config file\n"
                        "Please make sure there is a config file located at %s"
                        " and that it is accessible\n",
                        RL_WPA_SUPPLICANT_CONF_PATH);
        return NULL;
    }

    while (fgets(buffer, buf_size, config_fd)) {
        if (!strncmp("ctrl_interface", buffer, 14)) {
            if (!sscanf(buffer, "ctrl_interface=%m[^\n]", &ctrl_dir)) {
                PE("get_ctrl_dir_from_config: sscanf() failed\n");
            }
            break;
        }
    }

    fclose(config_fd);

    return ctrl_dir;
}

static int
start_wpa_supplicant(const char *config, const char *pid_file, const char *inf, char **ctrl_path)
{
    const char *driver = RL_WIFI_DRIVER;
    char *ctrl_dir;
    int pid;

    ctrl_dir = get_ctrl_dir_from_config(config);

    if (!ctrl_dir) {
        PE("Could not get ctrl_interface from the config file\n");
        return -1;
    }

    /* Start wpa_supplicant as a child process. */
    pid = fork();

    if (pid <= -1) {
        perror("fork()");
        return -1;
    } else if (pid == 0) {
        PD("Executing wpa_supplicant\n");
        execlp("wpa_supplicant", "-D", driver, "-i", inf, "-c", config,
               "-P", pid_file, "-B", NULL);
        perror("execlp(wpa_supplicant)");
        return -1;
    }

    /* Wait a bit to make sure it started. */
    sleep(2);

    *ctrl_path = create_ctrl_path(ctrl_dir, inf);
    free(ctrl_dir);
    if (!*ctrl_path) {
        return -1;
    }
    return 0;
}

#define RL_WPA_SUPPLICANT_MAX_MSG_LEN   4096

/* sends a command to the control interface */
static int
send_cmd(struct wpa_ctrl *ctrl_conn, const char *cmd)
{
    char buf[RL_WPA_SUPPLICANT_MAX_MSG_LEN];
    size_t len = sizeof(buf);
    len--;

    if (wpa_ctrl_request(ctrl_conn, cmd, strlen(cmd), buf, &len, NULL)) {
        return -1;
    }
    buf[len] = '\0';

    PD_S("%s", buf);
    if (len > 0 && buf[len - 1] != '\n')
        PD_S("\n");

    return 0;
}

static char *
send_cmd_get_resp(struct wpa_ctrl *ctrl_conn, const char *cmd)
{
    size_t len = RL_WPA_SUPPLICANT_MAX_MSG_LEN;
    char *buf  = malloc(len);
    if (!buf) {
        PE("Out of memory\n");
        return NULL;
    }
    len--;

    if (wpa_ctrl_request(ctrl_conn, cmd, strlen(cmd), buf, &len, NULL)) {
        free(buf);
        PE("wpa_ctrl_request() failed: %s.\n", strerror(errno));
        return NULL;
    }
    buf[len] = '\0';

    PD_S("%s", buf);
    if (len > 0 && buf[len - 1] != '\n')
        PD_S("\n");
    return buf;
}

/* reads a message from the control interface */
static size_t
recv_msg(struct wpa_ctrl *ctrl_conn, char *buf, size_t len)
{
    len--;
    if (wpa_ctrl_recv(ctrl_conn, buf, &len)) {
        return 0;
    }
    buf[len] = '\0';

    PD_S("%s", buf);
    if (len > 0 && buf[len - 1] != '\n') {
        PD_S("\n");
    }
    return len;
}

static const char *
parse_wpa_flags(u_int32_t *flags, const char *flagstr)
{
    const char *p = flagstr;
    int len = index(flagstr, ']') - flagstr;

    while (*p != ']') {
        if (*p == '-' || *p == '+') {
            p++;
            len--;
        } else if ((len >= 3) && (!strncmp(p, "PSK", 3))) {
            *flags |= RL_WPA_F_PSK;
            p += 3;
            len -= 3;
        } else if ((len >= 4) && (!strncmp(p, "CCMP", 4))) {
            *flags |= RL_WPA_F_CCMP;
            p += 4;
            len -= 4;
        } else if ((len >= 4) && (!strncmp(p, "TKIP", 4))) {
            *flags |= RL_WPA_F_TKIP;
            p += 4;
            len -= 4;
        } else if ((len >= 7) && (!strncmp(p, "preauth", 7))) {
            *flags |= RL_WPA_F_PREAUTH;
            p += 7;
            len -= 7;
        }
    }

    return p;
}

static void
parse_wifi_flags(struct wifi_network *elem, char *flagstr)
{
    const char *p = flagstr;
    int len;

    elem->wifi_flags = 0;
    elem->wpa1_flags = 0;
    elem->wpa2_flags = 0;

    while (*p != '\0') {
        len = index(p, ']') - p;
        if (*p == ']') {
            p++;
            continue;
        }
        if (len < 4) {
            p += len;
        } else if (len == 4) {
            if (!strncmp(p, "[WPS]", 5)) {
                elem->wifi_flags |= RL_WIFI_F_WPS;
            } else if (!strncmp(p, "[WEP]", 5)) {
                elem->wifi_flags |= RL_WIFI_F_WEP;
            } else if (!strncmp(p, "[ESS]", 5)) {
                elem->wifi_flags |= RL_WIFI_F_ESS;
            };
            p += 5;
        } else {
            if (!strncmp(p, "[WPA2", 5)) {
                p += 5;
                elem->wpa2_flags |= RL_WPA_F_ACTIVE;
                p = parse_wpa_flags(&elem->wpa2_flags, p);
            } else if (!strncmp(p, "[WPA", 4)) {
                p += 4;
                elem->wpa1_flags |= RL_WPA_F_ACTIVE;
                p = parse_wpa_flags(&elem->wpa1_flags, p);
            }
        }
    }
}

static int
parse_networks(struct list_head *list, const char *networks)
{
    const char *p = networks;
    struct wifi_network *elem;
    char flagstr[100];

    /* skip header */
    while (*p != '\0' && *(p++) != '\n')
        ;

    while (*p != '\0') {
        elem = malloc(sizeof(struct wifi_network));
        if (!elem) {
            return -1;
        }
        if (sscanf(p, "%17c %u %d %s %128[^\n]c\n", elem->bssid, &elem->freq,
                   &elem->signal, flagstr, elem->ssid) == EOF) {
            free(elem);
            return -1;
        }
        parse_wifi_flags(elem, flagstr);
        list_add_tail(&elem->list, list);
        while (*p != '\0' && *(p++) != '\n')
            ;
    }
    return 0;
}

static int
wait_for_msg(struct wpa_ctrl *ctrl_conn, const char *msg)
{
    char buf[RL_WPA_SUPPLICANT_MAX_MSG_LEN];
    const int len = sizeof(buf);
    int ret;

    struct pollfd ctrl_pollfd;
    int msg_len = strlen(msg);

    ctrl_pollfd.fd     = wpa_ctrl_get_fd(ctrl_conn);
    ctrl_pollfd.events = POLLIN;

    do {
        ctrl_pollfd.events = POLLIN;
        ret                = poll(&ctrl_pollfd, 1, 5000);
        if (ret == -1) {
            perror("poll()");
            return -1;
        }
        if (ctrl_pollfd.revents & POLLIN) {
            if (!recv_msg(ctrl_conn, buf, len)) {
                perror("Failed to read messages from wpa_supplicant");
                return -1;
            };
        }
    } while (strncmp(buf + 3, msg, msg_len));

    return 0;
}

static int
wifi_scan(struct wpa_ctrl *ctrl_conn, struct list_head *list)
{
    char *networks;

    list_init(list);

    if (send_cmd(ctrl_conn, "SCAN")) {
        perror("Failed to send \"SCAN\" command");
        return -1;
    }

    /* loop until we get a 'scanning done' message */
    if (wait_for_msg(ctrl_conn, WPA_EVENT_SCAN_RESULTS)) {
        return -1;
    }

    networks = send_cmd_get_resp(ctrl_conn, "SCAN_RESULTS");
    if (!networks) {
        perror("Failed to send \"SCAN_RESULTS\" command");
        return -1;
    }

    if (parse_networks(list, networks)) {
        perror("Failed to parse networks");
    }
    free(networks);

    return 0;
}

static void
wifi_destroy_network_list(struct list_head *list)
{
    struct wifi_network *cur, *tmp;
    list_for_each_entry_safe (cur, tmp, list, list) {
        list_del_init(&cur->list);
        free(cur);
    }
}

static void
wpa_flags_print(u_int32_t flags)
{
    if (flags & RL_WPA_F_PSK) {
        PD_S("-PSK");
    }
    if (flags & RL_WPA_F_CCMP) {
        PD_S("-CCMP");
    }
    if (flags & RL_WPA_F_TKIP) {
        PD_S("+TKIP");
    }
    if (flags & RL_WPA_F_PREAUTH) {
        PD_S("-preauth");
    }
}

static void
wifi_networks_print(const struct list_head *networks)
{
    struct wifi_network *cur;
    PD_S("bssid / frequency / signal level / flags / ssid\n");
    list_for_each_entry (cur, networks, list) {
        PD_S("%s\t%u\t%d\t", cur->bssid, cur->freq, cur->signal);
        if (cur->wpa1_flags & RL_WPA_F_ACTIVE) {
            PD_S("[WPA");
            wpa_flags_print(cur->wpa1_flags);
            PD_S("]");
        }
        if (cur->wpa2_flags & RL_WPA_F_ACTIVE) {
            PD_S("[WPA2");
            wpa_flags_print(cur->wpa2_flags);
            PD_S("]");
        }
        if (cur->wifi_flags & RL_WIFI_F_WPS) {
            PD_S("[WPS]");
        }
        if (cur->wifi_flags & RL_WIFI_F_WEP) {
            PD_S("[WEP]");
        }
        if (cur->wifi_flags & RL_WIFI_F_ESS) {
            PD_S("[ESS]");
        }
        PD_S("\t%s\n", cur->ssid);
    }
}

static struct wifi_network *
wifi_find_network_by_ssid(const struct list_head *networks, const char *ssid)
{
    struct wifi_network *cur;
    int len;

    len = strlen(ssid);

    list_for_each_entry (cur, networks, list) {
        if (!strncmp(cur->ssid, ssid, len)) {
            return cur;
        }
    }

    return NULL;
}

static int
requires_psk(const struct list_head *networks, const char *ssid)
{
    const struct wifi_network *network;

    network = wifi_find_network_by_ssid(networks, ssid);
    if (!network) {
        return -1;
    }

    return ((network->wifi_flags & RL_WIFI_F_WEP) ||
            (network->wpa1_flags & RL_WPA_F_ACTIVE) ||
            (network->wpa2_flags & RL_WPA_F_ACTIVE))
               ? 1
               : 0;
}

/*
 * A wrapper function for the SET_NETWORK wpa_supplicant command.
 * The format of the command is "SET_NETWORK <ID> <VARIABLE> <VALUE>".
 * This function takes <ID>, <VARIABLE> and <VALUE> as string parameters,
 * creates the command string to send and sends it to the control connection.
 */
static int
wifi_set_network(struct wpa_ctrl *ctrl_conn, const char *id, const char *var,
            const char *val)
{
    const char cmd[]  = "SET_NETWORK";
    const int cmd_len = 11;
    int len, id_len, var_len, val_len;
    char *msg;

    id_len  = strlen(id);
    var_len = strlen(var);
    val_len = strlen(val);

    /* three spaces + quotes for value + null terminator */
    len = cmd_len + id_len + var_len + val_len + 6;

    msg = malloc(len);
    if (!msg) {
        PE("Out of memory\n");
        return -1;
    }

    snprintf(msg, len, "%s %s %s \"%s\"", cmd, id, var, val);
    send_cmd(ctrl_conn, msg);

    free(msg);

    return 0;
}

/*
 * A wrapper function for the ENABLE_NETWORK wpa_supplicant command.
 * The format of the command is "ENABLE_NETWORK <ID>".
 * This function takes <ID> as a string parameter, creates the command string
 * and sends it to the control connection. It then waits for a positive
 * response.
 */
static int
wifi_enable_network(struct wpa_ctrl *ctrl_conn, const char *id)
{
    const char cmd[]  = "ENABLE_NETWORK";
    const int cmd_len = 14;
    int len, id_len;
    char *msg;

    id_len = strlen(id);

    /* a space + null terminator */
    len = cmd_len + id_len + 2;

    msg = malloc(len);
    if (!msg) {
        PE("Out of memory\n");
        return -1;
    }

    snprintf(msg, len, "%s %s", cmd, id);
    send_cmd(ctrl_conn, msg);

    free(msg);

    return wait_for_msg(ctrl_conn, WPA_EVENT_CONNECTED);
}

/*
 * Creates a new network configuration in wpa_supplicant.
 * @id   : output parameter, the id of the newly created network configuration
 * @ssid : ssid of the network to add
 * @psk  : psk of network to add, NULL if network is open (without psk)
 * Sends the "ADD_NETWORK" command to create the configuration, then sets
 * ssid (and possibly psk) using the "SET_NETWORK" command.
 */
static int
wifi_add_network(struct wpa_ctrl *ctrl_conn, char id[8], const char *ssid,
                 const char *psk)
{
    const char *resp;

    resp = send_cmd_get_resp(ctrl_conn, "ADD_NETWORK");
    if (!resp) {
        return -1;
    }
    sscanf(resp, "%8[^\n]c\n", id);

    if (wifi_set_network(ctrl_conn, id, "ssid", ssid)) {
        return -1;
    }
    if (psk) {
        if (wifi_set_network(ctrl_conn, id, "psk", psk)) {
            return -1;
        }
    }

    return 0;
}

static int
wifi_associate_to_network(struct wpa_ctrl *ctrl_conn,
                          const struct list_head *networks, const char *ssid,
                          const char *psk)
{
    int has_psk;
    char id[8];
    int ret = 0;

    has_psk = requires_psk(networks, ssid);
    if (has_psk == -1) {
        PE("Cannot find network with SSID %s.\n", ssid);
        return -1;
    }

    if (has_psk) {
        if (psk == NULL) {
            PE("Network with SSID %s requires PSK.\n", ssid);
            return -1;
        }
        ret = wifi_add_network(ctrl_conn, id, ssid, psk);
    } else {
        ret = wifi_add_network(ctrl_conn, id, ssid, NULL);
    }

    if (ret) {
        return -1;
    }

    return wifi_enable_network(ctrl_conn, id);
}

static void
usage()
{
    printf("test-wifi -i INF [-d] [-t] [-a SSID] [-p PSK]\n"
           "   -i INF  : name of interface to use\n"
           "   -d      : print debug messages\n"
           "   -t      : terminate wpa_supplicant before exiting\n"
           "   -a SSID : associate to network with given SSID\n"
           "   -p PSK  : use given PSK when associating\n"
           "A minimal wpa_supplicant.conf is expected at %s\n"
           "(See 'man 5 wpa_supplicant.conf' for details)\n",
           RL_WPA_SUPPLICANT_CONF_PATH);
}

int
main(int argc, char **argv)
{
    int opt;
    char *inf = NULL, *config = RL_WPA_SUPPLICANT_CONF_PATH;
    char *pid_file = RL_WPA_SUPPLICANT_PID_PATH;

    char *ctrl_dir, *ctrl_path;
    struct wpa_ctrl *ctrl_conn = NULL;
    struct list_head networks;
    int debug     = 0;
    int terminate = 0;

    char *ssid = NULL;
    char *psk  = NULL;

    int ret;

    while ((opt = getopt(argc, argv, "i:hda:p:t")) != -1) {
        switch (opt) {
        case 'i':
            inf = optarg;
            break;
        case 'h':
            usage();
            return 0;
        case 'd':
            debug = 1;
            break;
        case 'a':
            ssid = optarg;
            break;
        case 'p':
            psk = optarg;
            break;
        case 't':
            terminate = 1;
            break;
        }
    }

    if (!inf || (!ssid && psk)) {
        PE("Invalid arguments\n\n");
        usage();
        return -1;
    }

    /* Try to access the pidfile of a running wpa_supplicant process. */
    ret = access(pid_file, R_OK);
    if (ret) {
        switch (errno) {
            case ENOENT:
                /* Couldn't find a pidifle. We assume there is no process,
                 * let's start a wpa_supplicant instance. */
                if (start_wpa_supplicant(config, pid_file, inf, &ctrl_path)) {
                    return -1;
                }
                break;
            default:
                perror("Failed to access the PID file");
                return -1;
        }
    } else {
        /* A pidfile was found, so a wpa_supplicant process is already running.
         * We just need to recover the control directory from its configuration
         * file. */
        ctrl_dir = get_ctrl_dir_from_config(config);
        if (!ctrl_dir) {
            PE("Could not get ctrl_interface from the config file\n");
            return -1;
        }
        ctrl_path = create_ctrl_path(ctrl_dir, inf);
        free(ctrl_dir);
        if (!ctrl_path) {
            return -1;
        }
    }

    /* Create a control connection with the child and get the handle. */
    ctrl_conn = wpa_ctrl_open(ctrl_path);
    if (!ctrl_conn) {
        perror("Failed to connect to the wpa_supplicant control interface");
        return -1;
    }

    free(ctrl_path);

    /* Attach to the child so that we can send control messages. */
    wpa_ctrl_attach(ctrl_conn);

    ret = wifi_scan(ctrl_conn, &networks);
    if (debug && !ret) {
        wifi_networks_print(&networks);
    }

    if (!ret && ssid) {
        /* We were asked to associate to a WiFi network. */
        ret = wifi_associate_to_network(ctrl_conn, &networks, ssid, psk);
    }

    /* Cleanup. */
    wifi_destroy_network_list(&networks);
    if (terminate) {
        PD("Terminating wpa_supplicant\n");
        send_cmd(ctrl_conn, "TERMINATE");
    }
    wpa_ctrl_detach(ctrl_conn);
    wpa_ctrl_close(ctrl_conn);

    return ret;
}
