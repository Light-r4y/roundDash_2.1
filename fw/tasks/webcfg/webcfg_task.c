#include "webcfg_task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "conf.h"
#include "settings.h"
#include "auth.h"
#include "layout.h"
#include "board.h"
#include "imu_task.h"
#include "esp_heap_caps.h"
#include "can_map.h"
#include "can_task.h"
#include "cJSON.h"
#include "ota_page.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "freertos/semphr.h"

static const char *TAG = "WEBCFG";

// Число из макроса - в строку (для текстов ошибок).
#define RG_STR_(x) #x
#define RG_STR(x) RG_STR_(x)

static atomic_bool s_start_requested = false;
static atomic_bool s_running = false;

// ---------------------------------------------------------------------------
// Пароль: Basic Auth, логин игнорируется (как в tools/webtest/mock_server.py).
// ---------------------------------------------------------------------------
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, char *out, size_t out_max)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64_val(*in);
        if (v < 0) return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= out_max) return 0;
            out[n++] = (char)(acc >> bits);
        }
    }
    out[n] = '\0';
    return n;
}

// true - можно продолжать; false - 401 уже отправлен.
static bool check_auth(httpd_req_t *req)
{
    if (!roundGauge_auth_is_enabled()) {
        return true;
    }
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        char dec[128];
        if (b64_decode(hdr + 6, dec, sizeof(dec)) > 0) {
            const char *colon = strchr(dec, ':');
            if (colon && roundGauge_auth_check(colon + 1)) {
                return true;
            }
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"" RG_AUTH_REALM "\"");
    httpd_resp_send(req, NULL, 0);
    return false;
}

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, roundGauge_auth_is_enabled() ? "{\"enabled\":true}" : "{\"enabled\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t auth_verify_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// OTA и /api/status
// ---------------------------------------------------------------------------
#define OTA_CHUNK 4096
#define WWW_BLOCK 4096
#define RECV_IDLE_RETRIES 4

// Итог последнего обновления www - страница читает его из /api/status.
static struct {
    bool valid, ok;
    char id[16];
    char error[48];
    uint32_t written, skipped, total_ms;
} s_www_update;
static SemaphoreHandle_t s_upd_mutex;

static esp_err_t status_handler(httpd_req_t *req)
{
    roundGauge_ap_info_t ap;
    roundGauge_webcfg_get_ap_info(&ap);
    char json[512];
    int n = snprintf(json, sizeof(json),
                     "{\"fw_version\":\"%s\",\"uptime_s\":%lld,\"reset_reason\":%d,\"ap_clients\":%u,"
                     "\"heap\":{\"int_free\":%u,\"int_min\":%u,\"int_block\":%u,\"psram_free\":%u,\"psram_min\":%u}",
                     esp_app_get_description()->version, (long long)(esp_timer_get_time() / 1000000),
                     (int)esp_reset_reason(), (unsigned)ap.clients,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    xSemaphoreTake(s_upd_mutex, portMAX_DELAY);
    if (s_www_update.valid) {
        n += snprintf(json + n, sizeof(json) - n, ",\"www_update\":{\"id\":\"%s\",\"done\":true,\"ok\":%s",
                      s_www_update.id, s_www_update.ok ? "true" : "false");
        if (!s_www_update.ok) {
            n += snprintf(json + n, sizeof(json) - n, ",\"error\":\"%s\"", s_www_update.error);
        }
        n += snprintf(json + n, sizeof(json) - n, ",\"blocks_written\":%u,\"blocks_skipped\":%u,\"total_ms\":%u}",
                      (unsigned)s_www_update.written, (unsigned)s_www_update.skipped,
                      (unsigned)s_www_update.total_ms);
    }
    xSemaphoreGive(s_upd_mutex);
    snprintf(json + n, sizeof(json) - n, "}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_text_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

// Принять ровно len байт тела. Таймауты сокета повторяем: Wi-Fi AP бывает медленным.
static bool recv_exact(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    int idle = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++idle > RECV_IDLE_RETRIES) return false;
            continue;
        }
        if (r <= 0) return false;
        idle = 0;
        got += r;
    }
    return true;
}

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    size_t total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || total == 0 || total > part->size) {
        return send_text_err(req, "400 Bad Request", "Bad image size");
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        return send_text_err(req, "500 Internal Server Error", "No memory");
    }
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, total, &h);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_text_err(req, "500 Internal Server Error", "OTA begin failed");
    }

    const char *fail = NULL;
    for (size_t done = 0; done < total && !fail;) {
        size_t n = total - done < OTA_CHUNK ? total - done : OTA_CHUNK;
        if (!recv_exact(req, buf, n)) {
            fail = "Receive failed";
        } else if (done == 0 && (uint8_t)buf[0] != 0xE9) {
            fail = "Not a firmware image";
        } else if (esp_ota_write(h, buf, n) != ESP_OK) {
            fail = "Write failed";
        }
        done += n;
    }
    free(buf);

    if (fail) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "OTA failed: %s", fail);
        return send_text_err(req, "400 Bad Request", fail);
    }
    if (esp_ota_end(h) != ESP_OK) {
        return send_text_err(req, "400 Bad Request", "Image check failed");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Set boot partition failed");
    }

    ESP_LOGW(TAG, "OTA ok (%u bytes) -> %s, rebooting", (unsigned)total, part->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static void www_mount(void)
{
    esp_vfs_spiffs_conf_t fs = {
        .base_path = RG_WWW_BASE_PATH,
        .partition_label = RG_WWW_PARTITION,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount %s failed: %s", RG_WWW_PARTITION, esp_err_to_name(err));
    }
}

// Образ раздела www целиком. Блоки, совпадающие с уже записанными, не стираются
// и не пишутся - быстрее и бережёт флеш. После записи раздел монтируется снова,
// перезагрузка не нужна.
static esp_err_t www_update_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char id[16] = "";
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
    }

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                                           RG_WWW_PARTITION);
    size_t total = req->content_len;
    if (!part || total == 0 || total > part->size) {
        return send_text_err(req, "400 Bad Request", "Bad image size");
    }

    char *buf = malloc(WWW_BLOCK);
    char *cur = malloc(WWW_BLOCK);
    if (!buf || !cur) {
        free(buf);
        free(cur);
        return send_text_err(req, "500 Internal Server Error", "No memory");
    }

    int64_t t0 = esp_timer_get_time();
    esp_vfs_spiffs_unregister(RG_WWW_PARTITION);

    const char *fail = NULL;
    uint32_t written = 0, skipped = 0;
    for (size_t off = 0; off < total && !fail; off += WWW_BLOCK) {
        size_t n = total - off < WWW_BLOCK ? total - off : WWW_BLOCK;
        if (!recv_exact(req, buf, n)) {
            fail = "Receive failed";
            break;
        }
        // Хвост неполного блока добиваем 0xFF - так выглядит стёртый флеш.
        if (n < WWW_BLOCK) memset(buf + n, 0xFF, WWW_BLOCK - n);
        if (esp_partition_read(part, off, cur, WWW_BLOCK) == ESP_OK && memcmp(cur, buf, WWW_BLOCK) == 0) {
            skipped++;
        } else if (esp_partition_erase_range(part, off, WWW_BLOCK) != ESP_OK ||
                   esp_partition_write(part, off, buf, WWW_BLOCK) != ESP_OK) {
            fail = "Write failed";
        } else {
            written++;
        }
    }
    free(buf);
    free(cur);
    www_mount();

    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    xSemaphoreTake(s_upd_mutex, portMAX_DELAY);
    s_www_update.valid = id[0] != '\0';
    s_www_update.ok = fail == NULL;
    strlcpy(s_www_update.id, id, sizeof(s_www_update.id));
    strlcpy(s_www_update.error, fail ? fail : "", sizeof(s_www_update.error));
    s_www_update.written = written;
    s_www_update.skipped = skipped;
    s_www_update.total_ms = ms;
    xSemaphoreGive(s_upd_mutex);

    if (fail) {
        ESP_LOGE(TAG, "www update failed: %s", fail);
        return send_text_err(req, "500 Internal Server Error", fail);
    }
    ESP_LOGW(TAG, "www update ok: %u written, %u skipped, %u ms", (unsigned)written, (unsigned)skipped, (unsigned)ms);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"status\":\"success\",\"blocks_written\":%u,\"blocks_skipped\":%u,\"total_ms\":%u}",
             (unsigned)written, (unsigned)skipped, (unsigned)ms);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    static const struct { const char *ext, *type; } types[] = {
        {".html", "text/html"}, {".js", "application/javascript"}, {".css", "text/css"},
        {".woff2", "font/woff2"}, {".woff", "font/woff"}, {".json", "application/json"},
        {".png", "image/png"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"},
    };
    const char *type = "application/octet-stream";
    size_t plen = strlen(path);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t el = strlen(types[i].ext);
        if (plen > el && strcmp(path + plen - el, types[i].ext) == 0) {
            type = types[i].type;
            break;
        }
    }
    httpd_resp_set_type(req, type);

    char *buf = malloc(1024);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    esp_err_t ret = ESP_OK;
    size_t r;
    while ((r = fread(buf, 1, 1024, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    free(buf);
    fclose(f);
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t www_get_handler(httpd_req_t *req)
{
    char path[sizeof(RG_WWW_BASE_PATH) + 256];
    const char *uri = req->uri;
    size_t n = strcspn(uri, "?#");
    if (n == 1 && uri[0] == '/') {
        uri = "/index.html";
        n = strlen(uri);
    }
    if (n >= sizeof(path) - sizeof(RG_WWW_BASE_PATH) || strstr(uri, "..")) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    snprintf(path, sizeof(path), RG_WWW_BASE_PATH "%.*s", (int)n, uri);
    return send_file(req, path);
}

// ---------------------------------------------------------------------------
// Раскладка экранов и картинки (раздел media)
// ---------------------------------------------------------------------------

// Имя файла в media: [A-Za-z0-9._-], до 31 символа, не начинается с точки.
// Длина ограничена SPIFFS_OBJ_NAME_LEN (48) за вычетом "/media/".
static bool safe_name(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 31 || n[0] == '.') return false;
    for (; *n; n++) {
        bool ok = (*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') || (*n >= '0' && *n <= '9') ||
                  *n == '.' || *n == '_' || *n == '-';
        if (!ok) return false;
    }
    return true;
}

// Имя файла из параметра ?name=; false - нет или небезопасное (404/400 уже отправлен).
static bool query_name(httpd_req_t *req, char *name, size_t size)
{
    char query[96];
    name[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, size) != ESP_OK || !safe_name(name)) {
        send_text_err(req, "400 Bad Request", "Bad file name");
        return false;
    }
    return true;
}

static esp_err_t layout_get_handler(httpd_req_t *req)
{
    char *json = roundGauge_layout_json_dup();
    if (json == NULL) {
        return send_text_err(req, "500 Internal Server Error", "No memory");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

static esp_err_t layout_put_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    size_t len = req->content_len;
    if (len == 0 || len > RG_LAYOUT_JSON_MAX) {
        return send_text_err(req, "400 Bad Request", "Bad layout size");
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return send_text_err(req, "500 Internal Server Error", "No memory");
    }
    esp_err_t ret;
    if (!recv_exact(req, buf, len)) {
        ret = send_text_err(req, "400 Bad Request", "Receive failed");
    } else {
        buf[len] = '\0';
        esp_err_t err = roundGauge_layout_apply_json(buf);
        if (err == ESP_OK) {
            httpd_resp_set_type(req, "application/json");
            ret = httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
        } else if (err == ESP_ERR_INVALID_ARG) {
            ret = send_text_err(req, "400 Bad Request", "Invalid layout or too many distinct background images");
        } else {
            ret = send_text_err(req, "500 Internal Server Error", "Save failed");
        }
    }
    free(buf);
    return ret;
}

static esp_err_t layout_reset_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    if (roundGauge_layout_reset() != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Reset failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t media_get_handler(httpd_req_t *req)
{
    // "/media/<имя>" совпадает с путём файла в VFS.
    char name[40];
    size_t n = strcspn(req->uri, "?#");
    if (n <= sizeof(RG_MEDIA_BASE_PATH) || n - sizeof(RG_MEDIA_BASE_PATH) >= sizeof(name)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    snprintf(name, sizeof(name), "%.*s", (int)(n - sizeof(RG_MEDIA_BASE_PATH)), req->uri + sizeof(RG_MEDIA_BASE_PATH));
    if (!safe_name(name)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    char path[sizeof(RG_MEDIA_BASE_PATH) + 40];
    snprintf(path, sizeof(path), RG_MEDIA_BASE_PATH "/%s", name);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return send_file(req, path);
}

static esp_err_t media_list_handler(httpd_req_t *req)
{
    size_t total = 0, used = 0;
    esp_spiffs_info(RG_MEDIA_PARTITION, &total, &used);

    httpd_resp_set_type(req, "application/json");
    char line[128];
    snprintf(line, sizeof(line), "{\"total\":%u,\"used\":%u,\"max_file\":%d,\"files\":[", (unsigned)total,
             (unsigned)used, RG_MEDIA_MAX_FILE);
    httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);

    DIR *d = opendir(RG_MEDIA_BASE_PATH);
    bool first = true;
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        if (!safe_name(e->d_name)) continue;
        char name[32]; // safe_name гарантирует до 31 символа - компилятору это знать нужно
        strlcpy(name, e->d_name, sizeof(name));
        char path[sizeof(RG_MEDIA_BASE_PATH) + 40];
        struct stat st;
        snprintf(path, sizeof(path), RG_MEDIA_BASE_PATH "/%s", name);
        if (stat(path, &st) != 0) continue;
        snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"size\":%ld}", first ? "" : ",", name, (long)st.st_size);
        first = false;
        httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);
    }
    if (d != NULL) closedir(d);
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Загрузка картинки: тело запроса - готовый LVGL .bin. Пишем во временный файл и
// только потом подменяем настоящий - оборванная загрузка не портит старый.
static esp_err_t media_upload_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char name[40];
    if (!query_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    size_t len = req->content_len;
    if (len == 0 || len > RG_MEDIA_MAX_FILE) {
        return send_text_err(req, "400 Bad Request", "Bad file size");
    }

    char path[sizeof(RG_MEDIA_BASE_PATH) + 40];
    snprintf(path, sizeof(path), RG_MEDIA_BASE_PATH "/%s", name);
    struct stat st;
    size_t old_size = stat(path, &st) == 0 ? (size_t)st.st_size : 0;
    size_t total = 0, used = 0;
    esp_spiffs_info(RG_MEDIA_PARTITION, &total, &used);
    // SPIFFS занимает больше, чем размер файла (страницы, служебные блоки) - запас.
    if (total - used + old_size < len + len / 8 + 32768) {
        return send_text_err(req, "507 Insufficient Storage", "Not enough space");
    }

    const char *tmp_path = RG_MEDIA_BASE_PATH "/.upload";
    FILE *f = fopen(tmp_path, "wb");
    char *buf = malloc(OTA_CHUNK);
    if (f == NULL || buf == NULL) {
        if (f) fclose(f);
        free(buf);
        return send_text_err(req, "500 Internal Server Error", "Cannot create file");
    }
    const char *fail = NULL;
    for (size_t done = 0; done < len && !fail;) {
        size_t n = len - done < OTA_CHUNK ? len - done : OTA_CHUNK;
        if (!recv_exact(req, buf, n)) {
            fail = "Receive failed";
        } else if (fwrite(buf, 1, n, f) != n) {
            fail = "Write failed";
        }
        done += n;
    }
    free(buf);
    if (fclose(f) != 0 && !fail) {
        fail = "Write failed";
    }
    if (fail) {
        remove(tmp_path);
        return send_text_err(req, "500 Internal Server Error", fail);
    }
    remove(path);
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return send_text_err(req, "500 Internal Server Error", "Rename failed");
    }
    ESP_LOGW(TAG, "media: %s, %u bytes", name, (unsigned)len);
    roundGauge_layout_reload(); // экраны подхватят новую картинку
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t media_delete_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char name[40];
    if (!query_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    char path[sizeof(RG_MEDIA_BASE_PATH) + 40];
    snprintf(path, sizeof(path), RG_MEDIA_BASE_PATH "/%s", name);
    if (remove(path) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    roundGauge_layout_reload();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// CAN: настройки, таблица привязок, сниффер
// ---------------------------------------------------------------------------

static esp_err_t can_get_handler(httpd_req_t *req)
{
    roundGauge_can_settings_t c;
    roundGauge_settings_get_can(&c);
    roundGauge_can_status_t st;
    roundGauge_can_get_status(&st);
    char json[320];
    snprintf(json, sizeof(json),
             "{\"bitrate\":%u,\"mode\":\"%s\",\"demo\":%s,\"node_up\":%s,\"state\":%u,\"rx\":%u,\"dropped\":%u,"
             "\"tx_err\":%u,\"rx_err\":%u,\"bus_err\":%u,\"busoff\":%u}",
             (unsigned)c.bitrate, c.mode == RG_CAN_MODE_LISTEN_ONLY ? "listen_only" : "normal",
             c.demo ? "true" : "false", st.node_up ? "true" : "false", (unsigned)st.state, (unsigned)st.rx,
             (unsigned)st.dropped, (unsigned)st.tx_err, (unsigned)st.rx_err, (unsigned)st.bus_err,
             (unsigned)st.busoff_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Принять небольшое JSON-тело запроса в кучу (освободить free()); NULL - ответ уже отправлен.
static char *recv_body(httpd_req_t *req, size_t max)
{
    size_t len = req->content_len;
    if (len == 0 || len > max) {
        send_text_err(req, "400 Bad Request", "Bad body size");
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        send_text_err(req, "500 Internal Server Error", "No memory");
        return NULL;
    }
    if (!recv_exact(req, buf, len)) {
        free(buf);
        send_text_err(req, "400 Bad Request", "Receive failed");
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

// Поля, которых нет в запросе, остаются прежними.
static esp_err_t can_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = recv_body(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_text_err(req, "400 Bad Request", "Invalid JSON");
    }
    roundGauge_can_settings_t c;
    roundGauge_settings_get_can(&c);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "bitrate");
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 10000 || v->valuedouble > 1000000) {
            cJSON_Delete(root);
            return send_text_err(req, "400 Bad Request", "Bitrate must be 10000..1000000");
        }
        c.bitrate = (uint32_t)v->valuedouble;
    }
    v = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        if (strcmp(v->valuestring, "listen_only") == 0) {
            c.mode = RG_CAN_MODE_LISTEN_ONLY;
        } else if (strcmp(v->valuestring, "normal") == 0) {
            c.mode = RG_CAN_MODE_NORMAL;
        } else {
            cJSON_Delete(root);
            return send_text_err(req, "400 Bad Request", "Unknown mode");
        }
    }
    v = cJSON_GetObjectItemCaseSensitive(root, "demo");
    if (cJSON_IsBool(v)) {
        c.demo = cJSON_IsTrue(v);
    }
    cJSON_Delete(root);

    if (roundGauge_settings_set_can(&c) != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Save failed");
    }
    roundGauge_can_apply_settings();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t can_map_get_handler(httpd_req_t *req)
{
    char *json = roundGauge_can_map_json_dup();
    if (json == NULL) {
        return send_text_err(req, "500 Internal Server Error", "No memory");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

// Вернуть привязки к пресету rusEFI (стирает сохранённую таблицу).
static esp_err_t can_map_reset_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    if (roundGauge_can_map_reset() != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Reset failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t can_map_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = recv_body(req, RG_CAN_MAP_JSON_MAX);
    if (body == NULL) {
        return ESP_OK;
    }
    esp_err_t err = roundGauge_can_map_apply_json(body);
    free(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_text_err(req, "400 Bad Request", "Invalid CAN map");
    }
    if (err != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Save failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

// Сниффер: последний кадр каждого ID. Сам запрос включает приём чужих ID на
// RG_CAN_SNIFFER_ACTIVE_MS, поэтому первый ответ может быть пустым.
static esp_err_t can_frames_handler(httpd_req_t *req)
{
    roundGauge_can_frame_info_t *fr = malloc(sizeof(*fr) * RG_CAN_SNIFFER_MAX);
    if (fr == NULL) {
        return send_text_err(req, "500 Internal Server Error", "No memory");
    }
    size_t n = roundGauge_can_sniffer_snapshot(fr, RG_CAN_SNIFFER_MAX);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    for (size_t i = 0; i < n; i++) {
        char hex[17] = "";
        for (int b = 0; b < fr[i].dlc && b < 8; b++) {
            snprintf(hex + b * 2, 3, "%02x", fr[i].data[b]);
        }
        char row[128];
        int len = snprintf(row, sizeof(row), "%s{\"id\":%u,\"ext\":%s,\"dlc\":%u,\"data\":\"%s\",\"count\":%u,\"age\":%u}",
                           i ? "," : "", (unsigned)fr[i].id, fr[i].ext ? "true" : "false", (unsigned)fr[i].dlc, hex,
                           (unsigned)fr[i].count, (unsigned)fr[i].age_ms);
        httpd_resp_send_chunk(req, row, len);
    }
    free(fr);
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ---------------------------------------------------------------------------
// Яркость и пароль на настройки
// ---------------------------------------------------------------------------

static esp_err_t display_get_handler(httpd_req_t *req)
{
    roundGauge_display_settings_t d;
    roundGauge_settings_get_display(&d);
    char json[48];
    snprintf(json, sizeof(json), "{\"brightness\":%u}", (unsigned)d.brightness);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t display_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = recv_body(req, 128);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *v = root ? cJSON_GetObjectItemCaseSensitive(root, "brightness") : NULL;
    if (!cJSON_IsNumber(v) || v->valuedouble < RG_LCD_BL_MIN_PCT || v->valuedouble > 100) {
        cJSON_Delete(root);
        return send_text_err(req, "400 Bad Request", "Brightness must be " RG_STR(RG_LCD_BL_MIN_PCT) "..100");
    }
    roundGauge_display_settings_t d = { .brightness = (uint8_t)v->valuedouble };
    cJSON_Delete(root);
    if (roundGauge_settings_set_display(&d) != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Save failed");
    }
    roundGauge_board_backlight_set(d.brightness); // сразу, без перезагрузки
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

// Задать, сменить или снять пароль на настройки (пустой - снять). Пока пароль есть,
// запрос сам требует текущий (check_auth); пока нет - первый задаёт любой, кто в сети.
static esp_err_t auth_password_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = recv_body(req, 256);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *v = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
    if (!cJSON_IsString(v) || v->valuestring == NULL) {
        cJSON_Delete(root);
        return send_text_err(req, "400 Bad Request", "Invalid body");
    }
    size_t len = strlen(v->valuestring);
    if (len != 0 && (len < RG_AUTH_PASSWORD_MIN_LEN || len > RG_AUTH_PASSWORD_MAX_LEN)) {
        cJSON_Delete(root);
        return send_text_err(req, "400 Bad Request", "Password must be " RG_STR(RG_AUTH_PASSWORD_MIN_LEN) ".." RG_STR(RG_AUTH_PASSWORD_MAX_LEN) " characters");
    }
    bool ok = roundGauge_auth_set_password(len ? v->valuestring : NULL);
    cJSON_Delete(root);
    if (!ok) {
        return send_text_err(req, "500 Internal Server Error", "Save failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, roundGauge_auth_is_enabled() ? "{\"status\":\"success\",\"enabled\":true}"
                                                               : "{\"status\":\"success\",\"enabled\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Wi-Fi точка доступа: имя и пароль сети
// ---------------------------------------------------------------------------

// Имя по умолчанию: префикс и конец MAC - у каждой приборки своё.
static void default_ap_ssid(char *out, size_t n)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, n, RG_WIFI_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
}

// Пароль сети не отдаём: чтение открыто, а пароль это то, что защищает сеть.
static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    roundGauge_wifi_ap_settings_t w;
    roundGauge_settings_get_wifi(&w);
    char def[RG_WIFI_AP_SSID_MAX_LEN + 1];
    default_ap_ssid(def, sizeof(def));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", w.ssid[0] ? w.ssid : def);
    cJSON_AddStringToObject(root, "default_ssid", def);
    cJSON_AddBoolToObject(root, "custom_ssid", w.ssid[0] != '\0');
    cJSON_AddBoolToObject(root, "password_default", strcmp(w.password, RG_WIFI_AP_PASS_DEFAULT) == 0);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return send_text_err(req, "500 Internal Server Error", "No memory");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

// Менять Wi-Fi можно только с паролем на настройки: без него кто угодно в сети точки
// сменил бы пароль самой сети. Новые значения работают после перезагрузки.
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!roundGauge_auth_is_enabled()) {
        return send_text_err(req, "403 Forbidden", "Set a settings password first");
    }
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = recv_body(req, 512);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *ssid = root ? cJSON_GetObjectItemCaseSensitive(root, "ssid") : NULL;
    const cJSON *pass = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
    if (!cJSON_IsString(pass) || pass->valuestring == NULL || (ssid != NULL && !cJSON_IsString(ssid))) {
        cJSON_Delete(root);
        return send_text_err(req, "400 Bad Request", "Invalid body");
    }
    roundGauge_wifi_ap_settings_t w = {0};
    const char *name = ssid ? ssid->valuestring : "";
    size_t nlen = strlen(name), plen = strlen(pass->valuestring);
    bool ok = nlen <= RG_WIFI_AP_SSID_MAX_LEN && plen >= RG_WIFI_AP_PASSWORD_MIN_LEN &&
              plen <= RG_WIFI_AP_PASSWORD_MAX_LEN;
    for (size_t i = 0; ok && i < nlen; i++) {
        ok = (unsigned char)name[i] >= 0x20 && name[i] != 0x7f;
    }
    if (!ok) {
        cJSON_Delete(root);
        return send_text_err(req, "400 Bad Request",
                             "SSID up to " RG_STR(RG_WIFI_AP_SSID_MAX_LEN) " bytes, password " RG_STR(RG_WIFI_AP_PASSWORD_MIN_LEN) ".." RG_STR(RG_WIFI_AP_PASSWORD_MAX_LEN) " characters");
    }
    strlcpy(w.ssid, name, sizeof(w.ssid));
    strlcpy(w.password, pass->valuestring, sizeof(w.password));
    cJSON_Delete(root);
    if (roundGauge_settings_set_wifi(&w) != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Save failed");
    }
    ESP_LOGI(TAG, "Wi-Fi settings saved, apply after restart");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\",\"restart\":true}", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Акселерометр: живые показания, калибровка, направление "вперёд"
// ---------------------------------------------------------------------------

static esp_err_t imu_get_handler(httpd_req_t *req)
{
    static const char *const cal[] = { "idle", "running", "done", "moved" };
    static const char *const det[] = { "idle", "armed", "done", "timeout" };
    roundGauge_imu_state_t s;
    roundGauge_imu_get_state(&s);
    roundGauge_imu_settings_t cfg;
    roundGauge_settings_get_imu(&cfg);
    char json[400];
    snprintf(json, sizeof(json),
             "{\"ok\":%s,\"calibrated\":%s,\"fwd\":%u,\"cal\":\"%s\",\"detect\":\"%s\","
             "\"raw\":[%.3f,%.3f,%.3f],\"g0\":[%.3f,%.3f,%.3f],\"lon\":%.3f,\"lat\":%.3f,\"vert\":%.3f,\"tot\":%.3f}",
             s.ok ? "true" : "false", s.calibrated ? "true" : "false", (unsigned)s.fwd, cal[s.cal & 3], det[s.fwd_detect & 3],
             (double)s.raw[0], (double)s.raw[1], (double)s.raw[2], (double)cfg.g0[0], (double)cfg.g0[1], (double)cfg.g0[2],
             (double)s.lon, (double)s.lat, (double)s.vert, (double)s.tot);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t imu_calibrate_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    if (roundGauge_imu_calibrate() != ESP_OK) {
        return send_text_err(req, "409 Conflict", "Sensor not available");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN);
}

// {"fwd": 0..3} - выбрать вручную; {"detect": true} - определить по разгону.
static esp_err_t imu_forward_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = recv_body(req, 128);
    if (body == NULL) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_text_err(req, "400 Bad Request", "Invalid JSON");
    }
    const cJSON *fwd = cJSON_GetObjectItemCaseSensitive(root, "fwd");
    const cJSON *detect = cJSON_GetObjectItemCaseSensitive(root, "detect");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsNumber(fwd) && fwd->valuedouble >= 0 && fwd->valuedouble <= 3) {
        err = roundGauge_imu_set_forward((uint8_t)fwd->valuedouble);
    } else if (cJSON_IsTrue(detect)) {
        err = roundGauge_imu_detect_forward();
    }
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_text_err(req, "400 Bad Request", "Expected fwd 0..3 or detect");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_text_err(req, "409 Conflict", "Sensor not available");
    }
    if (err != ESP_OK) {
        return send_text_err(req, "500 Internal Server Error", "Save failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, RG_OTA_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static void start_http_server(void)
{
    s_upd_mutex = xSemaphoreCreateMutex();
    www_mount();

    esp_err_t err;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = RG_HTTPD_STACK;
    cfg.task_priority = RG_HTTPD_PRIORITY;
    cfg.core_id = RG_HTTPD_CORE;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 36;
    cfg.recv_wait_timeout = 10;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return;
    }
    // Специфичные обработчики раньше шаблона "/*".
    static const httpd_uri_t uris[] = {
        {.uri = "/ota", .method = HTTP_GET, .handler = ota_page_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/auth/status", .method = HTTP_GET, .handler = auth_status_handler},
        {.uri = "/api/auth/verify", .method = HTTP_POST, .handler = auth_verify_handler},
        {.uri = "/api/ota/update", .method = HTTP_POST, .handler = ota_update_handler},
        {.uri = "/api/www/update", .method = HTTP_POST, .handler = www_update_handler},
        {.uri = "/api/layout", .method = HTTP_GET, .handler = layout_get_handler},
        {.uri = "/api/layout", .method = HTTP_POST, .handler = layout_put_handler},
        {.uri = "/api/layout/reset", .method = HTTP_POST, .handler = layout_reset_handler},
        {.uri = "/api/media", .method = HTTP_GET, .handler = media_list_handler},
        {.uri = "/api/media", .method = HTTP_POST, .handler = media_upload_handler},
        {.uri = "/api/media/delete", .method = HTTP_POST, .handler = media_delete_handler},
        {.uri = "/api/imu", .method = HTTP_GET, .handler = imu_get_handler},
        {.uri = "/api/imu/calibrate", .method = HTTP_POST, .handler = imu_calibrate_handler},
        {.uri = "/api/imu/forward", .method = HTTP_POST, .handler = imu_forward_handler},
        {.uri = "/api/display", .method = HTTP_GET, .handler = display_get_handler},
        {.uri = "/api/display", .method = HTTP_POST, .handler = display_post_handler},
        {.uri = "/api/auth/password", .method = HTTP_POST, .handler = auth_password_handler},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler},
        {.uri = "/api/can", .method = HTTP_GET, .handler = can_get_handler},
        {.uri = "/api/can", .method = HTTP_POST, .handler = can_post_handler},
        {.uri = "/api/can/map", .method = HTTP_GET, .handler = can_map_get_handler},
        {.uri = "/api/can/map", .method = HTTP_POST, .handler = can_map_post_handler},
        {.uri = "/api/can/map/reset", .method = HTTP_POST, .handler = can_map_reset_handler},
        {.uri = "/api/can/frames", .method = HTTP_GET, .handler = can_frames_handler},
        {.uri = "/media/*", .method = HTTP_GET, .handler = media_get_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = www_get_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

// ---------------------------------------------------------------------------
// Состояние точки доступа для экрана
// ---------------------------------------------------------------------------
static roundGauge_ap_info_t s_ap_info;
static portMUX_TYPE s_ap_lock = portMUX_INITIALIZER_UNLOCKED;

void roundGauge_webcfg_get_ap_info(roundGauge_ap_info_t *out)
{
    portENTER_CRITICAL(&s_ap_lock);
    *out = s_ap_info;
    portEXIT_CRITICAL(&s_ap_lock);
}

static void ap_set_state(roundGauge_ap_state_t state, const char *ssid, const char *password)
{
    portENTER_CRITICAL(&s_ap_lock);
    s_ap_info.state = state;
    if (ssid != NULL) {
        strlcpy(s_ap_info.ssid, ssid, sizeof(s_ap_info.ssid));
        strlcpy(s_ap_info.password, password ? password : "", sizeof(s_ap_info.password));
    }
    portEXIT_CRITICAL(&s_ap_lock);
}

// Кто подключился к точке и кто отключился - для числа клиентов на значке.
static void ap_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    portENTER_CRITICAL(&s_ap_lock);
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_info.clients++;
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_ap_info.clients > 0) {
        s_ap_info.clients--;
    }
    portEXIT_CRITICAL(&s_ap_lock);
}

static void start_access_point(void)
{
    const roundGauge_settings_t *cfg = roundGauge_settings_get();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t wc = {0};
    if (cfg->wifi_ap.ssid[0]) {
        strlcpy((char *)wc.ap.ssid, cfg->wifi_ap.ssid, sizeof(wc.ap.ssid));
    } else {
        default_ap_ssid((char *)wc.ap.ssid, sizeof(wc.ap.ssid));
    }
    wc.ap.ssid_len = strlen((char *)wc.ap.ssid);
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    if (strlen(cfg->wifi_ap.password) >= RG_WIFI_AP_PASSWORD_MIN_LEN) {
        strlcpy((char *)wc.ap.password, cfg->wifi_ap.password, sizeof(wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ESP_LOGW(TAG, "AP password shorter than %d - open network", RG_WIFI_AP_PASSWORD_MIN_LEN);
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(TAG, "Access point \"%s\" up, open http://192.168.4.1/", wc.ap.ssid);

    start_http_server();

    // Только теперь сеть и сайт готовы - экран покажет данные для подключения.
    ap_set_state(RG_AP_UP, (const char *)wc.ap.ssid,
                 wc.ap.authmode == WIFI_AUTH_OPEN ? "" : (const char *)wc.ap.password);
}

static void webcfg_task(void *arg)
{
    ESP_LOGI(TAG, "Web config task started, access point is off");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RG_WEBCFG_TICK_MS));

        if (atomic_exchange(&s_start_requested, false) && !atomic_load(&s_running)) {
            atomic_store(&s_running, true);
            start_access_point();
        }
    }
}

void roundGauge_webcfg_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(webcfg_task, "webcfg_task", RG_WEBCFG_TASK_STACK, NULL,
                                            RG_WEBCFG_TASK_PRIORITY, NULL, RG_WEBCFG_TASK_CORE);
    configASSERT(ok == pdPASS);
}

void roundGauge_webcfg_start(void)
{
    // Экран сразу показывает "запуск", не дожидаясь, пока webcfg_task подхватит запрос.
    portENTER_CRITICAL(&s_ap_lock);
    if (s_ap_info.state == RG_AP_OFF) {
        s_ap_info.state = RG_AP_STARTING;
    }
    portEXIT_CRITICAL(&s_ap_lock);
    atomic_store(&s_start_requested, true);
}

bool roundGauge_webcfg_running(void)
{
    return atomic_load(&s_running);
}
