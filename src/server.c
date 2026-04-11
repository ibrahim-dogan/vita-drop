/*
 * VitaDrop - server.c
 * Embedded HTTP Server with Full File Manager API
 * by Ibrahim Dogan
 */

#include "server.h"
#include "frontend.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_CONNECTIONS 5
#define RECV_BUF_SIZE 262144 // 256 KB chunks

TransferStatus g_transfer = {
    .state = TRANSFER_IDLE,
    .bytes_received = 0,
    .bytes_total = 0,
    .filename = {0},
    .error_msg = {0},
    .files_completed = 0,
    .history_count = 0
};

static int server_socket = -1;
static volatile int server_running = 0;
static char local_ip[16] = "0.0.0.0";

/* ============================================================
 * HTTP Response Templates
 * ============================================================ */
static const char *http_200_html =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char *http_200_json =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"status\":\"success\"}";

static const char *http_404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Connection: close\r\n\r\n";

static const char *http_500 =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Connection: close\r\n\r\n";

/* ============================================================
 * URL / Header Parsing Helpers
 * ============================================================ */

// URL-decode a string in-place-ish into output buffer
static void url_decode(const char *src, char *dst, size_t max) {
    size_t oi = 0;
    for (size_t i = 0; src[i] && oi < max - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[oi++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[oi++] = ' ';
        } else {
            dst[oi++] = src[i];
        }
    }
    dst[oi] = '\0';
}

// Extract query parameter value from query string like "path=ux0%3A%2F&foo=bar"
static void get_query_value(const char *query, const char *key, char *out, size_t max) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, '&');
            char encoded[512];
            if (end) {
                size_t len = end - val;
                if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
                strncpy(encoded, val, len);
                encoded[len] = '\0';
            } else {
                strncpy(encoded, val, sizeof(encoded) - 1);
                encoded[sizeof(encoded)-1] = '\0';
            }
            url_decode(encoded, out, max);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

// Parse request line: extract path and query from "GET /api/ls?path=x HTTP/1.1"
static void parse_request(const char *header, char *method, size_t mmax,
                          char *path, size_t pmax, char *query, size_t qmax) {
    method[0] = path[0] = query[0] = '\0';

    // Method
    const char *sp1 = strchr(header, ' ');
    if (!sp1) return;
    size_t mlen = sp1 - header;
    if (mlen >= mmax) mlen = mmax - 1;
    strncpy(method, header, mlen);
    method[mlen] = '\0';

    // URI
    const char *uri = sp1 + 1;
    const char *sp2 = strchr(uri, ' ');
    if (!sp2) sp2 = uri + strlen(uri);

    const char *q = strchr(uri, '?');
    if (q && q < sp2) {
        size_t plen = q - uri;
        if (plen >= pmax) plen = pmax - 1;
        strncpy(path, uri, plen);
        path[plen] = '\0';

        q++;
        size_t qlen = sp2 - q;
        if (qlen >= qmax) qlen = qmax - 1;
        strncpy(query, q, qlen);
        query[qlen] = '\0';
    } else {
        size_t plen = sp2 - uri;
        if (plen >= pmax) plen = pmax - 1;
        strncpy(path, uri, plen);
        path[plen] = '\0';
    }
}

// Parse Content-Length header
static uint64_t get_content_length(const char *headers) {
    const char *p = strstr(headers, "Content-Length:");
    if (!p) p = strstr(headers, "content-length:");
    if (p) {
        p += 15;
        while (*p == ' ' || *p == '\t') p++;
        return strtoull(p, NULL, 10);
    }
    return 0;
}

// Parse X-File-Name header (URL-decoded)
static void get_x_file_name(const char *headers, char *out_name, size_t max_len) {
    const char *p = strstr(headers, "X-File-Name:");
    if (!p) p = strstr(headers, "x-file-name:");
    if (p) {
        p += 12;
        while (*p == ' ' || *p == '\t') p++;
        const char *end = strstr(p, "\r\n");
        if (!end) end = p + strlen(p);

        char encoded[512];
        size_t len = end - p;
        if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
        strncpy(encoded, p, len);
        encoded[len] = '\0';

        url_decode(encoded, out_name, max_len);
    } else {
        snprintf(out_name, max_len, "upload_%u.dat", sceKernelGetProcessTimeLow());
    }
}

// Parse X-Upload-Dir header (URL-decoded)
static void get_x_upload_dir(const char *headers, char *out, size_t max) {
    const char *p = strstr(headers, "X-Upload-Dir:");
    if (!p) p = strstr(headers, "x-upload-dir:");
    if (p) {
        p += 13;
        while (*p == ' ' || *p == '\t') p++;
        const char *end = strstr(p, "\r\n");
        if (!end) end = p + strlen(p);

        char encoded[512];
        size_t len = end - p;
        if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
        strncpy(encoded, p, len);
        encoded[len] = '\0';

        url_decode(encoded, out, max);
    } else {
        strncpy(out, "ux0:/VitaDrop/", max);
    }
}

// JSON-escape a string
static void json_escape(const char *in, char *out, size_t max) {
    size_t oi = 0;
    for (size_t i = 0; in[i] && oi < max - 2; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            out[oi++] = '\\';
        }
        if (oi < max - 1) out[oi++] = in[i];
    }
    out[oi] = '\0';
}

// Ensure nested directory structure exists
static void ensure_nested_dirs(const char *filepath) {
    char temp[512];
    strncpy(temp, filepath, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';

    char *p = temp;
    // Skip device prefix like "ux0:/"
    char *colon = strchr(p, ':');
    if (colon && *(colon+1) == '/') p = colon + 2;

    while ((p = strchr(p, '/')) != NULL) {
        *p = '\0';
        SceIoStat stat;
        if (sceIoGetstat(temp, &stat) < 0) {
            sceIoMkdir(temp, 0777);
        }
        *p = '/';
        p++;
    }
}

/* ============================================================
 * API Handlers
 * ============================================================ */

// GET /api/ls — Directory listing as JSON
static void handle_api_ls(int client_sock, const char *path) {
    SceUID dir = sceIoDopen(path);
    if (dir < 0) {
        const char *err = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n[]";
        sceNetSend(client_sock, err, strlen(err), 0);
        return;
    }

    // Build JSON array into buffer
    char *json = malloc(65536);
    if (!json) {
        sceIoDclose(dir);
        sceNetSend(client_sock, http_500, strlen(http_500), 0);
        return;
    }

    int pos = 0;
    pos += snprintf(json + pos, 65536 - pos, "[");

    SceIoDirent entry;
    memset(&entry, 0, sizeof(entry));
    int first = 1;

    while (sceIoDread(dir, &entry) > 0) {
        if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0) {
            memset(&entry, 0, sizeof(entry));
            continue;
        }

        if (!first && pos < 65530) {
            json[pos++] = ',';
        }
        first = 0;

        int is_dir = SCE_S_ISDIR(entry.d_stat.st_mode);
        char escaped_name[512];
        json_escape(entry.d_name, escaped_name, sizeof(escaped_name));

        pos += snprintf(json + pos, 65536 - pos,
            "{\"n\":\"%s\",\"t\":\"%s\",\"s\":%llu}",
            escaped_name,
            is_dir ? "d" : "f",
            (unsigned long long)entry.d_stat.st_size
        );

        if (pos >= 65500) break; // Safety limit
        memset(&entry, 0, sizeof(entry));
    }

    pos += snprintf(json + pos, 65536 - pos, "]");
    sceIoDclose(dir);

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", pos);

    sceNetSend(client_sock, header, strlen(header), 0);
    sceNetSend(client_sock, json, pos, 0);
    free(json);
}

// GET /api/dl — File download
static void handle_api_download(int client_sock, const char *path) {
    SceIoStat stat;
    if (sceIoGetstat(path, &stat) < 0) {
        sceNetSend(client_sock, http_404, strlen(http_404), 0);
        return;
    }

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        sceNetSend(client_sock, http_500, strlen(http_500), 0);
        return;
    }

    // Extract filename from path
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    char header[768];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %llu\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n"
        "\r\n",
        (unsigned long long)stat.st_size, fname);

    sceNetSend(client_sock, header, strlen(header), 0);

    // Update transfer status for Vita UI
    g_transfer.state = TRANSFER_RECEIVING;
    g_transfer.bytes_total = stat.st_size;
    g_transfer.bytes_received = 0;
    strncpy(g_transfer.filename, fname, sizeof(g_transfer.filename) - 1);
    g_transfer.filename[sizeof(g_transfer.filename) - 1] = '\0';

    char *buf = malloc(RECV_BUF_SIZE);
    if (buf) {
        int r;
        while ((r = sceIoRead(fd, buf, RECV_BUF_SIZE)) > 0) {
            int sent = 0;
            while (sent < r) {
                int s = sceNetSend(client_sock, buf + sent, r - sent, 0);
                if (s <= 0) goto dl_done;
                sent += s;
                g_transfer.bytes_received += s;
            }
        }
dl_done:
        free(buf);
    }

    g_transfer.state = TRANSFER_COMPLETE;
    sceIoClose(fd);
}

// POST /api/mkdir — Create directory
static void handle_api_mkdir(int client_sock, const char *path) {
    sceIoMkdir(path, 0777);
    sceNetSend(client_sock, http_200_json, strlen(http_200_json), 0);
}

// POST /api/delete — Delete file or directory
static void handle_api_delete(int client_sock, const char *path) {
    // Try as file first, then as directory
    int ret = sceIoRemove(path);
    if (ret < 0) {
        ret = sceIoRmdir(path);
    }
    if (ret < 0) {
        char err[256];
        snprintf(err, sizeof(err),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n\r\n"
            "{\"status\":\"error\",\"code\":%d}", ret);
        sceNetSend(client_sock, err, strlen(err), 0);
    } else {
        sceNetSend(client_sock, http_200_json, strlen(http_200_json), 0);
    }
}

// POST /api/upload — Upload file to specified directory
static void handle_api_upload(int client_sock, const char *headers) {
    uint64_t content_length = get_content_length(headers);
    char filename[256];
    get_x_file_name(headers, filename, sizeof(filename));

    char upload_dir[512];
    get_x_upload_dir(headers, upload_dir, sizeof(upload_dir));

    // Ensure trailing slash
    size_t dlen = strlen(upload_dir);
    if (dlen > 0 && upload_dir[dlen-1] != '/') {
        if (dlen < sizeof(upload_dir) - 1) {
            upload_dir[dlen] = '/';
            upload_dir[dlen+1] = '\0';
        }
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%s", upload_dir, filename);

    // Update global state
    g_transfer.state = TRANSFER_RECEIVING;
    g_transfer.bytes_total = content_length;
    g_transfer.bytes_received = 0;
    strncpy(g_transfer.filename, filename, sizeof(g_transfer.filename));

    ensure_nested_dirs(filepath);

    SceUID fd = sceIoOpen(filepath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        strncpy(g_transfer.error_msg, "Could not open file for writing.", sizeof(g_transfer.error_msg));
        g_transfer.state = TRANSFER_ERROR;

        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf),
            "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nFile IO Open Error: 0x%08X", fd);
        sceNetSend(client_sock, err_buf, strlen(err_buf), 0);
        return;
    }

    // Double-buffered I/O
    char *buf_a = malloc(RECV_BUF_SIZE);
    char *buf_b = malloc(RECV_BUF_SIZE);
    if (!buf_a || !buf_b) {
        free(buf_a); free(buf_b);
        sceIoClose(fd);
        const char* ram_err = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nRAM Malloc Failed.";
        sceNetSend(client_sock, ram_err, strlen(ram_err), 0);
        return;
    }

    uint64_t remain = content_length;
    int io_error = 0;
    int net_error = 0;

    char *net_buf = buf_a;
    char *disk_buf = buf_b;
    int pending_len = 0;

    while (remain > 0 && server_running) {
        int to_read = (remain < RECV_BUF_SIZE) ? (int)remain : RECV_BUF_SIZE;
        int r = sceNetRecv(client_sock, net_buf, to_read, 0);
        if (r <= 0) { net_error = 1; break; }

        if (pending_len > 0) {
            int w = sceIoWrite(fd, disk_buf, pending_len);
            if (w != pending_len) { io_error = 1; break; }
        }

        g_transfer.bytes_received += r;
        remain -= r;

        pending_len = r;
        char *tmp = net_buf;
        net_buf = disk_buf;
        disk_buf = tmp;
    }

    if (pending_len > 0 && !io_error && !net_error) {
        int w = sceIoWrite(fd, disk_buf, pending_len);
        if (w != pending_len) io_error = 1;
    }

    sceIoClose(fd);
    free(buf_a);
    free(buf_b);

    if (io_error || net_error || !server_running) {
        g_transfer.state = TRANSFER_ERROR;
        strncpy(g_transfer.error_msg, "Transfer interrupted or disk error.", sizeof(g_transfer.error_msg));

        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf),
            "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nIO:%d NET:%d Run:%d",
            io_error, net_error, server_running);
        sceNetSend(client_sock, err_buf, strlen(err_buf), 0);

        sceIoRemove(filepath);
    } else {
        g_transfer.state = TRANSFER_COMPLETE;
        g_transfer.files_completed++;

        for (int i = 4; i > 0; i--) {
            strncpy((char*)g_transfer.history[i], (char*)g_transfer.history[i-1], 256);
        }
        strncpy((char*)g_transfer.history[0], filename, 256);
        if (g_transfer.history_count < 5) g_transfer.history_count++;

        sceNetSend(client_sock, http_200_json, strlen(http_200_json), 0);
    }
}

/* ============================================================
 * Main Request Router
 * ============================================================ */
static void handle_client(int client_sock) {
    char *header_buf = malloc(4096);
    if (!header_buf) {
        sceNetSend(client_sock, http_500, strlen(http_500), 0);
        sceNetSocketClose(client_sock);
        return;
    }

    // Read HTTP headers
    int header_len = 0;
    while (header_len < 4095) {
        int r = sceNetRecv(client_sock, header_buf + header_len, 1, 0);
        if (r <= 0) break;
        header_len++;
        header_buf[header_len] = '\0';
        if (strstr(header_buf, "\r\n\r\n")) break;
    }

    if (header_len == 0) {
        free(header_buf);
        sceNetSocketClose(client_sock);
        return;
    }

    // Parse request line
    char method[8], req_path[256], req_query[1024];
    parse_request(header_buf, method, sizeof(method),
                  req_path, sizeof(req_path), req_query, sizeof(req_query));

    if (strcmp(method, "GET") == 0) {
        if (strcmp(req_path, "/") == 0 || strcmp(req_path, "/index.html") == 0) {
            // Serve frontend
            sceNetSend(client_sock, http_200_html, strlen(http_200_html), 0);
            sceNetSend(client_sock, frontend_html, strlen(frontend_html), 0);
        }
        else if (strcmp(req_path, "/api/ls") == 0) {
            char path[512];
            get_query_value(req_query, "path", path, sizeof(path));
            if (path[0]) handle_api_ls(client_sock, path);
            else sceNetSend(client_sock, http_404, strlen(http_404), 0);
        }
        else if (strcmp(req_path, "/api/dl") == 0) {
            char path[512];
            get_query_value(req_query, "path", path, sizeof(path));
            if (path[0]) handle_api_download(client_sock, path);
            else sceNetSend(client_sock, http_404, strlen(http_404), 0);
        }
        else {
            sceNetSend(client_sock, http_404, strlen(http_404), 0);
        }
    }
    else if (strcmp(method, "POST") == 0) {
        if (strcmp(req_path, "/api/upload") == 0 || strcmp(req_path, "/upload") == 0) {
            handle_api_upload(client_sock, header_buf);
        }
        else if (strcmp(req_path, "/api/mkdir") == 0) {
            char path[512];
            get_query_value(req_query, "path", path, sizeof(path));
            if (path[0]) handle_api_mkdir(client_sock, path);
            else sceNetSend(client_sock, http_404, strlen(http_404), 0);
        }
        else if (strcmp(req_path, "/api/delete") == 0) {
            char path[512];
            get_query_value(req_query, "path", path, sizeof(path));
            if (path[0]) handle_api_delete(client_sock, path);
            else sceNetSend(client_sock, http_404, strlen(http_404), 0);
        }
        else {
            sceNetSend(client_sock, http_404, strlen(http_404), 0);
        }
    }
    else {
        sceNetSend(client_sock, http_404, strlen(http_404), 0);
    }

    free(header_buf);
    sceNetSocketClose(client_sock);
}

/* ============================================================
 * mDNS Responder Implementation
 * ============================================================ */
static int mdns_running = 0;
static SceUID mdns_thid = -1;

static int mdns_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    int sock = sceNetSocket("mdns_sock", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
    if (sock < 0) return 0;

    int optval = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &optval, sizeof(optval));

#ifdef SCE_NET_SO_REUSEPORT
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEPORT, &optval, sizeof(optval));
#endif

    SceNetSockaddrIn bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = SCE_NET_AF_INET;
    bind_addr.sin_port = sceNetHtons(5353);
    bind_addr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);

    if (sceNetBind(sock, (SceNetSockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        sceNetSocketClose(sock);
        return 0;
    }

    SceNetIpMreq mreq;
    sceNetInetPton(SCE_NET_AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    sceNetSetsockopt(sock, SCE_NET_IPPROTO_IP, SCE_NET_IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    uint8_t buffer[512];

    while (mdns_running) {
        SceNetSockaddrIn src_addr;
        unsigned int src_len = sizeof(src_addr);

        SceNetEpollEvent ev;
        int epoll_id = sceNetEpollCreate("mdns_epoll", 0);
        ev.events = SCE_NET_EPOLLIN;
        ev.data.fd = sock;
        sceNetEpollControl(epoll_id, SCE_NET_EPOLL_CTL_ADD, sock, &ev);

        int n = sceNetEpollWait(epoll_id, &ev, 1, 1000000);
        sceNetEpollDestroy(epoll_id);

        if (n > 0) {
            int len = sceNetRecvfrom(sock, buffer, sizeof(buffer), 0, (SceNetSockaddr *)&src_addr, &src_len);
            if (len >= 12) {
                uint16_t flags = (buffer[2] << 8) | buffer[3];
                uint16_t qdcount = (buffer[4] << 8) | buffer[5];

                if ((flags & 0x8000) == 0 && qdcount > 0) {
                    const uint8_t query_name[] = {8, 'v','i','t','a','d','r','o','p', 5, 'l','o','c','a','l', 0};
                    int match = 0;
                    for (int i = 12; i <= len - (int)sizeof(query_name); i++) {
                        if (memcmp(buffer + i, query_name, sizeof(query_name)) == 0) {
                            match = 1;
                            break;
                        }
                    }

                    if (match) {
                        uint8_t resp[42];
                        memset(resp, 0, sizeof(resp));

                        resp[2] = 0x84; resp[3] = 0x00;
                        resp[6] = 0x00; resp[7] = 0x01;

                        memcpy(resp + 12, query_name, 16);

                        resp[28] = 0x00; resp[29] = 0x01;
                        resp[30] = 0x80; resp[31] = 0x01;
                        resp[32] = 0x00; resp[33] = 0x00; resp[34] = 0x00; resp[35] = 0x78;
                        resp[36] = 0x00; resp[37] = 0x04;

                        SceNetInAddr my_ip;
                        sceNetInetPton(SCE_NET_AF_INET, server_get_ip(), &my_ip);
                        memcpy(resp + 38, &my_ip.s_addr, 4);

                        SceNetSockaddrIn dst;
                        memset(&dst, 0, sizeof(dst));
                        dst.sin_family = SCE_NET_AF_INET;
                        dst.sin_port = sceNetHtons(5353);
                        sceNetInetPton(SCE_NET_AF_INET, "224.0.0.251", &dst.sin_addr);

                        sceNetSendto(sock, resp, sizeof(resp), 0, (SceNetSockaddr *)&dst, sizeof(dst));
                        sceNetSendto(sock, resp, sizeof(resp), 0, (SceNetSockaddr *)&src_addr, src_len);
                    }
                }
            }
        }
    }

    sceNetSocketClose(sock);
    return 0;
}

/* ============================================================
 * Server Lifecycle
 * ============================================================ */
int server_init(int port) {
    SceNetInitParam net_param;
    memset(&net_param, 0, sizeof(net_param));
    net_param.memory = malloc(1 * 1024 * 1024);
    net_param.size = 1 * 1024 * 1024;
    net_param.flags = 0;
    if (sceNetInit(&net_param) < 0) return -1;
    if (sceNetCtlInit() < 0) return -1;

    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) {
        return -1;
    }
    strncpy(local_ip, info.ip_address, sizeof(local_ip));

    server_socket = sceNetSocket("VitaDropServer", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (server_socket < 0) return -1;

    SceNetSockaddrIn server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = SCE_NET_AF_INET;
    server_addr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    server_addr.sin_port = sceNetHtons(port);

    if (sceNetBind(server_socket, (SceNetSockaddr *)&server_addr, sizeof(server_addr)) < 0) return -1;
    if (sceNetListen(server_socket, MAX_CONNECTIONS) < 0) return -1;

    int opt = 1;
    sceNetSetsockopt(server_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &opt, sizeof(opt));

    int srv_rcv = 262144;
    sceNetSetsockopt(server_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &srv_rcv, sizeof(srv_rcv));

    server_running = 1;

    mdns_running = 1;
    mdns_thid = sceKernelCreateThread("VitaDropMDNS", mdns_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (mdns_thid >= 0) {
        sceKernelStartThread(mdns_thid, 0, NULL);
    }

    return 0;
}

void server_run(void) {
    while (server_running) {
        SceNetSockaddrIn client_addr;
        unsigned int addr_len = sizeof(client_addr);

        int client_sock = sceNetAccept(server_socket, (SceNetSockaddr *)&client_addr, &addr_len);

        if (client_sock >= 0) {
            int opt = 0;
            sceNetSetsockopt(client_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &opt, sizeof(opt));

            int rcv_opt = 262144;
            sceNetSetsockopt(client_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &rcv_opt, sizeof(rcv_opt));

            int nodelay = 1;
            sceNetSetsockopt(client_sock, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &nodelay, sizeof(nodelay));

            handle_client(client_sock);
        } else {
            sceKernelDelayThread(10 * 1000);
        }
    }
}

void server_stop(void) {
    server_running = 0;
    mdns_running = 0;
}

void server_cleanup(void) {
    if (server_socket >= 0) {
        sceNetSocketClose(server_socket);
        server_socket = -1;
    }

    if (mdns_thid >= 0) {
        sceKernelWaitThreadEnd(mdns_thid, NULL, NULL);
        mdns_thid = -1;
    }

    sceNetCtlTerm();
    sceNetTerm();
}

const char *server_get_ip(void) {
    return local_ip;
}
