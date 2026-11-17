#include "storage.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "store";

#define MOUNT_POINT   "/data"
#define PART_LABEL    "storage"
#define CAP_FILE      MOUNT_POINT "/cap.bin"
#define REC_MAGIC     0xCA9E0002u   /* bumped: cap_rec_t gained manufacturer data */
#define HDR_MAGIC     0xCA9E0003u   /* bumped together with REC_MAGIC */

typedef struct {
    uint32_t magic;
    uint32_t count;      /* valid records, 0..CAP_SLOTS */
    uint32_t head;       /* next slot to write */
} cap_hdr_t;

static wl_handle_t       s_wl = WL_INVALID_HANDLE;
static bool              s_ready;
static SemaphoreHandle_t s_mtx;

static const char *path(void) { return CAP_FILE; }

static esp_err_t hdr_load(FILE *f, cap_hdr_t *h)
{
    if (fseek(f, 0, SEEK_SET) != 0) return ESP_FAIL;
    if (fread(h, sizeof(*h), 1, f) != 1) return ESP_FAIL;
    if (h->magic != HDR_MAGIC || h->head >= CAP_SLOTS || h->count > CAP_SLOTS) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t hdr_store(FILE *f, const cap_hdr_t *h)
{
    if (fseek(f, 0, SEEK_SET) != 0) return ESP_FAIL;
    if (fwrite(h, sizeof(*h), 1, f) != 1) return ESP_FAIL;
    if (fflush(f) != 0) return ESP_FAIL;
    return ESP_OK;
}

/* Open and make sure the layout is sane (rebuild when it is not) */
static FILE *open_ready(void)
{
    FILE *f = fopen(path(), "r+b");
    bool  fresh = false;
    if (!f) {
        f = fopen(path(), "w+b");
        fresh = true;
    }
    if (!f) return NULL;

    if (!fresh) {
        cap_hdr_t h;
        if (hdr_load(f, &h) == ESP_OK) return f;
        ESP_LOGW(TAG, "cap.bin header invalid, rebuilding");
    }

    /* Initialise: zeroed records + header */
    if (fseek(f, (long)sizeof(cap_hdr_t) + (long)sizeof(cap_rec_t) * CAP_SLOTS - 1, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    fputc(0, f);
    cap_hdr_t h = { .magic = HDR_MAGIC, .count = 0, .head = 0 };
    if (hdr_store(f, &h) != ESP_OK) {
        fclose(f);
        return NULL;
    }
    return f;
}

esp_err_t storage_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files              = 4,
        .allocation_unit_size   = 4096,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, PART_LABEL, &cfg, &s_wl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s (archive disabled, everything else still works)",
                 PART_LABEL, esp_err_to_name(e));
        return e;
    }
    s_ready = true;

    /* Probe once, creating the file if needed */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = open_ready();
    xSemaphoreGive(s_mtx);
    if (f) {
        fclose(f);
        ESP_LOGI(TAG, "storage ready: %s", CAP_FILE);
    }
    return ESP_OK;
}

bool storage_ready(void) { return s_ready; }

esp_err_t storage_capture_add(const detection_t *d, uint32_t boot_seq)
{
    if (!s_ready || !s_mtx || !d) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t ret = ESP_FAIL;
    FILE *f = open_ready();
    if (f) {
        cap_hdr_t h;
        if (hdr_load(f, &h) == ESP_OK) {
            cap_rec_t r;
            memset(&r, 0, sizeof(r));
            r.magic      = REC_MAGIC;
            r.uptime_s   = (uint32_t)(esp_timer_get_time() / 1000000);
            r.boot_seq   = boot_seq;
            r.is_ibeacon = d->is_ibeacon;
            memcpy(r.addr, d->addr, 6);
            r.addr_type  = d->addr_type;
            r.adv_type   = d->adv_type;
            r.raw_len    = d->raw_len > 31 ? 31 : d->raw_len;
            memcpy(r.raw, d->raw, r.raw_len);
            if (d->is_ibeacon) {
                memcpy(r.uuid, d->uuid, 16);
                r.major = d->major;
                r.minor = d->minor;
                r.ref_rssi = d->ref_rssi;
            }
            r.mfg_company = d->mfg_company;
            r.mfg_len     = d->mfg_len > sizeof(r.mfg) ? sizeof(r.mfg) : d->mfg_len;
            memcpy(r.mfg, d->mfg, r.mfg_len);
            r.mfg_src     = d->mfg_src;
            r.itvl_ms = d->itvl_ms;
            r.rssi    = d->rssi_last;

            if (fseek(f, (long)sizeof(cap_hdr_t) + (long)sizeof(cap_rec_t) * h.head, SEEK_SET) == 0 &&
                fwrite(&r, sizeof(r), 1, f) == 1 && fflush(f) == 0) {
                h.head = (h.head + 1) % CAP_SLOTS;
                if (h.count < CAP_SLOTS) h.count++;
                if (hdr_store(f, &h) == ESP_OK) ret = ESP_OK;
            }
        }
        fclose(f);
    }
    xSemaphoreGive(s_mtx);

    if (ret != ESP_OK) ESP_LOGW(TAG, "archive write failed");
    return ret;
}

int storage_capture_list(cap_rec_t *out, int max)
{
    if (!s_ready || !s_mtx || !out || max <= 0) return 0;

    if (max > CAP_SLOTS) max = CAP_SLOTS;
    int n = 0;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = open_ready();
    if (f) {
        cap_hdr_t h;
        if (hdr_load(f, &h) == ESP_OK && h.count > 0) {
            int want = h.count < max ? (int)h.count : max;
            for (int i = 0; i < want; i++) {
                /* Walk backwards from the newest: head-1, head-2, ... */
                int idx = ((int)h.head - 1 - i + CAP_SLOTS * 2) % CAP_SLOTS;
                cap_rec_t r;
                if (fseek(f, (long)sizeof(cap_hdr_t) + (long)sizeof(cap_rec_t) * idx, SEEK_SET) != 0) break;
                if (fread(&r, sizeof(r), 1, f) != 1) break;
                if (r.magic != REC_MAGIC) continue;
                out[n++] = r;
            }
        }
        fclose(f);
    }
    xSemaphoreGive(s_mtx);
    return n;
}

esp_err_t storage_capture_clear(void)
{
    if (!s_ready || !s_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t ret = ESP_FAIL;
    FILE *f = open_ready();
    if (f) {
        cap_hdr_t h = { .magic = HDR_MAGIC, .count = 0, .head = 0 };
        if (hdr_store(f, &h) == ESP_OK) ret = ESP_OK;
        fclose(f);
    }
    xSemaphoreGive(s_mtx);
    return ret;
}

const char *storage_stat_str(char *buf, size_t len)
{
    if (!buf || len == 0) return "?";
    if (!s_ready) { snprintf(buf, len, "off"); return buf; }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = open_ready();
    cap_hdr_t h = {0};
    if (f) {
        hdr_load(f, &h);
        fclose(f);
    }
    xSemaphoreGive(s_mtx);

    snprintf(buf, len, "%u/%u", (unsigned)h.count, (unsigned)CAP_SLOTS);
    return buf;
}
