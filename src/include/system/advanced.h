/**
 * @file advanced.h
 * @brief 高级网络功能头文件 (Go: handlers/advanced.go)
 */

#ifndef ADVANCED_H
#define ADVANCED_H

#include "mongoose.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int hour;
    int minute;
} ClockTime;

/* ClockLock配置 */
typedef struct {
    int enabled;
    char rat[16];
    char band[16];
    char arfcn[16];
    char pci[16];
    ClockTime start_time;
    ClockTime end_time;
} ClockLock;

/* 频段管理 */
void handle_get_bands(struct mg_connection *c, struct mg_http_message *hm);
void handle_lock_bands(struct mg_connection *c, struct mg_http_message *hm);
void handle_unlock_bands(struct mg_connection *c, struct mg_http_message *hm);

/* 小区管理 */
void handle_get_cells(struct mg_connection *c, struct mg_http_message *hm);
void handle_lock_cell(struct mg_connection *c, struct mg_http_message *hm);
void handle_unlock_cell(struct mg_connection *c, struct mg_http_message *hm);

/* 定时锁小区 */
int clock_lock_init(const char *db_path);
void handle_set_clock_lock(struct mg_connection *c, struct mg_http_message *hm);
void handle_get_clock_lock(struct mg_connection *c, struct mg_http_message *hm);

#ifdef __cplusplus
}
#endif

#endif /* ADVANCED_H */
