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
    scePowerSetConfigurationMode(0x0080); // override auto standby mostly

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

    while (running) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_pad.buttons;
        old_pad = pad;

        if (pressed & (SCE_CTRL_START | SCE_CTRL_CIRCLE)) {
            running = 0;
            break;
        }

        fill_solid(COLOR_BG);

        // Modern Header
        draw_box(0, 0, SCREEN_W, 60, COLOR_PANEL);
        draw_string(40, 20, "VitaDrop", 4, COLOR_ACCENT);
        draw_string(180, 24, "Wi-Fi File Transfer", 3, COLOR_TEXT_D);

        if (server_ok == 0) {
            // Instructions Area (Left Side)
            draw_string(40, 110, "1. Connect your PC/Phone to the same Wi-Fi network.", 2, COLOR_TEXT_D);
            draw_string(40, 140, "2. Open a web browser and visit the address below,", 2, COLOR_TEXT_D);
            draw_string(40, 170, "   or scan the QR code using your phone's camera.", 2, COLOR_TEXT_D);
            
            // URL Box Card
            draw_box(40, 210, 520, 60, COLOR_PANEL);
            draw_string(65, 230, url, 4, COLOR_ACCENT);

            // Right side QR Code (Centered at X=760, Y=240)
            draw_qrcode_centered(url, 760, 240, 6);

            // Status Card Area (Bottom Left)
            draw_box(40, 310, 520, 150, COLOR_PANEL);

            // Time Tracking
            if (g_transfer.state == TRANSFER_RECEIVING) {
                uint32_t now = sceKernelGetProcessTimeLow();
                if (last_time == 0) {
                    last_time = now;
                    last_bytes = g_transfer.bytes_received;
                } else if (now - last_time >= 500000) { // Update every 0.5 sec
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
                    draw_string(60, 330, "Status:", 3, COLOR_TEXT_D);
                    draw_string(60, 365, "Waiting for connection...", 3, COLOR_TEXT);
                    break;
                case TRANSFER_RECEIVING:
                    draw_string(60, 330, "Receiving:", 3, COLOR_ACCENT);
                    
                    char short_name[64];
                    strncpy(short_name, g_transfer.filename, 60);
                    short_name[60] = '\0';
                    if(strlen(g_transfer.filename) > 60) strcat(short_name, "...");
                    draw_string(210, 335, short_name, 2, COLOR_TEXT);

                    format_bytes(g_transfer.bytes_received, format_recv, sizeof(format_recv));
                    if (g_transfer.bytes_total > 0) {
                        format_bytes(g_transfer.bytes_total, format_total, sizeof(format_total));
                        
                        // Progress Bar background
                        draw_box(60, 370, 480, 12, RGBA8(40,40,45,255));
                        // Progress Fill
                        int prg_w = ((float)g_transfer.bytes_received / g_transfer.bytes_total) * 480;
                        draw_box(60, 370, prg_w, 12, COLOR_SUCCESS);

                        char stats1[128];
                        int percent = (g_transfer.bytes_received * 100) / g_transfer.bytes_total;
                        snprintf(stats1, sizeof(stats1), "%s / %s (%d%%)", format_recv, format_total, percent);
                        draw_string(60, 395, stats1, 2, COLOR_TEXT_D);
                    } else {
                        // Unknown total bytes
                        char stats1[128];
                        snprintf(stats1, sizeof(stats1), "Downloaded: %s", format_recv);
                        draw_string(60, 370, stats1, 2, COLOR_TEXT);
                    }
                    
                    char speed_str[64];
                    snprintf(speed_str, sizeof(speed_str), "Speed: %.2f MB/s", current_speed);
                    draw_string(60, 420, speed_str, 2, COLOR_ACCENT);
                    
                    char eta_str[64];
                    if (time_left > 0) snprintf(eta_str, sizeof(eta_str), "ETA: %d sec", time_left);
                    else snprintf(eta_str, sizeof(eta_str), "ETA: Finishing...");
                    draw_string(320, 420, eta_str, 2, COLOR_SUCCESS);
                    break;
                case TRANSFER_COMPLETE:
                    draw_string(60, 350, "Status: Transfer Complete!", 3, COLOR_SUCCESS);
                    break;
                case TRANSFER_ERROR:
                    draw_string(60, 330, "System Error Encountered:", 3, COLOR_ERROR);
                    draw_string(60, 370, g_transfer.error_msg, 2, COLOR_TEXT);
                    break;
            }

            char session_stats[128];
            snprintf(session_stats, sizeof(session_stats), "Total files saved to ux0:/downloads/  |  Session count: %d", g_transfer.files_completed);
            draw_string(40, 500, session_stats, 2, COLOR_TEXT_D);

        } else {
            draw_box(40, 110, 880, 100, COLOR_PANEL);
            draw_string(60, 130, "Error: Could not initialize Network Socket.", 3, COLOR_ERROR);
            draw_string(60, 165, "Make sure your Wi-Fi is enabled in Vita System Settings.", 2, COLOR_TEXT_D);
        }

        draw_string(SCREEN_W - 220, 500, "Press START to Exit", 2, COLOR_TEXT_D);

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
