/*
 * VitaDrop - Main Application UI and Logic (Software rendering)
 * by Ibrahim Dogan
 * 
 * Uses raw double buffered framebuffers since standard vita2d failed to link
 * in the environment. Mocks QR code scaling and manages background threads.
 */

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/power.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>

#include <psp2/appmgr.h>

#include <stdio.h>
#include <string.h>

#include "server.h"
#include "qrcodegen.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define SCREEN_FB_WIDTH 960
#define SCREEN_FB_SIZE  (2 * 1024 * 1024)

#define SERVER_PORT 80

// Modern Dark Theme Colors ABGR (Alpha, Blue, Green, Red)
#define RGBA8(r,g,b,a) ((((a)&0xFF)<<24) | (((b)&0xFF)<<16) | (((g)&0xFF)<<8) | (((r)&0xFF)))

#define COLOR_BG      RGBA8(20, 20, 24, 255)
#define COLOR_PANEL   RGBA8(32, 32, 38, 255)
#define COLOR_TEXT    RGBA8(240, 240, 245, 255)
#define COLOR_TEXT_D  RGBA8(140, 140, 150, 255)
#define COLOR_ACCENT  RGBA8(59, 130, 246, 255)  // Nice bright Blue
#define COLOR_SUCCESS RGBA8(16, 185, 129, 255)  // Emerald Green
#define COLOR_ERROR   RGBA8(239, 68, 68, 255)   // Red
#define COLOR_BLACK   RGBA8(0, 0, 0, 255)
#define COLOR_WHITE   RGBA8(255, 255, 255, 255)

static SceUID server_thid = -1;

// Double buffering
static void *framebuffers[2];
static SceUID fb_memblocks[2];
static int current_fb = 0;
static void *draw_buffer;

// 4x6 small font
static const uint8_t font_4x6[96][6] = {
    {0x0,0x0,0x0,0x0,0x0,0x0}, {0x4,0x4,0x4,0x0,0x4,0x0}, {0xA,0xA,0x0,0x0,0x0,0x0},
    {0xA,0xF,0xA,0xF,0xA,0x0}, {0x4,0xE,0xC,0x2,0xE,0x4}, {0x9,0x2,0x4,0x8,0x9,0x0},
    {0x4,0xA,0x4,0xA,0x5,0x0}, {0x4,0x4,0x0,0x0,0x0,0x0}, {0x2,0x4,0x4,0x4,0x2,0x0},
    {0x4,0x2,0x2,0x2,0x4,0x0}, {0x0,0xA,0x4,0xA,0x0,0x0}, {0x0,0x4,0xE,0x4,0x0,0x0},
    {0x0,0x0,0x0,0x4,0x4,0x8}, {0x0,0x0,0xE,0x0,0x0,0x0}, {0x0,0x0,0x0,0x0,0x4,0x0},
    {0x1,0x2,0x4,0x8,0x0,0x0}, {0x6,0x9,0x9,0x9,0x6,0x0}, {0x4,0xC,0x4,0x4,0xE,0x0},
    {0x6,0x9,0x2,0x4,0xF,0x0}, {0xE,0x1,0x6,0x1,0xE,0x0}, {0x2,0x6,0xA,0xF,0x2,0x0},
    {0xF,0x8,0xE,0x1,0xE,0x0}, {0x6,0x8,0xE,0x9,0x6,0x0}, {0xF,0x1,0x2,0x4,0x4,0x0},
    {0x6,0x9,0x6,0x9,0x6,0x0}, {0x6,0x9,0x7,0x1,0x6,0x0}, {0x0,0x4,0x0,0x4,0x0,0x0},
    {0x0,0x4,0x0,0x4,0x4,0x8}, {0x2,0x4,0x8,0x4,0x2,0x0}, {0x0,0xE,0x0,0xE,0x0,0x0},
    {0x8,0x4,0x2,0x4,0x8,0x0}, {0x6,0x9,0x2,0x0,0x4,0x0}, {0x6,0x9,0xB,0x8,0x6,0x0},
    {0x6,0x9,0xF,0x9,0x9,0x0}, {0xE,0x9,0xE,0x9,0xE,0x0}, {0x6,0x9,0x8,0x9,0x6,0x0},
    {0xE,0x9,0x9,0x9,0xE,0x0}, {0xF,0x8,0xE,0x8,0xF,0x0}, {0xF,0x8,0xE,0x8,0x8,0x0},
    {0x6,0x8,0xB,0x9,0x6,0x0}, {0x9,0x9,0xF,0x9,0x9,0x0}, {0xE,0x4,0x4,0x4,0xE,0x0},
    {0x7,0x1,0x1,0x9,0x6,0x0}, {0x9,0xA,0xC,0xA,0x9,0x0}, {0x8,0x8,0x8,0x8,0xF,0x0},
    {0x9,0xF,0xF,0x9,0x9,0x0}, {0x9,0xD,0xB,0x9,0x9,0x0}, {0x6,0x9,0x9,0x9,0x6,0x0},
    {0xE,0x9,0xE,0x8,0x8,0x0}, {0x6,0x9,0x9,0xA,0x5,0x0}, {0xE,0x9,0xE,0xA,0x9,0x0},
    {0x6,0x8,0x6,0x1,0xE,0x0}, {0xE,0x4,0x4,0x4,0x4,0x0}, {0x9,0x9,0x9,0x9,0x6,0x0},
    {0x9,0x9,0x9,0x6,0x6,0x0}, {0x9,0x9,0xF,0xF,0x9,0x0}, {0x9,0x9,0x6,0x9,0x9,0x0},
    {0x9,0x9,0x6,0x4,0x4,0x0}, {0xF,0x1,0x6,0x8,0xF,0x0}, {0x6,0x4,0x4,0x4,0x6,0x0},
    {0x8,0x4,0x2,0x1,0x0,0x0}, {0x6,0x2,0x2,0x2,0x6,0x0}, {0x4,0xA,0x0,0x0,0x0,0x0},
    {0x0,0x0,0x0,0x0,0xF,0x0}, {0x4,0x2,0x0,0x0,0x0,0x0}, {0x0,0x6,0x9,0xB,0x5,0x0},
    {0x8,0xE,0x9,0x9,0xE,0x0}, {0x0,0x6,0x8,0x8,0x6,0x0}, {0x1,0x7,0x9,0x9,0x7,0x0},
    {0x0,0x6,0xF,0x8,0x6,0x0}, {0x2,0x4,0xE,0x4,0x4,0x0}, {0x0,0x7,0x9,0x7,0x1,0x6},
    {0x8,0xE,0x9,0x9,0x9,0x0}, {0x4,0x0,0x4,0x4,0x4,0x0}, {0x2,0x0,0x2,0x2,0xA,0x4},
    {0x8,0x9,0xA,0xC,0x9,0x0}, {0x4,0x4,0x4,0x4,0x2,0x0}, {0x0,0xA,0xF,0x9,0x9,0x0},
    {0x0,0xE,0x9,0x9,0x9,0x0}, {0x0,0x6,0x9,0x9,0x6,0x0}, {0x0,0xE,0x9,0xE,0x8,0x8},
    {0x0,0x7,0x9,0x7,0x1,0x1}, {0x0,0x6,0x9,0x8,0x8,0x0}, {0x0,0x7,0xC,0x3,0xE,0x0},
    {0x4,0xE,0x4,0x4,0x2,0x0}, {0x0,0x9,0x9,0x9,0x6,0x0}, {0x0,0x9,0x9,0x6,0x6,0x0},
    {0x0,0x9,0x9,0xF,0x6,0x0}, {0x0,0x9,0x6,0x6,0x9,0x0}, {0x0,0x9,0x9,0x7,0x1,0x6},
    {0x0,0xF,0x2,0x4,0xF,0x0}, {0x2,0x4,0xC,0x4,0x2,0x0}, {0x4,0x4,0x4,0x4,0x4,0x0},
    {0x8,0x4,0x6,0x4,0x8,0x0}, {0x0,0x5,0xA,0x0,0x0,0x0}, {0xF,0xF,0xF,0xF,0xF,0xF}
};

static void fill_solid(uint32_t color) {
    uint32_t *pixels = (uint32_t *)draw_buffer;
    for (int i = 0; i < SCREEN_FB_WIDTH * SCREEN_H; i++) {
        pixels[i] = color;
    }
}

static void draw_box(int x, int y, int w, int h, uint32_t color) {
    uint32_t *pixels = (uint32_t *)draw_buffer;
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= SCREEN_H) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= SCREEN_W) continue;
            pixels[py * SCREEN_FB_WIDTH + px] = color;
        }
    }
}

static void draw_char(int x, int y, char c, int scale, uint32_t fg) {
    int idx = c - 32;
    if (idx < 0 || idx >= 96) idx = 0;
    uint32_t *pixels = (uint32_t *)draw_buffer;
    
    for (int row = 0; row < 6; row++) {
        uint8_t line = font_4x6[idx][row];
        for (int col = 0; col < 4; col++) {
            if ((line >> (3 - col)) & 1) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
                            pixels[py * SCREEN_FB_WIDTH + px] = fg;
                        }
                    }
                }
            }
        }
    }
}

static void draw_string(int x, int y, const char *str, int scale, uint32_t fg) {
    int orig_x = x;
    while (*str) {
        if (*str == '\n') {
            y += 6 * scale + scale;
            x = orig_x;
        } else {
            draw_char(x, y, *str, scale, fg);
            x += 4 * scale + scale;
        }
        str++;
    }
}

static void draw_qrcode_centered(const char *url, int center_x, int center_y, int scale) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
    
    bool ok = qrcodegen_encodeText(url, tempBuffer, qrcode, qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) return;

    int size = qrcodegen_getSize(qrcode);
    int bg_padding = 4 * scale;
    int total_size = (size * scale) + (bg_padding * 2);
    
    int start_x = center_x - (total_size / 2) + bg_padding;
    int start_y = center_y - (total_size / 2) + bg_padding;
    
    // Draw white background
    draw_box(center_x - (total_size / 2), 
             center_y - (total_size / 2), 
             total_size, 
             total_size, 
             COLOR_WHITE);

    // Draw blocks
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                draw_box(start_x + (x * scale), 
                         start_y + (y * scale), 
                         scale, scale, 
                         COLOR_BLACK);
            }
        }
    }
}

// Convert bytes to human readable format
static void format_bytes(uint64_t bytes, char *out, size_t max_len) {
    if (bytes < 1024) snprintf(out, max_len, "%llu B", bytes);
    else if (bytes < 1024 * 1024) snprintf(out, max_len, "%.1f KB", bytes / 1024.0f);
    else if (bytes < 1024 * 1024 * 1024) snprintf(out, max_len, "%.2f MB", bytes / (1024.0f * 1024.0f));
    else snprintf(out, max_len, "%.2f GB", bytes / (1024.0f * 1024.0f * 1024.0f));
}

static void swap_buffers(void) {
    SceDisplayFrameBuf fb = {
        .size = sizeof(SceDisplayFrameBuf),
        .base = draw_buffer,
        .pitch = SCREEN_FB_WIDTH,
        .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        .width = SCREEN_W,
        .height = SCREEN_H
    };
    sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    
    current_fb = 1 - current_fb;
    draw_buffer = framebuffers[current_fb];
}

static int server_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    server_run();
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // Allocate double framebuffer memory
    for (int i = 0; i < 2; i++) {
        fb_memblocks[i] = sceKernelAllocMemBlock("display", 
                                              SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 
                                              SCREEN_FB_SIZE, NULL);
        if (fb_memblocks[i] < 0) return -1;
        sceKernelGetMemBlockBase(fb_memblocks[i], &framebuffers[i]);
        memset(framebuffers[i], 0, SCREEN_FB_SIZE);
    }
    
    draw_buffer = framebuffers[0];
    current_fb = 0;

    // Set initial dummy frame buffer to prevent black screen crash
    swap_buffers();
    fill_solid(COLOR_BG);
    draw_string(50, 50, "Initializing system...", 4, COLOR_TEXT);
    swap_buffers();

    // Load modules
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    // Wake mode override
    scePowerSetConfigurationMode(0x0080);

    // Init controller sampling
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    // Init network server
    fill_solid(COLOR_BG);
    draw_string(50, 50, "Initializing network...", 4, COLOR_TEXT);
    swap_buffers();

    int server_ok = server_init(SERVER_PORT);
    
    char url[128] = "http://unknown";
    if (server_ok == 0) {
        if (SERVER_PORT == 80) {
            snprintf(url, sizeof(url), "http://vitadrop.local");
        } else {
            snprintf(url, sizeof(url), "http://vitadrop.local:%d", SERVER_PORT);
        }
        
        server_thid = sceKernelCreateThread("VitaDropServerThread", server_thread, 
                                            0x10000100, 0x10000, 0, 0, NULL);
        if (server_thid >= 0) {
            sceKernelStartThread(server_thid, 0, NULL);
        }
    }

    SceCtrlData pad, old_pad;
    memset(&old_pad, 0, sizeof(old_pad));
    int running = 1;

    uint64_t last_bytes = 0;
    uint32_t last_time = 0;
    float current_speed = 0.0f;
    int time_left = 0;
    int show_info = 0;

    while (running) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_pad.buttons;
        old_pad = pad;

        if (pressed & (SCE_CTRL_START | SCE_CTRL_CIRCLE)) {
            running = 0;
            break;
        }
        
        if (pressed & SCE_CTRL_SQUARE) {
            sceAppMgrLaunchAppByUri(0x20000, "psgm:play?titleid=VITASHELL");
            sceKernelExitProcess(0);
        }
        
        if (pressed & SCE_CTRL_TRIANGLE) {
            show_info = !show_info;
        }

        // Prevent OLED auto-dimming and network suspend
        sceKernelPowerTick(0);

        fill_solid(COLOR_BG);

        // ===== TOP BAR (y 0-50) =====
        draw_box(0, 0, SCREEN_W, 50, COLOR_PANEL);
        draw_string(20, 15, "VitaDrop", 4, COLOR_ACCENT);
        draw_string(220, 20, "File Manager", 3, COLOR_TEXT_D);

        if (server_ok == 0) {
            // ===== LEFT COLUMN (x 20-580, y 60-490) =====

            // URL Card (y 60-120)
            draw_box(20, 60, 560, 55, COLOR_PANEL);
            draw_string(30, 68, "URL:", 2, COLOR_TEXT_D);
            draw_string(30, 86, url, 3, COLOR_ACCENT);

            // IP Fallback
            char ip_line[128];
            snprintf(ip_line, sizeof(ip_line), "IP: http://%s", server_get_ip());
            draw_string(300, 68, ip_line, 2, COLOR_TEXT_D);

            // Status Card (y 125-310)
            draw_box(20, 125, 560, 185, COLOR_PANEL);

            // Time Tracking
            if (g_transfer.state == TRANSFER_RECEIVING) {
                uint32_t now = sceKernelGetProcessTimeLow();
                if (last_time == 0) {
                    last_time = now;
                    last_bytes = g_transfer.bytes_received;
                } else if (now - last_time >= 500000) {
                    float bytes_diff = (float)(g_transfer.bytes_received - last_bytes);
                    float time_diff = (float)(now - last_time) / 1000000.0f;
                    current_speed = (bytes_diff / time_diff) / (1024.0f * 1024.0f);

                    if (current_speed > 0.001f && g_transfer.bytes_total > 0) {
                        time_left = (int)(((float)(g_transfer.bytes_total - g_transfer.bytes_received) / 1024.0f / 1024.0f) / current_speed);
                    } else {
                        time_left = 0;
                    }
                    last_time = now;
                    last_bytes = g_transfer.bytes_received;
                }
            } else {
                last_time = 0;
                current_speed = 0.0f;
                time_left = 0;
            }

            char format_recv[32];
            char format_total[32];

            switch (g_transfer.state) {
                case TRANSFER_IDLE:
                    draw_string(35, 140, "Status:", 2, COLOR_TEXT_D);
                    draw_string(35, 165, "Waiting for connection...", 3, COLOR_TEXT);
                    draw_string(35, 200, "Open the URL above in", 2, COLOR_TEXT_D);
                    draw_string(35, 220, "your browser to start.", 2, COLOR_TEXT_D);
                    break;
                case TRANSFER_COMPLETE:
                case TRANSFER_RECEIVING: {
                    // Same layout for both — COMPLETE just freezes at 100%
                    int is_done = (g_transfer.state == TRANSFER_COMPLETE);

                    if (is_done)
                        draw_string(35, 138, "Complete:", 2, COLOR_SUCCESS);
                    else
                        draw_string(35, 138, "Transferring:", 2, COLOR_ACCENT);

                    char short_name[50];
                    strncpy(short_name, g_transfer.filename, 46);
                    short_name[46] = '\0';
                    if (strlen(g_transfer.filename) > 46) strcat(short_name, "...");
                    draw_string(35, 158, short_name, 2, COLOR_TEXT);

                    // Progress bar — always present
                    draw_box(35, 183, 530, 10, RGBA8(40,40,48,255));
                    if (is_done) {
                        draw_box(35, 183, 530, 10, COLOR_SUCCESS);
                        draw_string(35, 200, "Done", 2, COLOR_SUCCESS);
                    } else {
                        format_bytes(g_transfer.bytes_received, format_recv, sizeof(format_recv));
                        if (g_transfer.bytes_total > 0) {
                            format_bytes(g_transfer.bytes_total, format_total, sizeof(format_total));
                            int prg_w = ((float)g_transfer.bytes_received / g_transfer.bytes_total) * 530;
                            if (prg_w > 530) prg_w = 530;
                            draw_box(35, 183, prg_w, 10, COLOR_SUCCESS);

                            char stats[128];
                            int pct = (g_transfer.bytes_received * 100) / g_transfer.bytes_total;
                            snprintf(stats, sizeof(stats), "%s / %s (%d%%)", format_recv, format_total, pct);
                            draw_string(35, 200, stats, 2, COLOR_TEXT_D);
                        } else {
                            char stats[128];
                            snprintf(stats, sizeof(stats), "Transferred: %s", format_recv);
                            draw_string(35, 200, stats, 2, COLOR_TEXT_D);
                        }
                    }

                    // Speed / ETA line — always in same position
                    if (!is_done) {
                        char speed_str[64];
                        snprintf(speed_str, sizeof(speed_str), "%.2f MB/s", current_speed);
                        draw_string(35, 222, speed_str, 3, COLOR_ACCENT);

                        char eta_str[64];
                        if (time_left > 0) snprintf(eta_str, sizeof(eta_str), "ETA: %ds", time_left);
                        else snprintf(eta_str, sizeof(eta_str), "Finishing...");
                        draw_string(250, 225, eta_str, 2, COLOR_SUCCESS);
                    }
                    break;
                }
                case TRANSFER_ERROR:
                    draw_string(35, 140, "Error:", 2, COLOR_ERROR);
                    draw_string(35, 165, g_transfer.error_msg, 2, COLOR_TEXT);
                    break;
            }

            // Session stats line
            char session_stats[128];
            snprintf(session_stats, sizeof(session_stats), "Session: %d transfers", g_transfer.files_completed);
            draw_string(35, 290, session_stats, 2, COLOR_TEXT_D);

            // ===== RECENT TRANSFERS (y 320-490) =====
            if (g_transfer.history_count > 0) {
                draw_box(20, 320, 560, 170, COLOR_PANEL);
                draw_string(35, 332, "Recent Transfers", 2, COLOR_TEXT_D);
                draw_box(35, 352, 530, 1, RGBA8(50,50,58,255)); // divider

                for (int i = 0; i < g_transfer.history_count && i < 5; i++) {
                    char h_str[48];
                    strncpy(h_str, (char * volatile)g_transfer.history[i], 44);
                    h_str[44] = '\0';
                    if (strlen((char * volatile)g_transfer.history[i]) > 44) strcat(h_str, "...");
                    draw_string(45, 362 + (i * 22), h_str, 2, COLOR_SUCCESS);
                }
            }

            // ===== RIGHT COLUMN — QR CODE (x 600-940, y 60-310) =====
            draw_box(600, 60, 340, 250, COLOR_PANEL);
            draw_string(610, 70, "Scan to Connect", 2, COLOR_TEXT_D);
            draw_qrcode_centered(url, 770, 200, 5);

        } else {
            // Error state
            draw_box(20, 80, 920, 80, COLOR_PANEL);
            draw_string(40, 100, "Error: Could not initialize network.", 3, COLOR_ERROR);
            draw_string(40, 130, "Make sure Wi-Fi is enabled in Vita Settings.", 2, COLOR_TEXT_D);
        }

        // ===== BOTTOM BAR (y 500-544) =====
        draw_box(0, 500, SCREEN_W, 44, COLOR_PANEL);
        draw_string(20, 516, "[SQUARE] VitaShell", 2, COLOR_ACCENT);
        draw_string(300, 516, "[TRIANGLE] Info", 2, COLOR_ACCENT);
        draw_string(SCREEN_W - 200, 516, "[START] Exit", 2, COLOR_TEXT_D);

        // ===== INFO MODAL =====
        if (show_info) {
            // Draw a darkened background overlay
            // (We don't have true alpha blending in double-buffer raw, so we draw a solid panel)
            draw_box(180, 100, 600, 300, COLOR_BG);
            draw_box(182, 102, 596, 296, COLOR_PANEL); // Border effect
            
            draw_string(210, 120, "VitaDrop - Info & Credits", 3, COLOR_ACCENT);
            
            draw_string(210, 170, "- Connect your PC/Phone to the same Wi-Fi network", 2, COLOR_TEXT);
            draw_string(210, 200, "- Open the URL or scan the QR Code on your browser", 2, COLOR_TEXT);
            draw_string(210, 230, "- Upload, Download & Manage your filesystem wirelessly", 2, COLOR_TEXT);
            
            draw_string(210, 280, "Created by: Ibrahim Dogan", 2, COLOR_SUCCESS);
            draw_string(210, 310, "Email: ibrahimmdogann@gmail.com", 2, COLOR_SUCCESS);
            
            draw_string(210, 350, "Press [TRIANGLE] to close", 2, COLOR_TEXT_D);
        }

        swap_buffers();
    }

    server_stop();
    if (server_thid >= 0) {
        sceKernelWaitThreadEnd(server_thid, NULL, NULL);
    }
    server_cleanup();

    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    sceKernelExitProcess(0);
    return 0;
}
