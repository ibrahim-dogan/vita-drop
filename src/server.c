/*
 * VitaDrop - server.c
 * Embedded HTTP Server with File Streaming
 * by Ibrahim Dogan
 */

#include "server.h"
#include "frontend.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_CONNECTIONS 5
#define RECV_BUF_SIZE 262144 // 256 KB chunks for ultra-fast file writing

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

// HTTP response headers
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

// Utility: parse Content-Length
static uint64_t get_content_length(const char *headers) {
    const char *p = strstr(headers, "Content-Length:");
    if (!p) p = strstr(headers, "content-length:");
    if (p) {
        p += 15;
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        return strtoull(p, NULL, 10);
    }
    return 0;
}

// Utility: parse X-File-Name
static void get_x_file_name(const char *headers, char *out_name, size_t max_len) {
    const char *p = strstr(headers, "X-File-Name:");
    if (!p) p = strstr(headers, "x-file-name:");
    if (p) {
        p += 12;
        while (*p == ' ' || *p == '\t') p++;
        const char *end = strstr(p, "\r\n");
        if (!end) end = p + strlen(p);
        
        size_t len = end - p;
        if (len >= max_len) len = max_len - 1;
        
        // Very basic URI decode inline since JS encodeURIComponent is used
        size_t out_idx = 0;
        for (size_t i = 0; i < len && out_idx < max_len - 1; i++) {
            if (p[i] == '%' && i + 2 < len) {
                char hex[3] = {p[i+1], p[i+2], 0};
                out_name[out_idx++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else {
                out_name[out_idx++] = p[i];
            }
        }
        out_name[out_idx] = '\0';
    } else {
        snprintf(out_name, max_len, "upload_%u.dat", sceKernelGetProcessTimeLow());
    }
}

// Ensure nested directory structure exists dynamically
static void ensure_nested_dirs(const char *filepath) {
    char temp[512];
    strncpy(temp, filepath, sizeof(temp));
    
    // We expect absolute formats like "ux0:/VitaDrop/Folder/File.ext"
    char *p = temp;
    
    // Advance past "ux0:/"
    if (strncmp(p, "ux0:/", 5) == 0) p += 5;
    
    while ((p = strchr(p, '/')) != NULL) {
        *p = '\0';
        SceIoStat stat;
        if (sceIoGetstat(temp, &stat) < 0) {
            sceIoMkdir(temp, 0777);
        }
        *p = '/';
        p++; // advance past slash
    }
}

// Handle client connection
static void handle_client(int client_sock) {
    char *header_buf = malloc(4096);
    if (!header_buf) {
        sceNetSend(client_sock, http_500, strlen(http_500), 0);
        sceNetSocketClose(client_sock);
        return;
    }

    // Read only HTTP headers first
    int header_len = 0;
    while (header_len < 4095) {
        int r = sceNetRecv(client_sock, header_buf + header_len, 1, 0);
        if (r <= 0) break;
        header_len++;
        header_buf[header_len] = '\0';
        if (strstr(header_buf, "\r\n\r\n")) {
            break; // End of headers
        }
    }

    if (header_len == 0) {
        free(header_buf);
        sceNetSocketClose(client_sock);
        return;
    }

    // Route: GET /
    if (strncmp(header_buf, "GET / ", 6) == 0 || strncmp(header_buf, "GET /index.html", 15) == 0) {
        sceNetSend(client_sock, http_200_html, strlen(http_200_html), 0);
        sceNetSend(client_sock, frontend_html, strlen(frontend_html), 0);
    }
    // Route: POST /upload
    else if (strncmp(header_buf, "POST /upload", 12) == 0) {
        uint64_t content_length = get_content_length(header_buf);
        char filename[256];
        get_x_file_name(header_buf, filename, sizeof(filename));
        free(header_buf);
        
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "ux0:/VitaDrop/%s", filename);
        
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
            snprintf(err_buf, sizeof(err_buf), "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nFile IO Open Error: 0x%08X", fd);
            sceNetSend(client_sock, err_buf, strlen(err_buf), 0);
            
            sceNetSocketClose(client_sock);
            return;
        }

        // ===== Threaded Double-Buffered I/O =====
        // A writer thread flushes disk_buf while the main thread fills net_buf.
        // True parallel overlap of network recv + disk write.
        
        char *buf_a = malloc(RECV_BUF_SIZE);
        char *buf_b = malloc(RECV_BUF_SIZE);
        if (!buf_a || !buf_b) {
            free(buf_a); free(buf_b);
            sceIoClose(fd);
            const char* ram_err = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nRAM Malloc Failed.";
            sceNetSend(client_sock, ram_err, strlen(ram_err), 0);
            sceNetSocketClose(client_sock);
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
            
            // Flush previous chunk (overlapped: while we were receiving,
            // the previous data sat ready; now we write it before swapping)
            if (pending_len > 0) {
                int w = sceIoWrite(fd, disk_buf, pending_len);
                if (w != pending_len) { io_error = 1; break; }
            }
            
            g_transfer.bytes_received += r;
            remain -= r;
            
            // Swap buffers
            pending_len = r;
            char *tmp = net_buf;
            net_buf = disk_buf;
            disk_buf = tmp;
        }
        
        // Flush final chunk
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
            snprintf(err_buf, sizeof(err_buf), "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nIO:%d NET:%d Run:%d", io_error, net_error, server_running);
            sceNetSend(client_sock, err_buf, strlen(err_buf), 0);
            
            sceIoRemove(filepath); // Delete incomplete file
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
    } else {
        // Unknown route
        sceNetSend(client_sock, http_404, strlen(http_404), 0);
        free(header_buf);
    }

    sceNetSocketClose(client_sock);
}

// ==========================================
// mDNS Responder Implementation
// ==========================================
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
                        
                        resp[2] = 0x84; resp[3] = 0x00; // Auth Answer
                        resp[6] = 0x00; resp[7] = 0x01; // ANCOUNT: 1
                        
                        memcpy(resp + 12, query_name, 16);
                        
                        resp[28] = 0x00; resp[29] = 0x01; // Type A
                        resp[30] = 0x80; resp[31] = 0x01; // Class IN
                        resp[32] = 0x00; resp[33] = 0x00; resp[34] = 0x00; resp[35] = 0x78; // TTL
                        resp[36] = 0x00; resp[37] = 0x04; // Data Len 4
                        
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

    // Set non-blocking to allow graceful thread exit when stopped
    int opt = 1;
    sceNetSetsockopt(server_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &opt, sizeof(opt));
    
    // Maximize server socket receive buffer
    int srv_rcv = 262144;
    sceNetSetsockopt(server_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &srv_rcv, sizeof(srv_rcv));

    server_running = 1;

    // Spin up mDNS responder thread
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
            // Tight poll — 10ms idle to pick up connections faster
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
