#include "netcfg.h"
#include "osc.h"

#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000
#define CONFIG_DIR      "sdmc:/3ds/osc3ds"
#define CONFIG_PATH     CONFIG_DIR "/config.ini"

NetConfig net_cfg = { "192.168.1.100", 9000, "/3ds" };
volatile unsigned long net_sent = 0, net_errors = 0;

static int sock = -1;
static u32 *soc_buf = NULL;
static struct sockaddr_in dest;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

void net_config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcmp(k, "host"))        snprintf(net_cfg.host, sizeof net_cfg.host, "%s", v);
        else if (!strcmp(k, "port"))   net_cfg.port = atoi(v);
        else if (!strcmp(k, "prefix")) snprintf(net_cfg.prefix, sizeof net_cfg.prefix, "%s", v);
    }
    fclose(f);
}

void net_config_save(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# osc3ds config\nhost=%s\nport=%d\nprefix=%s\n", net_cfg.host, net_cfg.port, net_cfg.prefix);
    fclose(f);
}

void net_update_dest(void) {
    memset(&dest, 0, sizeof dest);
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)net_cfg.port);
    dest.sin_addr.s_addr = inet_addr(net_cfg.host);
}

bool net_init(void) {
    soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buf) return false;
    if (R_FAILED(socInit(soc_buf, SOC_BUFFERSIZE))) { free(soc_buf); soc_buf = NULL; return false; }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;
    net_update_dest();
    return true;
}

void net_exit(void) {
    if (sock >= 0) close(sock);
    sock = -1;
    if (soc_buf) { socExit(); free(soc_buf); soc_buf = NULL; }
}

void net_send(const char *address, const char *types, ...) {
    if (sock < 0) return;
    uint8_t buf[256];
    va_list ap;
    va_start(ap, types);
    size_t len = osc_vencode(buf, sizeof buf, address, types, ap);
    va_end(ap);
    if (len == 0) return;
    if (sendto(sock, buf, len, 0, (struct sockaddr *)&dest, sizeof dest) < 0) net_errors++;
    else net_sent++;
}

static bool keyboard_input(const char *hint, const char *initial, char *out, size_t outlen) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, (int)outlen - 1);
    swkbdSetNumpadKeys(&kb, L'.', 0);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, initial);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    char tmp[64];
    if (swkbdInputText(&kb, tmp, sizeof tmp) != SWKBD_BUTTON_CONFIRM) return false;
    snprintf(out, outlen, "%s", tmp);
    return true;
}

void net_edit_host(void) {
    char buf[64];
    if (keyboard_input("Target IP address", net_cfg.host, buf, sizeof buf)) {
        snprintf(net_cfg.host, sizeof net_cfg.host, "%s", buf);
        net_config_save();
        net_update_dest();
    }
}

void net_edit_port(void) {
    char buf[64], portstr[8];
    snprintf(portstr, sizeof portstr, "%d", net_cfg.port);
    if (keyboard_input("Target UDP port", portstr, buf, 6)) {
        int p = atoi(buf);
        if (p > 0 && p < 65536) {
            net_cfg.port = p;
            net_config_save();
            net_update_dest();
        }
    }
}

void net_edit_settings(void) {
    net_edit_host();
    net_edit_port();
}

const char *net_local_ip(void) {
    static char s[20];
    u32 ip = (u32)gethostid();
    snprintf(s, sizeof s, "%lu.%lu.%lu.%lu", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
    return s;
}
