#include "storage.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "store";

#define MOUNT_POINT   "/data"
#define PART_LABEL    "storage"
#define CAP_FILE      MOUNT_POINT "/cap.bin"
#define PIN_FILE      MOUNT_POINT "/pin.bin"
#define PIN_MAGIC     0xCA9E0009u
#define REC_MAGIC     0xCA9E0007u   /* bumped: record gained a src tag */
#define HDR_MAGIC     0xCA9E0008u   /* bumped together with REC_MAGIC */

typedef struct {
    uint32_t magic;
    uint32_t count;      /* valid records, 0..CAP_SLOTS */
    uint32_t head;       /* next slot to write */
} cap_hdr_t;

/* Favourites file: header + PIN_SLOTS fixed rows, append until full, removed by index. */
typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t reserved;
} pin_hdr_t;

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
    storage_ui_prepare();       /* /data/www: the optional uploaded SPA */

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
static void rec_from(const detection_t *d, uint32_t boot_seq, uint8_t src, cap_rec_t *r)
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
    r->is_ibeacon = d->is_ibeacon ? 1 : 0;
    r->src        = src;
    uint32_t dwell = d->last_ms > d->first_ms ? d->last_ms - d->first_ms : 0;
    r->dwell_ms  = dwell > 0xFFFF ? 0xFFFF : (uint16_t)dwell;
    r->count     = d->count;
}

int storage_capture_add_many(const detection_t *d, int n, uint32_t boot_seq, uint8_t src)
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
                rec_from(&d[i], boot_seq, src, &r);
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
    return storage_capture_add_many(d, 1, boot_seq, REC_SRC_CLONE) == 1 ? ESP_OK
                                                                        : ESP_ERR_INVALID_STATE;
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

/* ------------------------------- favourites ------------------------------- */

static esp_err_t hdr_store_pin(FILE *f, const pin_hdr_t *h)
{
    if (fseek(f, 0, SEEK_SET) != 0) return ESP_FAIL;
    if (fwrite(h, sizeof(*h), 1, f) != 1) return ESP_FAIL;
    return fflush(f) == 0 ? ESP_OK : ESP_FAIL;
}

/* Opens pin.bin, creating/repairing the header when needed. Caller holds s_mtx. */
static FILE *pin_open(bool create)
{
    FILE *f = fopen(PIN_FILE, "r+b");
    if (f) {
        pin_hdr_t h;
        if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == PIN_MAGIC && h.count <= PIN_SLOTS) {
            return f;
        }
        fclose(f);
        if (!create) return NULL;
        ESP_LOGW(TAG, "pin header invalid, rebuilding");
    } else if (!create) {
        return NULL;
    }

    f = fopen(PIN_FILE, "w+b");
    if (!f) return NULL;
    /* size the file once so later writes never extend it */
    if (fseek(f, (long)sizeof(pin_hdr_t) + (long)sizeof(cap_rec_t) * PIN_SLOTS - 1, SEEK_SET) != 0 ||
        fputc(0, f) == EOF) {
        fclose(f);
        return NULL;
    }
    const pin_hdr_t h = { .magic = PIN_MAGIC, .count = 0, .reserved = 0 };
    if (hdr_store_pin(f, &h) != ESP_OK) {
        fclose(f);
        return NULL;
    }
    return f;
}

int storage_pin_list(cap_rec_t *out, int max)
{
    if (!s_ready || !s_mtx || !out || max <= 0) return 0;
    if (max > PIN_SLOTS) max = PIN_SLOTS;

    int n = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = pin_open(true);
    if (f) {
        pin_hdr_t h;
        if (fseek(f, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, f) == 1 &&
            h.magic == PIN_MAGIC && h.count <= PIN_SLOTS) {
            int want = h.count < max ? (int)h.count : max;
            for (int i = 0; i < want; i++) {
                cap_rec_t r;
                if (fseek(f, (long)sizeof(pin_hdr_t) + (long)sizeof(cap_rec_t) * i, SEEK_SET) != 0) break;
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

/* Same device + same bytes = the same favourite; pinning twice is a no-op. */
static bool pin_same(const cap_rec_t *a, const cap_rec_t *b)
{
    return a->raw_len == b->raw_len && a->rsp_len == b->rsp_len &&
           memcmp(a->addr, b->addr, 6) == 0 &&
           memcmp(a->raw, b->raw, a->raw_len) == 0 &&
           memcmp(a->rsp, b->rsp, a->rsp_len) == 0;
}

esp_err_t storage_pin_add(const cap_rec_t *r)
{
    if (!s_ready || !s_mtx || !r) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_FAIL;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = pin_open(true);
    if (f) {
        pin_hdr_t h;
        if (fseek(f, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, f) == 1 && h.magic == PIN_MAGIC) {
            bool dup = false;
            for (int i = 0; i < (int)h.count && !dup; i++) {
                cap_rec_t o;
                if (fseek(f, (long)sizeof(pin_hdr_t) + (long)sizeof(cap_rec_t) * i, SEEK_SET) != 0) break;
                if (fread(&o, sizeof(o), 1, f) == 1 && o.magic == REC_MAGIC) dup = pin_same(&o, r);
            }
            if (dup) {
                ret = ESP_OK;                      /* already pinned */
            } else if (h.count >= PIN_SLOTS) {
                ret = ESP_ERR_NO_MEM;
            } else if (fseek(f, (long)sizeof(pin_hdr_t) + (long)sizeof(cap_rec_t) * h.count, SEEK_SET) == 0 &&
                       fwrite(r, sizeof(*r), 1, f) == 1) {
                h.count++;
                if (hdr_store_pin(f, &h) == ESP_OK) ret = ESP_OK;
            }
        }
        fclose(f);
    }
    xSemaphoreGive(s_mtx);
    if (ret != ESP_OK) ESP_LOGW(TAG, "pin add failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t storage_pin_remove(int idx)
{
    if (!s_ready || !s_mtx) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = pin_open(false);
    if (f) {
        pin_hdr_t h;
        if (fseek(f, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, f) == 1 &&
            h.magic == PIN_MAGIC && idx >= 0 && idx < (int)h.count) {
            /* shift the tail down by one row */
            ret = ESP_OK;
            for (int i = idx; i < (int)h.count - 1; i++) {
                cap_rec_t r;
                if (fseek(f, (long)sizeof(pin_hdr_t) + (long)sizeof(cap_rec_t) * (i + 1), SEEK_SET) != 0 ||
                    fread(&r, sizeof(r), 1, f) != 1) { ret = ESP_FAIL; break; }
                if (fseek(f, (long)sizeof(pin_hdr_t) + (long)sizeof(cap_rec_t) * i, SEEK_SET) != 0 ||
                    fwrite(&r, sizeof(r), 1, f) != 1) { ret = ESP_FAIL; break; }
                ret = ESP_OK;
            }
            if (ret != ESP_FAIL) {
                h.count--;
                ret = hdr_store_pin(f, &h);
            }
        }
        fclose(f);
    }
    xSemaphoreGive(s_mtx);
    return ret;
}

esp_err_t storage_pin_clear(void)
{
    if (!s_ready || !s_mtx) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_FAIL;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    FILE *f = pin_open(false);
    if (f) {
        pin_hdr_t h = { .magic = PIN_MAGIC, .count = 0, .reserved = 0 };
        ret = hdr_store_pin(f, &h);
        fclose(f);
    }
    xSemaphoreGive(s_mtx);
    return ret;
}

/* ------------------------------ uploaded SPA ------------------------------ */

#define UI_DIR   MOUNT_POINT "/www"
#define UI_FILE  UI_DIR "/index.html.gz"
#define UI_TMP   UI_DIR "/ui.tmp"

static SemaphoreHandle_t s_ui_mtx;    /* one upload and one reader at a time */
static FILE *s_ui_up;
static bool  s_ui_up_ok;              /* false once a chunk failed: such a .tmp must not be promoted */
static size_t s_ui_up_len;
static FILE *s_ui_rd;

void storage_ui_prepare(void)
{
    if (!s_ready) return;
    if (!s_ui_mtx) s_ui_mtx = xSemaphoreCreateMutex();
    remove(UI_TMP);     /* an upload cut short by a reboot leaves only its tmp behind */
    if (mkdir(UI_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed (errno %d) -- the UI stays embedded", UI_DIR, errno);
    }
}

/* gzip magic: the device never inflates, so this is the only sanity check it can do */
static bool ui_is_gzip(const uint8_t *b, size_t n)
{
    return n >= 2 && b[0] == 0x1f && b[1] == 0x8b;
}

esp_err_t storage_ui_stat(uint32_t *out_crc, size_t *out_len)
{
    if (!s_ready || !s_ui_mtx) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ui_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    FILE *f = fopen(UI_FILE, "rb");
    if (f) {
        uint8_t tmp[64];
        uint32_t crc = 0;
        size_t total = 0, n;
        bool gz = false;
        while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
            if (total == 0) gz = ui_is_gzip(tmp, n);
            crc = esp_rom_crc32_le(crc, tmp, (uint32_t)n);
            total += n;
        }
        if (ferror(f)) total = 0;   /* a CRC over a prefix is no ETag: treat a read error as absent */
        fclose(f);
        if (gz && total >= UI_MIN_BYTES && total <= UI_MAX_BYTES) {
            if (out_crc) *out_crc = crc;
            if (out_len) *out_len = total;
            ret = ESP_OK;
        }
        /* a truncated or foreign file is treated as absent: the caller serves the embedded copy */
    }
    xSemaphoreGive(s_ui_mtx);
    return ret;
}

esp_err_t storage_ui_remove(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return remove(UI_FILE) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t storage_ui_read_begin(void)
{
    if (!s_ready || !s_ui_mtx) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ui_mtx, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_ui_rd = fopen(UI_FILE, "rb");
    if (!s_ui_rd) { xSemaphoreGive(s_ui_mtx); return ESP_ERR_NOT_FOUND; }
    return ESP_OK;
}

int storage_ui_read_chunk(uint8_t *buf, size_t cap)
{
    if (!s_ui_rd || !buf || cap == 0) return -1;
    size_t n = fread(buf, 1, cap, s_ui_rd);
    if (n == 0 && ferror(s_ui_rd)) return -1;
    return (int)n;
}

/* Only the holder releases: begin() already gives the mutex on its own failure paths,
 * so read_end() must stay a no-op unless a read session is actually open. */
void storage_ui_read_end(void)
{
    if (!s_ui_rd) return;
    fclose(s_ui_rd);
    s_ui_rd = NULL;
    xSemaphoreGive(s_ui_mtx);
}

esp_err_t storage_ui_upload_begin(void)
{
    if (!s_ready || !s_ui_mtx) return ESP_ERR_INVALID_STATE;
    /* non-blocking: a second concurrent upload is a client bug, not something to queue */
    if (xSemaphoreTake(s_ui_mtx, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    s_ui_up = fopen(UI_TMP, "wb");
    if (!s_ui_up) { xSemaphoreGive(s_ui_mtx); return ESP_ERR_NO_MEM; }
    s_ui_up_ok  = true;
    s_ui_up_len = 0;
    return ESP_OK;
}

esp_err_t storage_ui_upload_chunk(const void *buf, size_t len)
{
    if (!s_ui_up || !buf || len == 0) return ESP_ERR_INVALID_ARG;
    /* the partition is shared with the archive: an upload must never eat it */
    if (len > UI_MAX_BYTES - s_ui_up_len) { s_ui_up_ok = false; return ESP_ERR_INVALID_SIZE; }
    if (fwrite(buf, 1, len, s_ui_up) != len) { s_ui_up_ok = false; return ESP_ERR_NO_MEM; }
    s_ui_up_len += len;
    return ESP_OK;
}

esp_err_t storage_ui_upload_commit(void)
{
    if (!s_ui_up) return ESP_ERR_INVALID_STATE;
    /* both calls must run (short-circuiting on fflush would leak the handle and its fd slot) */
    int rc = fflush(s_ui_up);
    if (fclose(s_ui_up) != 0) rc = -1;
    s_ui_up = NULL;
    /* a .tmp that some chunk failed to write is still >= 1 KB of valid-looking gzip, so:
     * never promote it, or stat() accepts it and the device serves a broken page */
    bool ok = s_ui_up_ok && rc == 0;
    s_ui_up_ok = false;
    if (!ok) { remove(UI_TMP); xSemaphoreGive(s_ui_mtx); return ESP_FAIL; }
    esp_err_t ret = ESP_OK;
    if (rename(UI_TMP, UI_FILE) != 0) {
        /* FatFs rename refuses an existing destination: drop the old file and retry */
        remove(UI_FILE);
        ret = rename(UI_TMP, UI_FILE) == 0 ? ESP_OK : ESP_FAIL;
    }
    if (ret != ESP_OK) remove(UI_TMP);
    xSemaphoreGive(s_ui_mtx);
    return ret;
}

/* No-op without an open session, for the same reason as storage_ui_read_end(). */
void storage_ui_upload_abort(void)
{
    if (!s_ui_up) return;
    fclose(s_ui_up);
    s_ui_up = NULL;
    s_ui_up_ok = false;
    remove(UI_TMP);
    xSemaphoreGive(s_ui_mtx);
}
