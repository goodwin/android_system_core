/*
 * Copyright (C) 2012-2014 The Android Open Source Project
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

#include <limits.h>
#include <sys/cdefs.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cutils/sockets.h>
#include <log/logger.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>

#include "LogListener.h"

LogListener::LogListener(LogBuffer *buf, LogReader *reader) :
        SocketListener(getLogSocket(), false),
        logbuf(buf),
        reader(reader) {
}

bool LogListener::onDataAvailable(SocketClient *cli) {
    static bool name_set;
    if (!name_set) {
        prctl(PR_SET_NAME, "logd.writer");
        name_set = true;
    }

    char buffer[sizeof_log_id_t + sizeof(uint16_t) + sizeof(log_time)
        + LOGGER_ENTRY_MAX_PAYLOAD];
    struct iovec iov = { buffer, sizeof(buffer) };
    memset(buffer, 0, sizeof(buffer));

    char control[CMSG_SPACE(sizeof(struct ucred))] __aligned(4);
    struct msghdr hdr = {
        NULL,
        0,
        &iov,
        1,
        control,
        sizeof(control),
        0,
    };

    int socket = cli->getSocket();

    ssize_t n = recvmsg(socket, &hdr, 0);
    if (n <= (ssize_t)(sizeof(android_log_header_t))) {
        return false;
    }

    struct ucred *cred = NULL;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
    while (cmsg != NULL) {
        if (cmsg->cmsg_level == SOL_SOCKET
                && cmsg->cmsg_type  == SCM_CREDENTIALS) {
            cred = (struct ucred *)CMSG_DATA(cmsg);
            break;
        }
        cmsg = CMSG_NXTHDR(&hdr, cmsg);
    }

    if (cred == NULL) {
        return false;
    }

    if (cred->uid == AID_LOGD) {
        // ignore log messages we send to ourself.
        // Such log messages are often generated by libraries we depend on
        // which use standard Android logging.
        return false;
    }

    android_log_header_t *header = reinterpret_cast<android_log_header_t *>(buffer);
    if (/* header->id < LOG_ID_MIN || */ header->id >= LOG_ID_MAX || header->id == LOG_ID_KERNEL) {
        return false;
    }

    char *msg = ((char *)buffer) + sizeof(android_log_header_t);
    n -= sizeof(android_log_header_t);

    // NB: hdr.msg_flags & MSG_TRUNC is not tested, silently passing a
    // truncated message to the logs.

    if (logbuf->log((log_id_t)header->id, header->realtime,
            cred->uid, cred->pid, header->tid, msg,
            ((size_t) n <= USHRT_MAX) ? (unsigned short) n : USHRT_MAX) >= 0) {
        reader->notifyNewLog();
    }

    return true;
}

int LogListener::getLogSocket() {
    static const char socketName[] = "logdw";
    int sock = android_get_control_socket(socketName);

    if (sock < 0) {
        sock = socket_local_server(socketName,
                                   ANDROID_SOCKET_NAMESPACE_RESERVED,
                                   SOCK_DGRAM);
    }

    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0) {
        return -1;
    }
    return sock;
}
