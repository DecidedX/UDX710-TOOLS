/**
 * @file advanced.c
 * @brief 高级网络功能实现 (Go: handlers/advanced.go)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "mongoose.h"
#include "advanced.h"
#include "dbus_core.h"
#include "exec_utils.h"
#include "http_utils.h"
#include "ofono.h"
#include "json_builder.h"

pthread_mutex_t g_clock_lock_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_clock_lock_initialized = 0;
static int timer_id = 0;
static ClockLock g_clock_lock = {0};

/* 频段映射结构 */
typedef struct {
    const char *name;
    const char *mode;  /* "4G" or "5G" */
    const char *type;  /* "TDD" or "FDD" */
    int value;
} BandMapping;

/* 频段映射表 */
static const BandMapping band_map[] = {
    {"TDD_34", "4G", "TDD", 2},
    {"TDD_38", "4G", "TDD", 32},
    {"TDD_39", "4G", "TDD", 64},
    {"TDD_40", "4G", "TDD", 128},
    {"TDD_41", "4G", "TDD", 256},
    {"FDD_01", "4G", "FDD", 1},
    {"FDD_03", "4G", "FDD", 4},
    {"FDD_05", "4G", "FDD", 16},
    {"FDD_08", "4G", "FDD", 128},
    {"N01", "5G", "FDD", 1},
    {"N08", "5G", "FDD", 128},
    {"N28", "5G", "FDD", 512},
    {"N41", "5G", "TDD", 16},
    {"N77", "5G", "TDD", 128},
    {"N78", "5G", "TDD", 256},
    {"N79", "5G", "TDD", 512},
    {NULL, NULL, NULL, 0}
};  

typedef struct {
    int onPeriod;
    gint64 duration;
    gint64 interval;
    ClockTime execute_time;
    void (*function)(void);
} ExecuteOnTimeArgs;

static void do_lock_cell(int lock, char *band, char *arfcn, char *pci);
static int create_clock_lock_tables(void);
static int load_clock_lock_config(void);

static void lock_mission(void);
static void unlock_mission(void);
static gboolean execute_on_time(gpointer user_data);
static long get_seconds_diff(int hour, int minute);
static gboolean set_clock_lock_timer(void *v);
static ClockTime get_clock_time_now(void);
static ClockTime transform_from_str_time(const char *time);
static void transform_to_str_time(char *str_time, size_t size, ClockTime time);
static int clock_time_compare(ClockTime start, ClockTime end);

/* 解析频段锁定状态 */
static void parse_bands_info(const char *output4G, const char *output5G, int *bands) {
    /* 初始化所有频段为未锁定 */
    memset(bands, 0, 16 * sizeof(int));

    /* 解析4G频段: +SPLBAND: 0,tdd,0,fdd,0 */
    if (output4G && strlen(output4G) > 0) {
        int tdd = 0, fdd = 0;
        char *p = strstr(output4G, "+SPLBAND:");
        if (p) {
            sscanf(p, "+SPLBAND: 0,%d,0,%d,0", &tdd, &fdd);
            /* 4G TDD */
            if (tdd & 2) bands[0] = 1;    /* TDD_34 */
            if (tdd & 32) bands[1] = 1;   /* TDD_38 */
            if (tdd & 64) bands[2] = 1;   /* TDD_39 */
            if (tdd & 128) bands[3] = 1;  /* TDD_40 */
            if (tdd & 256) bands[4] = 1;  /* TDD_41 */
            /* 4G FDD */
            if (fdd & 1) bands[5] = 1;    /* FDD_01 */
            if (fdd & 4) bands[6] = 1;    /* FDD_03 */
            if (fdd & 16) bands[7] = 1;   /* FDD_05 */
            if (fdd & 128) bands[8] = 1;  /* FDD_08 */
        }
    }

    /* 解析5G频段: +SPLBAND: fdd,0,tdd,0 */
    if (output5G && strlen(output5G) > 0) {
        int fdd = 0, tdd = 0;
        char *p = strstr(output5G, "+SPLBAND:");
        if (p) {
            sscanf(p, "+SPLBAND: %d,0,%d,0", &fdd, &tdd);
            /* 5G FDD */
            if (fdd & 1) bands[9] = 1;     /* N01 */
            if (fdd & 128) bands[10] = 1;  /* N08 */
            if (fdd & 512) bands[11] = 1;  /* N28 */
            /* 5G TDD */
            if (tdd & 16) bands[12] = 1;   /* N41 */
            if (tdd & 128) bands[13] = 1;  /* N77 */
            if (tdd & 256) bands[14] = 1;  /* N78 */
            if (tdd & 512) bands[15] = 1;  /* N79 */
        }
    }
}

/* GET /api/bands - 获取频段状态 */
void handle_get_bands(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_GET(c, hm);

    char *result4G = NULL, *result5G = NULL;
    int bands[16] = {0};

    printf("开始获取频段锁定状态...\n");

    /* 查询4G频段 */
    if (execute_at("AT+SPLBAND=0", &result4G) == 0) {
        printf("4G频段查询结果: %s\n", result4G);
    }

    /* 查询5G频段 */
    if (execute_at("AT+SPLBAND=3", &result5G) == 0) {
        printf("5G频段查询结果: %s\n", result5G);
    }

    parse_bands_info(result4G, result5G, bands);

    if (result4G) g_free(result4G);
    if (result5G) g_free(result5G);

    /* 使用JSON Builder构建响应 */
    JsonBuilder *j = json_new();
    json_obj_open(j);
    
    /* 4G TDD */
    json_arr_open(j, "4G_TDD");
    json_arr_obj_open(j); json_add_str(j, "name", "TDD_34"); json_add_str(j, "label", "B34"); json_add_bool(j, "locked", bands[0]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "TDD_38"); json_add_str(j, "label", "B38"); json_add_bool(j, "locked", bands[1]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "TDD_39"); json_add_str(j, "label", "B39"); json_add_bool(j, "locked", bands[2]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "TDD_40"); json_add_str(j, "label", "B40"); json_add_bool(j, "locked", bands[3]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "TDD_41"); json_add_str(j, "label", "B41"); json_add_bool(j, "locked", bands[4]); json_obj_close(j);
    json_arr_close(j);
    
    /* 4G FDD */
    json_arr_open(j, "4G_FDD");
    json_arr_obj_open(j); json_add_str(j, "name", "FDD_01"); json_add_str(j, "label", "B1"); json_add_bool(j, "locked", bands[5]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "FDD_03"); json_add_str(j, "label", "B3"); json_add_bool(j, "locked", bands[6]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "FDD_05"); json_add_str(j, "label", "B5"); json_add_bool(j, "locked", bands[7]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "FDD_08"); json_add_str(j, "label", "B8"); json_add_bool(j, "locked", bands[8]); json_obj_close(j);
    json_arr_close(j);
    
    /* 5G */
    json_arr_open(j, "5G");
    json_arr_obj_open(j); json_add_str(j, "name", "N01"); json_add_str(j, "label", "N1"); json_add_bool(j, "locked", bands[9]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "N08"); json_add_str(j, "label", "N8"); json_add_bool(j, "locked", bands[10]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "N28"); json_add_str(j, "label", "N28"); json_add_bool(j, "locked", bands[11]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "N41"); json_add_str(j, "label", "N41"); json_add_bool(j, "locked", bands[12]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "N77"); json_add_str(j, "label", "N77"); json_add_bool(j, "locked", bands[13]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "N78"); json_add_str(j, "label", "N78"); json_add_bool(j, "locked", bands[14]); json_obj_close(j);
    json_arr_obj_open(j); json_add_str(j, "name", "N79"); json_add_str(j, "label", "N79"); json_add_bool(j, "locked", bands[15]); json_obj_close(j);
    json_arr_close(j);
    
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
}


/* 查找频段映射 */
static const BandMapping *find_band(const char *name) {
    for (int i = 0; band_map[i].name; i++) {
        if (strcmp(band_map[i].name, name) == 0) {
            return &band_map[i];
        }
    }
    return NULL;
}

/* 从 JSON 数组中提取频段名称 - 使用mongoose JSON API */
static int parse_bands_array(const char *json_str, char bands[][32], int max_bands) {
    int count = 0;
    struct mg_str json = mg_str(json_str);
    
    /* 获取bands数组 */
    struct mg_str bands_arr = mg_json_get_tok(json, "$.bands");
    if (bands_arr.buf == NULL) return 0;
    
    /* 遍历数组元素 */
    struct mg_str key, val;
    size_t ofs = 0;
    while ((ofs = mg_json_next(bands_arr, ofs, &key, &val)) > 0 && count < max_bands) {
        /* val 包含引号，如 "TDD_34" */
        if (val.len > 2 && val.buf[0] == '"') {
            size_t len = val.len - 2;
            if (len < 32) {
                memcpy(bands[count], val.buf + 1, len);
                bands[count][len] = '\0';
                count++;
            }
        }
    }
    return count;
}

/* POST /api/lock_bands - 锁定频段 */
void handle_lock_bands(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_POST(c, hm);

    char bands[32][32] = {{0}};
    int band_count = parse_bands_array(hm->body.buf, bands, 32);

    printf("收到锁频请求，要锁定的频段数量: %d\n", band_count);

    /* 计算位掩码 */
    int tdd4G = 0, fdd4G = 0, fdd5G = 0, tdd5G = 0;
    for (int i = 0; i < band_count; i++) {
        const BandMapping *bm = find_band(bands[i]);
        if (bm) {
            printf("处理频段 %s: 模式=%s, 类型=%s, 值=%d\n", bm->name, bm->mode, bm->type, bm->value);
            if (strcmp(bm->mode, "4G") == 0 && strcmp(bm->type, "TDD") == 0) {
                tdd4G |= bm->value;
            } else if (strcmp(bm->mode, "4G") == 0 && strcmp(bm->type, "FDD") == 0) {
                fdd4G |= bm->value;
            } else if (strcmp(bm->mode, "5G") == 0 && strcmp(bm->type, "TDD") == 0) {
                tdd5G |= bm->value;
            } else if (strcmp(bm->mode, "5G") == 0 && strcmp(bm->type, "FDD") == 0) {
                fdd5G |= bm->value;
            }
        }
    }

    printf("计算结果: 4G TDD=%d, 4G FDD=%d, 5G FDD=%d, 5G TDD=%d\n", tdd4G, fdd4G, fdd5G, tdd5G);

    char *result = NULL;
    char cmd[128];

    /* 执行命令序列 */
    /* 1. 关闭设备 */
    if (execute_at("AT+SFUN=5", &result) != 0) {
        HTTP_ERROR(c, 500, "关闭设备失败");
        if (result) g_free(result);
        return;
    }
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 2. 解锁5G频段 */
    execute_at("AT+SPLBAND=2,0,0,0,0", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 3. 锁定4G频段 */
    if (tdd4G != 0 || fdd4G != 0) {
        snprintf(cmd, sizeof(cmd), "AT+SPLBAND=1,0,%d,0,%d,0", tdd4G, fdd4G);
        execute_at(cmd, &result);
        if (result) { g_free(result); result = NULL; }
        usleep(300000);
    }

    /* 4. 锁定5G频段 */
    if (fdd5G != 0 || tdd5G != 0) {
        snprintf(cmd, sizeof(cmd), "AT+SPLBAND=2,%d,0,%d,0", fdd5G, tdd5G);
        execute_at(cmd, &result);
        if (result) { g_free(result); result = NULL; }
        usleep(300000);
    }

    /* 5. 开启设备 */
    execute_at("AT+SFUN=4", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 6. 激活网络 */
    execute_at("AT+CGACT=0,1", &result);
    if (result) g_free(result);

    printf("频段锁定成功\n");
    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_bool(j, "success", 1);
    json_add_str(j, "message", "频段锁定成功");
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
}


/* POST /api/unlock_bands - 解锁所有频段 */
void handle_unlock_bands(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_POST(c, hm);

    printf("开始解锁所有频段...\n");
    char *result = NULL;

    /* 1. 关闭设备 */
    if (execute_at("AT+SFUN=5", &result) != 0) {
        HTTP_ERROR(c, 500, "关闭设备失败");
        if (result) g_free(result);
        return;
    }
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 2. 解锁4G频段 */
    execute_at("AT+SPLBAND=1,0,0,0,0,0", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 3. 解锁5G频段 */
    execute_at("AT+SPLBAND=2,0,0,0,0", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 4. 开启设备 */
    execute_at("AT+SFUN=4", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 5. 激活网络 */
    execute_at("AT+CGACT=0,1", &result);
    if (result) g_free(result);

    printf("频段解锁成功\n");
    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_bool(j, "success", 1);
    json_add_str(j, "message", "频段解锁成功");
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
}

/* 解析小区数据 (复用 handlers.c 中的函数) */
extern int parse_cell_to_vec(const char *input, char data[64][16][32]);

/**
 * 根据 NR ARFCN 推算 5G 频段
 * 参考 3GPP TS 38.104
 * @param arfcn NR 绝对频点号
 * @return 频段号字符串（不含N前缀），如 "41", "78"，未知返回空字符串
 */
static const char* arfcn_to_nr_band(int arfcn) {
    if (arfcn >= 422000 && arfcn <= 434000) return "1";   /* N1 (2100 MHz FDD) */
    if (arfcn >= 361000 && arfcn <= 376000) return "3";   /* N3 (1800 MHz FDD) */
    if (arfcn >= 185000 && arfcn <= 192000) return "8";   /* N8 (900 MHz FDD) */
    if (arfcn >= 151600 && arfcn <= 160600) return "28";  /* N28 (700 MHz FDD) */
    if (arfcn >= 499200 && arfcn <= 537999) return "41";  /* N41 (2600 MHz TDD) */
    if (arfcn >= 620000 && arfcn <= 680000) return "78";  /* N77/N78 (3700 MHz TDD) */
    if (arfcn >= 693334 && arfcn <= 733333) return "79";  /* N79 (4700 MHz TDD) */
    return "";
}

/**
 * 根据 LTE EARFCN 推算 4G 频段
 * 参考 3GPP TS 36.101
 * @param earfcn LTE 绝对频点号
 * @return 频段号字符串（不含B前缀），如 "3", "41"，未知返回空字符串
 */
static const char* earfcn_to_lte_band(int earfcn) {
    if (earfcn >= 0 && earfcn <= 599) return "1";         /* B1 (2100 MHz FDD) */
    if (earfcn >= 1200 && earfcn <= 1949) return "3";     /* B3 (1800 MHz FDD) */
    if (earfcn >= 2400 && earfcn <= 2649) return "5";     /* B5 (850 MHz FDD) */
    if (earfcn >= 2750 && earfcn <= 3449) return "7";     /* B7 (2600 MHz FDD) */
    if (earfcn >= 3450 && earfcn <= 3799) return "8";     /* B8 (900 MHz FDD) */
    if (earfcn >= 6150 && earfcn <= 6449) return "20";    /* B20 (800 MHz FDD) */
    if (earfcn >= 9210 && earfcn <= 9659) return "28";    /* B28 (700 MHz FDD) */
    if (earfcn >= 37750 && earfcn <= 38249) return "38";  /* B38 (2600 MHz TDD) */
    if (earfcn >= 38250 && earfcn <= 38649) return "39";  /* B39 (1900 MHz TDD) */
    if (earfcn >= 38650 && earfcn <= 39649) return "40";  /* B40 (2300 MHz TDD) */
    if (earfcn >= 39650 && earfcn <= 41589) return "41";  /* B41 (2500 MHz TDD) */
    return "";
}

/**
 * 判断当前网络是否为 5G
 * 通过 D-Bus 查询 oFono NetworkMonitor 获取网络类型
 * @return 1=5G, 0=4G/其他
 */
static int is_5g_network(void) {
    char tech[32] = {0};
    
    /* 使用 C 原生 D-Bus 调用获取网络类型 */
    if (ofono_get_serving_cell_tech(tech, sizeof(tech)) != 0) {
        printf("D-Bus 查询网络类型失败，默认使用 4G\n");
        return 0;
    }

    /* 判断网络类型 - 检查是否为 "nr" */
    if (strcmp(tech, "nr") == 0) {
        return 1; /* 5G */
    }
    
    return 0; /* 4G 或其他 */
}

/* 辅助函数：添加小区对象到JSON Builder */
static void add_cell_to_json(JsonBuilder *j, const char *rat, const char *band_prefix, 
                              const char *band, int arfcn, int pci,
                              double rsrp, double rsrq, double sinr, int is_serving) {
    char band_str[32];
    snprintf(band_str, sizeof(band_str), "%s%s", band_prefix, band);
    
    json_arr_obj_open(j);
    json_add_str(j, "rat", rat);
    json_add_str(j, "band", band_str);
    json_add_int(j, "arfcn", arfcn);
    json_add_int(j, "pci", pci);
    json_add_double(j, "rsrp", rsrp);
    json_add_double(j, "rsrq", rsrq);
    json_add_double(j, "sinr", sinr);
    json_add_bool(j, "isServing", is_serving);
    json_obj_close(j);
}

/* GET /api/cells - 获取小区信息 */
void handle_get_cells(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_GET(c, hm);

    printf("开始获取小区信息...\n");
    char *result = NULL;
    
    /* 通过 D-Bus 判断网络类型 (与 Go 版本一致) */
    int is_5g = is_5g_network();
    printf("检测到%s网络\n", is_5g ? "5G" : "4G");

    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 0);
    json_add_str(j, "Error", "");
    json_arr_open(j, "Data");
    
    int cell_count = 0;

    if (is_5g) {
        /* 5G 主小区 */
        if (execute_at("AT+SPENGMD=0,14,1", &result) == 0 && result) {
            char data[64][16][32] = {{{0}}};
            int rows = parse_cell_to_vec(result, data);
            if (rows > 15) {
                add_cell_to_json(j, "5G", "N", data[0][0],
                    atoi(data[1][0]), atoi(data[2][0]),
                    atof(data[3][0]) / 100.0, atof(data[4][0]) / 100.0,
                    atof(data[15][0]) / 100.0, 1);
                cell_count++;
            }
            g_free(result);
            result = NULL;
        }

        /* 5G 邻小区 */
        if (execute_at("AT+SPENGMD=0,14,2", &result) == 0 && result) {
            char data[64][16][32] = {{{0}}};
            int rows = parse_cell_to_vec(result, data);
            if (rows > 5) {
                int col_count = 0;
                for (int i = 0; i < 16 && data[0][i][0]; i++) col_count++;
                for (int i = 0; i < col_count; i++) {
                    int arfcn = atoi(data[1][i]);
                    int pci = atoi(data[2][i]);
                    if (arfcn == 0 || pci == 0) continue;
                    
                    /* 频段处理：如果为空或"0"，通过 ARFCN 推算 */
                    const char *band_str = data[0][i];
                    if (strlen(band_str) == 0 || strcmp(band_str, "0") == 0) {
                        band_str = arfcn_to_nr_band(arfcn);
                    }
                    
                    add_cell_to_json(j, "5G", "N", band_str,
                        arfcn, pci,
                        atof(data[3][i]) / 100.0, atof(data[4][i]) / 100.0,
                        atof(data[5][i]) / 100.0, 0);
                    cell_count++;
                }
            }
            g_free(result);
        }
    } else {
        /* 4G 主小区 */
        if (execute_at("AT+SPENGMD=0,6,0", &result) == 0 && result) {
            char data[64][16][32] = {{{0}}};
            int rows = parse_cell_to_vec(result, data);
            if (rows > 33) {
                add_cell_to_json(j, "4G", "B", data[0][0],
                    atoi(data[1][0]), atoi(data[2][0]),
                    atof(data[3][0]) / 100.0, atof(data[4][0]) / 100.0,
                    atof(data[33][0]) / 100.0, 1);
                cell_count++;
            }
            g_free(result);
            result = NULL;
        }

        /* 4G 邻小区 */
        if (execute_at("AT+SPENGMD=0,6,6", &result) == 0 && result) {
            char data[64][16][32] = {{{0}}};
            int rows = parse_cell_to_vec(result, data);
            for (int i = 0; i < rows; i++) {
                int arfcn = atoi(data[i][0]);
                int pci = atoi(data[i][1]);
                if (arfcn == 0 || pci == 0) continue;
                
                /* 频段处理：如果为空或"0"，通过 EARFCN 推算 */
                const char *band = data[i][12];
                if (strlen(band) == 0 || strcmp(band, "0") == 0) {
                    band = earfcn_to_lte_band(arfcn);
                    if (strlen(band) == 0) band = "0";  /* 未知频段默认显示0 */
                }
                
                add_cell_to_json(j, "4G", "B", band,
                    arfcn, pci,
                    atof(data[i][2]) / 100.0, atof(data[i][3]) / 100.0,
                    atof(data[i][6]) / 100.0, 0);
                cell_count++;
            }
            g_free(result);
        }
    }

    json_arr_close(j);
    json_obj_close(j);
    printf("小区信息获取完成，共 %d 个小区\n", cell_count);

    HTTP_OK_FREE(c, json_finish(j));
}


/* POST /api/lock_cell - 锁定小区 */
void handle_lock_cell(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_POST(c, hm);

    char technology[32] = {0}, arfcn[32] = {0}, pci[32] = {0};

    /* 使用mongoose JSON API解析 */
    char *tech_str = mg_json_get_str(hm->body, "$.technology");
    char *arfcn_str = mg_json_get_str(hm->body, "$.arfcn");
    char *pci_str = mg_json_get_str(hm->body, "$.pci");
    
    if (tech_str) { strncpy(technology, tech_str, sizeof(technology) - 1); free(tech_str); }
    if (arfcn_str) { strncpy(arfcn, arfcn_str, sizeof(arfcn) - 1); free(arfcn_str); }
    if (pci_str) { strncpy(pci, pci_str, sizeof(pci) - 1); free(pci_str); }

    printf("收到锁小区请求: Technology=%s, ARFCN=%s, PCI=%s\n", technology, arfcn, pci);

    /* 确定 band 参数 */
    const char *band = "12"; /* 4G */
    if (strstr(technology, "5G") || strstr(technology, "NR") ||
        strstr(technology, "5g") || strstr(technology, "nr")) {
        band = "16"; /* 5G */
    }

    do_lock_cell(1, band, arfcn, pci);

    printf("小区锁定成功\n");
    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 0);
    json_add_str(j, "Error", "");
    json_key_obj_open(j, "Data");
    json_add_bool(j, "success", 1);
    json_add_str(j, "message", "小区锁定成功");
    json_obj_close(j);
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
}

/* POST /api/unlock_cell - 解锁小区 */
void handle_unlock_cell(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_POST(c, hm);

    printf("开始解锁小区...\n");
    
    do_lock_cell(0, NULL, NULL, NULL);

    printf("小区解锁成功\n");
    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 0);
    json_add_str(j, "Error", "");
    json_key_obj_open(j, "Data");
    json_add_bool(j, "success", 1);
    json_add_str(j, "message", "小区解锁成功");
    json_obj_close(j);
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
}

/* 锁定或解锁小区 */
static void do_lock_cell(int lock, char *band, char *arfcn, char *pci) {
    char *result = NULL;
    char cmd[128];

    /* 1. 关闭射频 */
    execute_at("AT+SFUN=5", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 2. 解锁4G */
    execute_at("AT+SPFORCEFRQ=12,0", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 3. 解锁5G */
    execute_at("AT+SPFORCEFRQ=16,0", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    if (lock) {
        /* 4. 锁定小区 */
        snprintf(cmd, sizeof(cmd), "AT+SPFORCEFRQ=%s,2,%s,%s", band, arfcn, pci);
        execute_at(cmd, &result);
        if (result) { g_free(result); result = NULL; }
        usleep(300000);
    }

    /* 5. 打开射频 */
    execute_at("AT+SFUN=4", &result);
    if (result) { g_free(result); result = NULL; }
    usleep(300000);

    /* 6. 激活网络 */
    execute_at("AT+CGACT=0,1", &result);
    if (result) g_free(result);
}

/* 创建ClockLock数据表 */
static int create_clock_lock_tables(void) {
    const char *sql = "CREATE TABLE IF NOT EXISTS clock_lock_config ("
                      "id INTEGER PRIMARY KEY DEFAULT 1 CHECK (id = 1),"
                      "enabled INTEGER DEFAULT 0,"
                      "rat varchar(16),"
                      "band varchar(16),"
                      "arfcn varchar(16),"
                      "pci varchar(16),"
                      "start_time varchar(8),"
                      "end_time varchar(8)"
                      ");";

                     
    pthread_mutex_lock(&g_clock_lock_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_clock_lock_mutex);
    if (ret != 0) {
        printf("[ClockLock] 创建配置表失败 (ret=%d)\n", ret);
        return ret;
    }

    printf("[ClockLock] 数据库表创建/验证成功\n");
    return 0;
}

/* 加载ClockLock配置 */
static int load_clock_lock_config(void) {
    char output[4096];
    const char *sql = "SELECT enabled, rat, band, arfcn, pci, start_time, end_time "
                      "FROM clock_lock_config WHERE id = 1;";

    pthread_mutex_lock(&g_clock_lock_mutex);
    int ret = db_query_rows(sql, "|", output, sizeof(output));
    pthread_mutex_unlock(&g_clock_lock_mutex);

    if (ret != 0 || strlen(output) == 0) {
        memset(&g_clock_lock, 0, sizeof(g_clock_lock));
        return 0;
    }

    char *fields[7] = {NULL};
    int field_count = 0;
    char *p = output;
    char *start = p;

    while (*p && field_count < 7) {
        if (*p == '|') {
        *p = '\0';
        fields[field_count++] = start;
        start = p + 1;
        }
        p++;
    }
    if (field_count < 7 && start) {
        fields[field_count++] = start;
    }

    if (field_count >= 7) {
        g_clock_lock.enabled = atoi(fields[0]);
        strncpy(g_clock_lock.rat, fields[1],
                sizeof(g_clock_lock.rat) - 1);
        strncpy(g_clock_lock.band, fields[2],
                sizeof(g_clock_lock.band) - 1);
        strncpy(g_clock_lock.arfcn, fields[3],
                sizeof(g_clock_lock.arfcn) - 1);
        strncpy(g_clock_lock.pci, fields[4],
                sizeof(g_clock_lock.pci) - 1);
        g_clock_lock.start_time = transform_from_str_time(fields[5]);
        g_clock_lock.end_time = transform_from_str_time(fields[6]);
    }

    printf("[ClockLock] 配置加载完成: 启用=%d, 开始时间=%s, 结束时间=%s,"
            "目标小区={rat: %s, band: %s, arfcn: %s, pci: %s}\n",
            g_clock_lock.enabled, fields[5], fields[6],
            g_clock_lock.rat, g_clock_lock.band, g_clock_lock.arfcn, g_clock_lock.pci);
    return 0;
}

int clock_lock_init(const char *db_path) {
    if (g_clock_lock_initialized) {
        return 0;
    }

    printf("[ClockLock] 初始化模块\n");

    if (db_path && strlen(db_path) > 0) {
        db_init(db_path);
    }

    if (create_clock_lock_tables() != 0) {
        printf("[ClockLock] 创建数据库表失败\n");
        return -1;
    }

    load_clock_lock_config();

    if (g_clock_lock.enabled) {
        g_timeout_add_seconds(15, set_clock_lock_timer, NULL);
    }
    return 0;
}

static gint64 get_seconds_diff(int hour, int minute) {
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *target = g_date_time_new_local(
        g_date_time_get_year(now),
        g_date_time_get_month(now),
        g_date_time_get_day_of_month(now),
        hour, minute, 0.0
    );
    if (g_date_time_compare(target, now) <= 0) {
        GDateTime *next_day = g_date_time_add_days(target, 1);
        g_date_time_unref(target);
        target = next_day;
    }
    gint64 diff_sec = g_date_time_difference(target, now) / (1000 * 1000);
    
    g_date_time_unref(now);
    g_date_time_unref(target);
    
    return diff_sec;
}

static void lock_mission(void) {
    const char *band = "12"; /* 4G */
    if (strstr(g_clock_lock.rat, "5G") || strstr(g_clock_lock.rat, "NR") ||
        strstr(g_clock_lock.rat, "5g") || strstr(g_clock_lock.rat, "nr")) {
        band = "16"; /* 5G */
    }
    printf("[ClockLock] 开始执行锁定\n");
    do_lock_cell(1, band, g_clock_lock.arfcn, g_clock_lock.pci);
    gint64 sec_diff = get_seconds_diff(g_clock_lock.end_time.hour, g_clock_lock.end_time.minute);
    ExecuteOnTimeArgs *args = g_new(ExecuteOnTimeArgs, 1);
    args->onPeriod = 0;
    args->duration = 30;
    args->interval = 60;
    args->execute_time = g_clock_lock.end_time;
    args->function = unlock_mission;
    timer_id = g_timeout_add_seconds(MAX(sec_diff - 15 * 60, 1), execute_on_time, args);
    printf("[ClockLock] 已执行锁定，设定定时解锁\n");
}

static void unlock_mission(void) {
    printf("[ClockLock] 开始执行解锁\n");
    if (timer_id != 0) {
        do_lock_cell(0, NULL, NULL, NULL);
        printf("[ClockLock] 已执行解锁\n");
    }
    gint64 sec_diff = get_seconds_diff(g_clock_lock.start_time.hour, g_clock_lock.start_time.minute);
    ExecuteOnTimeArgs *args = g_new(ExecuteOnTimeArgs, 1);
    args->onPeriod = 0;
    args->duration = 30;
    args->interval = 60;
    args->execute_time = g_clock_lock.start_time;
    args->function = lock_mission;
    timer_id = g_timeout_add_seconds(MAX(sec_diff - 15 * 60, 1), execute_on_time, args);
    printf("[ClockLock] 已设定定时锁定\n");
}

static gboolean execute_on_time(gpointer user_data) {
    ExecuteOnTimeArgs *args = (ExecuteOnTimeArgs*)user_data;
    static gint64 count = 0;
    if (args->onPeriod) {
        ClockTime clock_now = get_clock_time_now();
        if ((clock_now.hour == args->execute_time.hour && clock_now.minute == args->execute_time.minute)
            || count == args->duration) {
            args->function();
            count = 0;
            return FALSE;
        } else {
            count++;
            return TRUE;
        }
    } else {
        args->onPeriod = 1;
        timer_id = g_timeout_add_seconds(args->interval, execute_on_time, args);
        return FALSE;
    }
}

/* 设置定时任务 */
static gboolean set_clock_lock_timer(void *v) {
    printf("[ClockLock] 开始设定定时计划\n");
    ClockTime clock_now = get_clock_time_now();
    if (clock_time_compare(g_clock_lock.start_time, g_clock_lock.end_time)) {
        if (clock_time_compare(clock_now , g_clock_lock.start_time)) {
            lock_mission();
        } else {
            unlock_mission();
        }
    } else {
        if (clock_time_compare(clock_now , g_clock_lock.start_time) && clock_time_compare(g_clock_lock.end_time, clock_now)) {
            lock_mission();
        } else {
            unlock_mission();
        }
    }
    return FALSE;
}

/* GET /api/get/clock_lock - 获取定时锁定数据 */
void handle_get_clock_lock(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_GET(c, hm);
    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 0);
    json_add_str(j, "Error", "");
    json_key_obj_open(j, "Data");
    json_add_bool(j, "enabled", g_clock_lock.enabled);
    char start_time[6] = {0}, end_time[6] = {0};
    transform_to_str_time(start_time, 6, g_clock_lock.start_time);
    transform_to_str_time(end_time, 6, g_clock_lock.end_time);
    json_add_str(j, "startTime", start_time);
    json_add_str(j, "endTime", end_time);
    json_key_obj_open(j, "targetCell");
    json_add_str(j, "rat", g_clock_lock.rat);
    json_add_str(j, "band", g_clock_lock.band);
    json_add_str(j, "arfcn", g_clock_lock.arfcn);
    json_add_str(j, "pci", g_clock_lock.pci);
    json_obj_close(j);
    json_obj_close(j);
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
}

/* POST /api/set/clock_lock - 设定定时锁定数据 */
void handle_set_clock_lock(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_POST(c, hm);

    if (timer_id != 0) {
        g_source_remove(timer_id);
        timer_id = 0;
        do_lock_cell(0, NULL, NULL, NULL);
    }

    int enabled = 0;
    mg_json_get_bool(hm->body, "$.enabled", &enabled);

    if (enabled) {
        char sql[512];
        char rat[16] = {0}, band[16] = {0}, arfcn[16] = {0}, pci[16] = {0}, start_time[8] = {0}, end_time[8] = {0};
        char *rat_str = mg_json_get_str(hm->body, "$.targetCell.rat");
        char *band_str = mg_json_get_str(hm->body, "$.targetCell.band");
        char *arfcn_str = mg_json_get_str(hm->body, "$.targetCell.arfcn");
        char *pci_str = mg_json_get_str(hm->body, "$.targetCell.pci");
        char *start_time_str = mg_json_get_str(hm->body, "$.startTime");
        char *end_time_str = mg_json_get_str(hm->body, "$.endTime");

        if (rat_str) { strncpy(rat, rat_str, sizeof(rat) - 1); free(rat_str); }
        if (band_str) { strncpy(band, band_str, sizeof(band) - 1); free(band_str); }
        if (arfcn_str) { strncpy(arfcn, arfcn_str, sizeof(arfcn) - 1); free(arfcn_str); }
        if (pci_str) { strncpy(pci, pci_str, sizeof(pci) - 1); free(pci_str); }
        if (start_time_str) { strncpy(start_time, start_time_str, sizeof(start_time) - 1); free(start_time_str); }
        if (end_time_str) { strncpy(end_time, end_time_str, sizeof(end_time) - 1); free(end_time_str); }

        snprintf(sql, sizeof(sql),
                "INSERT OR REPLACE INTO clock_lock_config "
                "(id, enabled, rat, band, arfcn, pci, start_time, end_time) "
                "VALUES (1, 1, '%s', '%s', '%s', '%s', '%s', '%s');",
                rat, band, arfcn, pci, start_time, end_time);

        pthread_mutex_lock(&g_clock_lock_mutex);
        int ret = db_execute(sql);
        pthread_mutex_unlock(&g_clock_lock_mutex);

        if (ret == 0) {
            load_clock_lock_config();
            set_clock_lock_timer(NULL);
            printf("[ClockLock] 定时锁定已保存\n");
        }
        JsonBuilder *j = json_new();
        json_obj_open(j);
        json_add_int(j, "Code", 0);
        json_add_str(j, "Error", "");
        json_key_obj_open(j, "Data");
        json_add_bool(j, "success", ret == 0);
        json_add_str(j, "message", "定时锁定已保存");
        json_obj_close(j);
        json_obj_close(j);
        HTTP_OK_FREE(c, json_finish(j));
    } else {
        g_clock_lock.enabled = false;
        const char *sql = "DELETE FROM clock_lock_config WHERE id=1;";
        
        pthread_mutex_lock(&g_clock_lock_mutex);
        int ret = db_execute(sql);
        pthread_mutex_unlock(&g_clock_lock_mutex);

        if (ret == 0) {
            load_clock_lock_config();
            do_lock_cell(0, NULL, NULL, NULL);
            printf("[ClockLock] 定时锁定已停用\n");
        }
        JsonBuilder *j = json_new();
        json_obj_open(j);
        json_add_bool(j, "success", ret == 0);
        json_add_str(j, "message", "定时锁定已停用");
        json_obj_close(j);
        HTTP_OK_FREE(c, json_finish(j));
    }
}

static ClockTime get_clock_time_now() {
    ClockTime clock_time = {0, 0};
    GDateTime *now = g_date_time_new_now_local();
    clock_time.hour= g_date_time_get_hour(now);
    clock_time.minute = g_date_time_get_minute(now);
    return clock_time;
}

static ClockTime transform_from_str_time(const char *time) {
    ClockTime clock_time = {0};
    if (time) {
        sscanf(time, "%d:%d", &clock_time.hour, &clock_time.minute);
    }
    return clock_time;
}

static void transform_to_str_time(char *str_time, size_t size, ClockTime time) {
    snprintf(str_time, size, "%02d:%02d", time.hour, time.minute);
}

static int clock_time_compare(ClockTime a, ClockTime b) {
    if ((a.hour - b.hour) * 60 + a.minute - b.minute > 0) {
        return 1;
    }
    return 0;
}

