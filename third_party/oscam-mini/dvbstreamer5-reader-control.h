/* V593: Private OSCam-mini reader control. Included only in oscam.c.
 * Socket is a local root-only SOCK_SEQPACKET endpoint (not OSCam WebIF);
 * requests are applied by OSCam's own reader job dispatcher, not systemctl.
 */
#ifndef DVBSTREAMER5_OSCAM_READER_CONTROL_H
#define DVBSTREAMER5_OSCAM_READER_CONTROL_H
#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>

#define DVBSTREAMER5_READER_CONTROL_PATH "/run/dvbstreamer5-oscam-mini-reader.sock"

static int dvbstreamer5_reader_label_is_safe(const char *name)
{
    size_t i, n;
    if (!name) return 0;
    n = strlen(name);
    if (n == 0 || n > 64) return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == '@')) return 0;
    }
    return 1;
}

static void *dvbstreamer5_reader_control_worker(void *unused)
{
    (void)unused;
    const char *path = DVBSTREAMER5_READER_CONTROL_PATH;
    int server = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un address;
    struct stat existing;
    if (server < 0) return NULL;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);

    if (lstat(path, &existing) == 0) {
        int probe;
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid()) {
            close(server);
            return NULL;
        }
        probe = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (probe >= 0) {
            if (connect(probe, (struct sockaddr *)&address, sizeof(address)) == 0) {
                close(probe);
                close(server);
                return NULL; /* another daemon owns this active socket */
            }
            close(probe);
        }
        if (unlink(path) != 0) {
            close(server);
            return NULL;
        }
    }
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(server);
        return NULL;
    }
    if (chmod(path, 0600) != 0 || listen(server, 4) != 0) {
        unlink(path);
        close(server);
        return NULL;
    }
    cs_log("DVBStreamer5 OSCam-mini reader control started (local root only)");
    while (!exit_oscam) {
        int peer = accept(server, NULL, NULL);
        if (peer < 0) {
            if (errno == EINTR) continue;
            break;
        }
        (void)fcntl(peer, F_SETFD, FD_CLOEXEC);
        {
            /* SO_PEERCRED checks the caller process, not a forged HTTP header.
             * OSCam-mini and DVBStreamer5 systemd services both run as root.
             */
            struct { pid_t pid; uid_t uid; gid_t gid; } credentials;
            socklen_t size = sizeof(credentials);
            char command[80] = {0};
            const char *response = "ERROR unauthorized";
            ssize_t n;
            if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
                size != sizeof(credentials) || credentials.uid != 0 ||
                geteuid() != 0) {
                (void)send(peer, response, strlen(response), MSG_NOSIGNAL);
                close(peer);
                continue;
            }
            n = recv(peer, command, sizeof(command)-1, 0);
            response = "ERROR invalid request";
            if (n > 8 && n < (ssize_t)sizeof(command) &&
                memcmp(command, "restart ", 8) == 0 &&
                dvbstreamer5_reader_label_is_safe(command + 8)) {
                struct s_reader *rdr = get_reader_by_label(command + 8);
                if (!rdr || !rdr->enable) response = "ERROR reader not found or disabled";
                else if (!rdr->client) response = "ERROR reader not initialized";
                else if (add_job(rdr->client, ACTION_READER_RESTART, NULL, 0)) {
                    response = "OK queued";
                    cs_log("DVBStreamer5 OSCam-mini reader %s restart queued", rdr->label);
                } else response = "ERROR reader restart job rejected";
            }
            (void)send(peer, response, strlen(response), MSG_NOSIGNAL);
            close(peer);
        }
    }
    close(server);
    unlink(path);
    return NULL;
}
#endif
#endif
