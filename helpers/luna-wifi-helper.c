/* Narrow privileged helper for Wi-Fi drivers which ConnMan cannot scan.
 * Installed setuid-root; accepts only a sysfs-verified wireless interface and
 * the operations up, down, or scan. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int valid_name(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= IFNAMSIZ) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' &&
            name[i] != '-' && name[i] != '.') return 0;
    return 1;
}

static int wireless(const char* name) {
    char path[PATH_MAX];
    struct stat st;
    if (!valid_name(name)) return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", name);
    if (stat(path, &st) == 0) return 1;
    snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", name);
    return lstat(path, &st) == 0;
}

static int set_up(const char* name, int up) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", name);
    int rc = ioctl(fd, SIOCGIFFLAGS, &req);
    if (rc == 0) {
        if (up) req.ifr_flags |= IFF_UP;
        else req.ifr_flags &= (short)~IFF_UP;
        rc = ioctl(fd, SIOCSIFFLAGS, &req);
    }
    if (rc != 0) fprintf(stderr, "luna-wifi-helper: %s: %s\n", name, strerror(errno));
    close(fd);
    return rc != 0;
}

static const char* iw_path(void) {
    static const char* paths[] = { "/usr/sbin/iw", "/usr/bin/iw", "/sbin/iw", "/bin/iw" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (access(paths[i], X_OK) == 0) return paths[i];
    return NULL;
}

static const char* tool_path(const char* const* paths, size_t count) {
    for (size_t i = 0; i < count; i++) if (access(paths[i], X_OK) == 0) return paths[i];
    return NULL;
}

static int wait_tool(char* const args[]) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) { execv(args[0], args); _exit(127); }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int write_quoted(FILE* f, const char* value) {
    if (fputc('"', f) == EOF) return 0;
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        if (*p < 32 || *p == 127) return 0;
        if ((*p == '"' || *p == '\\') && fputc('\\', f) == EOF) return 0;
        if (fputc(*p, f) == EOF) return 0;
    }
    return fputc('"', f) != EOF;
}

static int connect_wpa(const char* iface, const char* ssid) {
    char password[128] = {0};
    if (!fgets(password, sizeof(password), stdin)) return 1;
    password[strcspn(password, "\r\n")] = 0;
    size_t plen = strlen(password), slen = strlen(ssid);
    if (!slen || slen > 32 || (plen && (plen < 8 || plen > 63))) {
        fprintf(stderr, "luna-wifi-helper: invalid SSID or WPA passphrase length\n");
        return 1;
    }
    static const char* wpa_paths[] = { "/usr/sbin/wpa_supplicant", "/usr/bin/wpa_supplicant", "/sbin/wpa_supplicant" };
    const char* wpa = tool_path(wpa_paths, sizeof(wpa_paths) / sizeof(wpa_paths[0]));
    if (!wpa) { fprintf(stderr, "luna-wifi-helper: wpa_supplicant not found\n"); return 1; }
    char conf[PATH_MAX], pidfile[PATH_MAX];
    snprintf(conf, sizeof(conf), "/run/luna-wifi-%s.conf", iface);
    snprintf(pidfile, sizeof(pidfile), "/run/luna-wifi-%s.pid", iface);
    int fd = open(conf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return 1;
    FILE* f = fdopen(fd, "w");
    int ok = f && fprintf(f, "ctrl_interface=/run/wpa_supplicant\nupdate_config=0\nnetwork={\n ssid=") > 0 &&
             write_quoted(f, ssid);
    if (ok) {
        if (plen) ok = fprintf(f, "\n psk=") > 0 && write_quoted(f, password);
        else ok = fprintf(f, "\n key_mgmt=NONE") > 0;
    }
    if (ok) ok = fprintf(f, "\n}\n") > 0 && fflush(f) == 0;
    if (f) fclose(f); else close(fd);
    memset(password, 0, sizeof(password));
    if (!ok || set_up(iface, 1) != 0) { unlink(conf); return 1; }
    char* wargs[] = { (char*)wpa, (char*)"-B", (char*)"-i", (char*)iface,
                      (char*)"-c", conf, (char*)"-P", pidfile, NULL };
    ok = wait_tool(wargs);
    unlink(conf);
    if (!ok) { fprintf(stderr, "luna-wifi-helper: wpa_supplicant failed\n"); return 1; }
    static const char* udhcpc_paths[] = { "/sbin/udhcpc", "/usr/sbin/udhcpc", "/usr/bin/udhcpc" };
    const char* dhcp = tool_path(udhcpc_paths, sizeof(udhcpc_paths) / sizeof(udhcpc_paths[0]));
    if (dhcp) {
        char* dargs[] = { (char*)dhcp, (char*)"-q", (char*)"-n", (char*)"-i", (char*)iface, NULL };
        if (!wait_tool(dargs)) { fprintf(stderr, "luna-wifi-helper: DHCP failed\n"); return 1; }
    }
    return 0;
}

int main(int argc, char** argv) {
    if ((argc != 3 && argc != 4) || !wireless(argv[2])) {
        fprintf(stderr, "usage: luna-wifi-helper up|down|scan|dump INTERFACE | connect INTERFACE SSID\n");
        return 2;
    }
    if (!strcmp(argv[1], "connect") && argc == 4) return connect_wpa(argv[2], argv[3]);
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "up")) return set_up(argv[2], 1);
    if (!strcmp(argv[1], "down")) return set_up(argv[2], 0);
    if (!strcmp(argv[1], "scan") || !strcmp(argv[1], "dump")) {
        if (set_up(argv[2], 1) != 0) return 1;
        const char* iw = iw_path();
        if (!iw) { fprintf(stderr, "luna-wifi-helper: iw not found\n"); return 127; }
        char* const args_scan[] = { (char*)iw, (char*)"dev", argv[2], (char*)"scan", NULL };
        char* const args_dump[] = { (char*)iw, (char*)"dev", argv[2], (char*)"scan", (char*)"dump", NULL };
        char* const* args = !strcmp(argv[1], "dump") ? args_dump : args_scan;
        execv(iw, args);
        return 126;
    }
    fprintf(stderr, "luna-wifi-helper: unsupported operation\n");
    return 2;
}
