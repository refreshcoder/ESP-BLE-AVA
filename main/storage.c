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
#define REC_MAGIC     0xCA9E0005u   /* bumped: record gained rsp/name/svc/count/dwell */
#define HDR_MAGIC     0xCA9E0006u   /* bumped together with REC_MAGIC */

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
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }

    /* Deliberately NOT format_if_mount_failed: a half-written FAT after a power cut would
     * otherwise be silently wiped, and what is on flash is field data that only exists once.
     * An unmountable partition therefore stays unmountable (the raw bytes are still readable
     * with `esptool read-flash 0x320000 0xD0000`), and the UI reports storage: off. */
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 4096,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, PART_LABEL, &cfg, &s_wl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s (archive disabled, data left untouched; "
                 "everything else still works)", PART_LABEL, esp_err_to_name(e));
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

/* The mount no longer formats on its own (see above), so a never-used or erased partition
 * needs this one deliberate call. Only from the USB console, never automatically. */
esp_err_t storage_format(void)
{
    if (s_ready) return ESP_ERR_INVALID_STATE;      /* would delete live records */
    esp_err_t e = esp_vfs_fat_spiflash_format_rw_wl(MOUNT_POINT, PART_LABEL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "format %s failed: %s", PART_LABEL, esp_err_to_name(e));
        return e;
    }
    ESP_LOGW(TAG, "%s formatted on request", PART_LABEL);
    return storage_init();
}

/* One row of the archive: everything a reader needs to understand the capture without
 * touching the raw bytes again, but the raw bytes are still the source of truth. */
static void rec_from(const detection_t *d, uint32_t boot_seq, cap_rec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->magic      = REC_MAGIC;
    r->boot_seq   = boot_seq;
    r->uptime_s   = (uint32_t)(esp_timer_get_time() / 1000000);
    memcpy(r->addr, d->addr, 6);
    r->addr_type  = d->addr_type;
    r->adv_type   = d->adv_type;

    r->raw_len    = d->raw_len > CFG_RAW_MAX ? CFG_RAW_MAX : d->raw_len;
    memcpy(r->raw, d->raw, r->raw_len);
    r->rsp_len    = d->rsp_len > CFG_RAW_MAX ? CFG_RAW_MAX : d->rsp_len;
    memcpy(r->rsp, d->rsp, r->rsp_len);

    r->mfg_company = d->mfg_company;
    r->svc_uuid    = d->svc_uuid;
    r->mfg_len     = d->mfg_len > sizeof(r->mfg) ? sizeof(r->mfg) : d->mfg_len;
    memcpy(r->mfg, d->mfg, r->mfg_len);
    r->mfg_src     = d->mfg_src;
    snprintf(r->name, sizeof(r->name), "%s", d->name);

    if (d->is_ibeacon) {
        memcpy(r->uuid, d->uuid, 16);
        r->major    = d->major;
        r->minor    = d->minor;
        r->ref_rssi = d->ref_rssi;
    }

    r->rssi_last = d->rssi_last;
    r->rssi_max  = d->rssi_max;
    r->rssi_avg  = d->rssi_avg;
    r->itvl_ms   = d->itvl_ms;
    uint32_t dwell = d->last_ms > d->first_ms ? d->last_ms - d->first_ms : 0;
    r->dwell_ms  = dwell > 0xFFFF ? 0xFFFF : (uint16_t)dwell;
    r->count     = d->count;
}

int storage_capture_add_many(const detection_t *d, int n, uint32_t boot_seq)
{
    if (!s_ready || !s_mtx || !d || n <= 0) return 0;

    int written = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = open_ready();
    if (f) {
        cap_hdr_t h;
        if (hdr_load(f, &h) == ESP_OK) {
            bool ok = true;
            for (int i = 0; i < n && ok; i++) {
                cap_rec_t r;
                rec_from(&d[i], boot_seq, &r);
                if (fseek(f, (long)sizeof(cap_hdr_t) + (long)sizeof(cap_rec_t) * h.head, SEEK_SET) != 0 ||
                    fwrite(&r, sizeof(r), 1, f) != 1) {
                    ok = false;
                    break;
                }
                h.head = (h.head + 1) % CAP_SLOTS;
                if (h.count < CAP_SLOTS) h.count++;
                written++;
            }
            /* one flush for the whole batch: a scan window is one write, not one per row */
            if (written && (fflush(f) != 0 || hdr_store(f, &h) != ESP_OK)) written = 0;
        }
        fclose(f);
    }
    xSemaphoreGive(s_mtx);

    if (!written) ESP_LOGW(TAG, "archive write failed (%d record(s))", n);
    return written;
}

esp_err_t storage_capture_add(const detection_t *d, uint32_t boot_seq)
{
    return storage_capture_add_many(d, 1, boot_seq) == 1 ? ESP_OK : ESP_ERR_INVALID_STATE;
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
