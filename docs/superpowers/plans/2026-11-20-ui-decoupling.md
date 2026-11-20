# UI 与固件解耦 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 SPA 可以从 `/data/www/index.html.gz` 提供并支持单独上传，改 UI 不必重编重刷固件，且任何上传失败都回落到内置版本。

**Architecture:** 复用已挂载的 FATFS（`/data`，分区 `storage`），新增 `/data/www` 目录存一个原样 gzip 的 `index.html.gz`。`h_index` 变成两态：文件可用就用它（`ETag: "ui-<crc32>"`、`Cache-Control: no-cache`、分块流式发送），否则用内置的 `index_html_gz`（构建常量 ETag、同样 `no-cache`）。上传走 `POST /api/ui`，按 4 KB 块直接落盘到 `.tmp` 再 `rename`，设备从不解压。

**Tech Stack:** ESP-IDF `esp_http_server`（`httpd_resp_send_chunk` / `httpd_req_recv`）、FATFS + `esp_vfs_fat_spiflash`、`esp_rom_crc32_le`；PC 侧 `tools/ui_push.py`（标准库 `gzip`/`urllib`）。

**Spec:** `docs/superpowers/specs/2026-11-20-ui-decoupling-design.md`

## Global Constraints

- 只接受 gzip：首两字节必须是 `1f 8b`；**设备永不解压**，体积上限只约束 flash。
- 大小限制：`1024 <= bytes <= 65536`（`UI_MIN_BYTES` / `UI_MAX_BYTES`）。
- 落盘必须 `.tmp` + `rename`，任何中断都不得留下半截文件。
- 内置 `index_html_gz` **永远保留**为兜底；`EMBED_FILES` 不动。
- 不改分区表、不改 NVS、不改 `main/CMakeLists.txt`。
- 新端点全部要过 `auth_ok(req)`（`Authorization: Bearer <token>`）。
- 发送与上传都按 **4 KB 块**流式处理，禁止为整页/整个请求体做堆分配。
- 本机没有 ESP-IDF：**每个任务的"测试"= 推送后 CI 编译全绿**（`esp32c3` / `esp32c6 board1` / `esp32c6 board2` + `Frontend syntax`）。上板验证按 Task 2/3/4 里写的 curl 命令做。
- 提交信息：现有风格 `feat(scope): …` / `docs: …`，英文。

---

### Task 1: 存储层——UI 文件的读、写、删

**Files:**
- Modify: `main/storage.h`（在 `storage_format()` 声明附近追加）
- Modify: `main/storage.c`（文件末尾追加一段；`storage_init()` 里调 `storage_ui_prepare()`）
- Modify: `docs/superpowers/specs/2026-11-20-ui-decoupling-design.md`（§2 的函数清单改成下面的流式 API，理由一行）

**Interfaces:**
- Consumes: 既有的 `s_ready`、`s_mtx`、`MOUNT_POINT`（`"/data"`）
- Produces:
  - `void storage_ui_prepare(void)`
  - `esp_err_t storage_ui_stat(uint32_t *out_crc, size_t *out_len)` — `ESP_OK` 表示磁盘上有一个可用的 gzip；`ESP_ERR_NOT_FOUND` 表示没有
  - `esp_err_t storage_ui_remove(void)`
  - `esp_err_t storage_ui_read_begin(void)` / `int storage_ui_read_chunk(uint8_t *buf, size_t cap)`（`>0` 字节数、`0` EOF、`<0` 错误）/ `void storage_ui_read_end(void)`
  - `esp_err_t storage_ui_upload_begin(void)` / `esp_err_t storage_ui_upload_chunk(const void *buf, size_t len)` / `esp_err_t storage_ui_upload_commit(void)` / `void storage_ui_upload_abort(void)`

> 为什么不是 spec 里的 `storage_ui_write(const void*, size_t)`：64 KB 请求体要一次性进 RAM，
> 而设备只剩 174 KB 堆、httpd 还有 7 KB 栈。改成 4 KB 块的会话式 API，读写都不占大块内存。
> 这是对 spec §2 的唯一偏离，顺手把 spec 改成一致的。

- [ ] **Step 1: 在 `main/storage.h` 追加声明**

```c
/* ---------------- uploaded SPA (index.html.gz) ----------------
 * The firmware embeds a copy; this is an optional override that lives in the FATFS partition
 * so the UI can be replaced without reflashing. Everything here is best-effort: on any error
 * the caller falls back to the embedded blob. */
#define UI_MIN_BYTES  1024
#define UI_MAX_BYTES  (64 * 1024)

void      storage_ui_prepare(void);
esp_err_t storage_ui_stat(uint32_t *out_crc, size_t *out_len);
esp_err_t storage_ui_remove(void);

esp_err_t storage_ui_read_begin(void);
int       storage_ui_read_chunk(uint8_t *buf, size_t cap);
void      storage_ui_read_end(void);

esp_err_t storage_ui_upload_begin(void);
esp_err_t storage_ui_upload_chunk(const void *buf, size_t len);
esp_err_t storage_ui_upload_commit(void);
void      storage_ui_upload_abort(void);
```

- [ ] **Step 2: 在 `main/storage.c` 顶部补 include**

```c
#include <errno.h>
#include <sys/stat.h>
#include "esp_rom_crc.h"
```

> **Superseded by commit `3ec17cf`** — the snippets in this task are the first draft. Review found six defects in them and the merged code differs in five places: the temp file is `ui.tmp` (a leading dot is not a valid FAT 8.3 name), `upload_chunk` caps the session at `UI_MAX_BYTES`, a per-session `s_ui_up_ok` flag gates `upload_commit`, `stat` treats `ferror` as absent, and `prepare` removes a stale temp file. Read `main/storage.c` as the source of truth; only re-apply these snippets if you are rebuilding the section from scratch.

- [ ] **Step 3: 在 `main/storage.c` 末尾实现**

```c
/* ------------------------------ uploaded SPA ------------------------------ */

#define UI_DIR   MOUNT_POINT "/www"
#define UI_FILE  UI_DIR "/index.html.gz"
#define UI_TMP   UI_DIR "/.ui.tmp"

static SemaphoreHandle_t s_ui_mtx;    /* one upload and one reader at a time */
static FILE *s_ui_up;
static FILE *s_ui_rd;

void storage_ui_prepare(void)
{
    if (!s_ready) return;
    if (!s_ui_mtx) s_ui_mtx = xSemaphoreCreateMutex();
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

void storage_ui_read_end(void)
{
    if (s_ui_rd) { fclose(s_ui_rd); s_ui_rd = NULL; }
    if (s_ui_mtx) xSemaphoreGive(s_ui_mtx);
}

esp_err_t storage_ui_upload_begin(void)
{
    if (!s_ready || !s_ui_mtx) return ESP_ERR_INVALID_STATE;
    /* non-blocking: a second concurrent upload is a client bug, not something to queue */
    if (xSemaphoreTake(s_ui_mtx, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    s_ui_up = fopen(UI_TMP, "wb");
    if (!s_ui_up) { xSemaphoreGive(s_ui_mtx); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t storage_ui_upload_chunk(const void *buf, size_t len)
{
    if (!s_ui_up || !buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (fwrite(buf, 1, len, s_ui_up) != len) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t storage_ui_upload_commit(void)
{
    if (!s_ui_up) return ESP_ERR_INVALID_STATE;
    if (fflush(s_ui_up) != 0 || fclose(s_ui_up) != 0) { s_ui_up = NULL; remove(UI_TMP); xSemaphoreGive(s_ui_mtx); return ESP_FAIL; }
    s_ui_up = NULL;
    esp_err_t ret = rename(UI_TMP, UI_FILE) == 0 ? ESP_OK : ESP_FAIL;
    if (ret != ESP_OK) remove(UI_TMP);
    xSemaphoreGive(s_ui_mtx);
    return ret;
}

void storage_ui_upload_abort(void)
{
    if (s_ui_up) { fclose(s_ui_up); s_ui_up = NULL; }
    remove(UI_TMP);
    if (s_ui_mtx) xSemaphoreGive(s_ui_mtx);
}
```

- [ ] **Step 4: 在 `storage_init()` 的 `s_ready = true;` 之后调用准备函数**

```c
    s_ready = true;
    storage_ui_prepare();       /* /data/www: the optional uploaded SPA */
```

- [ ] **Step 5: 用下面的段落替换 spec §2 的代码块与相邻一行**

````markdown
存储层新增（`storage.h`）—— **流式会话式**，因为 64 KB 的请求体不该一次性进 RAM：

```c
void      storage_ui_prepare(void);                                  /* mkdir /data/www */
esp_err_t storage_ui_stat(uint32_t *out_crc, size_t *out_len);       /* ESP_OK = 可用 gzip */
esp_err_t storage_ui_remove(void);

esp_err_t storage_ui_read_begin(void);                               /* 发送用，独占 */
int       storage_ui_read_chunk(uint8_t *buf, size_t cap);
void      storage_ui_read_end(void);

esp_err_t storage_ui_upload_begin(void);                             /* 上传用，独占 */
esp_err_t storage_ui_upload_chunk(const void *buf, size_t len);
esp_err_t storage_ui_upload_commit(void);                            /* fclose + rename */
void      storage_ui_upload_abort(void);                             /* fclose + unlink .tmp */
```
````

- [ ] **Step 6: 推送并在 CI 上验证编译**

```bash
git add main/storage.c main/storage.h docs/superpowers/specs/2026-11-20-ui-decoupling-design.md
git commit -m "feat(storage): keep an optional uploaded SPA in /data/www"
git push origin feat/usb-console
```

Expected: CI 四个 job 全绿（`Frontend syntax` / `esp32c3` / `esp32c6 board 1` / `esp32c6 board 2`）。

---

### Task 2: `h_index` 两态 + 分块发送

**Files:**
- Modify: `main/web_server.c:143-172`（整个 `h_index` 替换）

**Interfaces:**
- Consumes: `storage_ui_stat()`、`storage_ui_read_begin/chunk/end`（Task 1）
- Produces: 无（对浏览器是同一个 `/`）

- [ ] **Step 1: 确认 `main/web_server.c` 已经 `#include "storage.h"`**（本任务只用到
  `storage_ui_stat/read_begin/read_chunk/read_end`，CRC 在存储层算，这里不需要 `esp_rom_crc.h`）

- [ ] **Step 2: 替换 `h_index`**

```c
/* Two sources, fixed order: the uploaded file when it is usable, otherwise the blob that
 * lives in the firmware. The embedded copy is never removed, so a truncated upload, a failed
 * mount or `records format` can only cost the override -- never the ability to open the page. */
static esp_err_t h_index(httpd_req_t *req)
{
    static const char build_etag[] = "\"" APP_UI_ETAG "\"";
    char etag[24];
    char inm[48];
    bool fresh = httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK;
    const uint8_t *blob = index_html_gz_start;
    size_t len = (size_t)(index_html_gz_end - index_html_gz_start);
    uint32_t crc = 0;
    size_t flen = 0;
    bool from_fs = storage_ui_stat(&crc, &flen) == ESP_OK;

    if (from_fs) {
        snprintf(etag, sizeof(etag), "\"ui-%08x\"", (unsigned)crc);
        len = flen;
    } else {
        snprintf(etag, sizeof(etag), "%s", build_etag);
    }

    httpd_resp_set_hdr(req, "ETag", etag);
    /* The uploaded file can be replaced without a reboot, so the browser must revalidate;
     * the embedded one only changes with the firmware, so it can be cached. */
    httpd_resp_set_hdr(req, "Cache-Control", from_fs ? "no-cache" : "public, max-age=86400");
    if (fresh && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    if (!from_fs) return httpd_resp_send(req, (const char *)blob, (ssize_t)len);

    /* stream the file in 4 KB chunks: no whole-page allocation while the AP is serving */
    if (storage_ui_read_begin() != ESP_OK) {
        return httpd_resp_send(req, (const char *)blob, (ssize_t)(index_html_gz_end - index_html_gz_start));
    }
    /* static is safe here: storage_ui_read_begin() is exclusive, so only one request at a
     * time reaches this loop (4 KB on the 7 KB httpd stack would be the alternative). */
    static uint8_t buf[4096];
    int n;
    esp_err_t err = ESP_OK;
    while ((n = storage_ui_read_chunk(buf, sizeof(buf))) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) { err = ESP_FAIL; break; }
    }
    if (n < 0) err = ESP_FAIL;
    storage_ui_read_end();
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);   /* terminate the chunked body */
}
```

- [ ] **Step 3: 确认 `main/web_server.c` 已 include `string.h`（本任务用到 `strcmp`）**，没有就补

- [ ] **Step 4: 推送并验证编译**

```bash
git add main/web_server.c
git commit -m "feat(web): serve the SPA from /data/www when it is valid, embedded otherwise"
git push origin feat/usb-console
```

Expected: CI 全绿。

- [ ] **Step 5: 上板验证（板子在谁那儿就在谁那儿做；先不动文件，验证回落路径）**

```bash
TK=<token>          # 先 POST /api/session {"password":"<AP 密码>"} 取 token
# /api/ui 要鉴权：不带 Authorization 直接 401，而不是"文件不存在"
curl -s -H "Authorization: Bearer $TK" http://192.168.4.1/api/ui   # 期望 {"source":"embedded",...}
curl -s -D - -o /dev/null http://192.168.4.1/ | grep -i "etag\|cache-control\|content-encoding"
# 期望：ETag 为构建常量，Cache-Control: no-cache（无条件）。
# 若看到 max-age=86400，说明固件是加这个特性之前的旧版。
```

---

> **As implemented in `4f5dfe2`:** the read session is opened *before* the ETag is chosen, not after. `storage_ui_stat()` and `storage_ui_read_begin()` each take the mutex with a 500 ms timeout and `stat` releases it in between, so the snippet order below can serve the embedded body under the uploaded file's ETag when a commit lands in that gap. Opening first also means the 304 path must release the session explicitly (`if (from_successful_begin) read_end();`).

### Task 3: 上传/删除/查询端点

**Carried from the Task 2 review (fix these as part of this task):**

- **P1 (brief bug, ruled):** the embedded path sets `Cache-Control: public, max-age=86400`, which
  forbids revalidation — a device that loaded the firmware UI keeps it for a day and never sees an
  upload. Replace the ternary with one unconditional
  `httpd_resp_set_hdr(req, "Cache-Control", "no-cache");` and delete the comment that claims a
  flash invalidates the cache.
- **P2-2:** delete the dead store `len = flen;` in the fs branch (the chunked path never reads it).
- **P2-1 (contract, not code):** `storage_ui_read_begin()` holds the mutex for the whole socket
  transfer, and `storage_ui_upload_begin()` takes it with a zero timeout — so an upload attempted
  while another client is streaming `/` on a slow link returns `ESP_ERR_INVALID_STATE`. It is not
  "invalid payload": answer `409 Conflict` and let the client retry.
- **P2-4 (why Task 1's deferred `storage_ui_remove()` locking matters here):** an unlink can land
  while `s_ui_rd` is open; if FatFs permits unlink-while-open the loop reaches EOF without an error
  and the response ends as a cleanly-terminated **corrupt** page. So `DELETE /api/ui` must take
  `s_ui_mtx` (non-blocking; `409` if a stream is in flight) rather than calling
  `storage_ui_remove()` unlocked. Add the lock inside `storage_ui_remove()` itself.
- **P2-a (mutex contract):** only call `storage_ui_read_end()` / `storage_ui_upload_abort()` after a
  *successful* `*_begin()`; the guard keys on global session state, not on the caller's identity.

**Files:**
- Modify: `main/web_server.c`（`h_profiles_get` 之前插入三个 handler；`REG` 表尾追加三行）

**Interfaces:**
- Consumes: `storage_ui_upload_begin/chunk/commit/abort`、`storage_ui_remove`、`storage_ui_stat`、`storage_ui_read_begin/end`（Task 1）
- Produces: `GET/POST/DELETE /api/ui`

- [ ] **Step 1: 插入 handler**

```c
/* ---------------------------- uploaded SPA API ---------------------------- */

static void ui_drain(httpd_req_t *req, size_t want)
{
    /* An error response leaves the body unread; on a keep-alive socket that would desync the
     * next request, so swallow it in 512 B steps. */
    uint8_t junk[512];
    while (want > 0) {
        int r = httpd_req_recv(req, (char *)junk, want > sizeof(junk) ? sizeof(junk) : want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT || r <= 0) break;
        want -= (size_t)r;
    }
}

static esp_err_t h_ui_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    uint32_t crc = 0;
    size_t len = 0;
    bool fs = storage_ui_stat(&crc, &len) == ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "source", fs ? "fs" : "embedded");
    if (fs) {
        cJSON_AddNumberToObject(o, "size", (double)len);
        char e[24];
        snprintf(e, sizeof(e), "ui-%08x", (unsigned)crc);
        cJSON_AddStringToObject(o, "etag", e);
    }
    return send_json(req, o);
}

static esp_err_t h_ui_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");

    int total = req->content_len;
    if (total < UI_MIN_BYTES) { ui_drain(req, (size_t)total); return fail(req, "400 Bad Request", "body smaller than 1 KB"); }
    if (total > UI_MAX_BYTES) { ui_drain(req, (size_t)total); return fail(req, "413 Payload Too Large", "body larger than 64 KB"); }
    if (!storage_ready()) return fail(req, "503 Service Unavailable", "archive unavailable");

    esp_err_t e = storage_ui_upload_begin();
    if (e != ESP_OK) {
        ui_drain(req, (size_t)total);
        return fail(req, "409 Conflict", "an upload is already in progress");
    }

    uint8_t buf[4096];
    int got = 0;
    bool gz = false;
    while (got < total) {
        int want = total - got;
        if (want > (int)sizeof(buf)) want = (int)sizeof(buf);
        int r = httpd_req_recv(req, (char *)buf, (size_t)want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { storage_ui_upload_abort(); return fail(req, "400 Bad Request", "short body"); }
        if (got == 0) gz = (r >= 2 && buf[0] == 0x1f && buf[1] == 0x8b);
        if (got == 0 && !gz) { storage_ui_upload_abort(); ui_drain(req, (size_t)(total - r)); return fail(req, "400 Bad Request", "not gzip"); }
        if (storage_ui_upload_chunk(buf, (size_t)r) != ESP_OK) {
            storage_ui_upload_abort();
            ui_drain(req, (size_t)(total - got - r));
            return fail(req, "500 Internal Server Error", "write failed");
        }
        got += r;
    }

    if (storage_ui_upload_commit() != ESP_OK) return fail(req, "500 Internal Server Error", "commit failed");

    uint32_t crc = 0;
    size_t len = 0;
    storage_ui_stat(&crc, &len);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "size", (double)len);
    char et[24];
    snprintf(et, sizeof(et), "ui-%08x", (unsigned)crc);
    cJSON_AddStringToObject(o, "etag", et);
    return send_json(req, o);
}

static esp_err_t h_ui_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    storage_ui_remove();
    return ok_msg(req, "reverted to the embedded UI");
}
```

- [ ] **Step 2: 注册路由（`REG("/api/factory", …)` 之后）**

```c
    REG("/api/ui",             HTTP_GET,    h_ui_get);
    REG("/api/ui",             HTTP_POST,   h_ui_post);
    REG("/api/ui",             HTTP_DELETE, h_ui_delete);
```

- [ ] **Step 3: 推送并验证编译**

```bash
git add main/web_server.c
git commit -m "feat(web): POST/DELETE /api/ui to replace or revert the SPA"
git push origin feat/usb-console
```

Expected: CI 全绿。

- [ ] **Step 4: 上板验证（需要 token：`POST /api/session {"password":"<AP密码>"}`）**

```bash
TK=<token>
gzip -9c main/www/index.html > ui.gz          # Windows 用 python 代替，见 Task 4
curl -s -H "Authorization: Bearer $TK" -H "Expect:" --data-binary @ui.gz \
     -H "Content-Type: application/gzip" http://192.168.4.1/api/ui
# 期望 {"ok":true,"size":15662,"etag":"ui-xxxxxxxx"}
curl -s -H "Authorization: Bearer $TK" http://192.168.4.1/api/ui
# 期望 {"source":"fs", ...}
curl -s -D - -o /dev/null http://192.168.4.1/ | grep -i "etag\|cache-control"
# 期望 ETag: "ui-xxxxxxxx" 且 Cache-Control: no-cache
printf 'hello' | curl -s -H "Authorization: Bearer $TK" -H "Expect:" --data-binary @- \
     -H "Content-Type: application/gzip" http://192.168.4.1/api/ui
# 期望 400 "body smaller than 1 KB"
head -c 3000 /dev/zero | curl -s -H "Authorization: Bearer $TK" -H "Expect:" --data-binary @- \
     -H "Content-Type: application/gzip" http://192.168.4.1/api/ui
# 期望 400 "not gzip"
curl -s -X DELETE -H "Authorization: Bearer $TK" http://192.168.4.1/api/ui
curl -s -H "Authorization: Bearer $TK" http://192.168.4.1/api/ui
# 期望回到 {"source":"embedded"}
```

> 两个 curl 注意事项（P2-8，已踩过）：`-d/--data-binary` 默认带 `Expect: 100-continue`，而
> esp_http_server 不应答它，上传会白等约 1 秒 —— 一律加 `-H "Expect:"`；`/` 只注册了
> `HTTP_GET`，所以看响应头要用 `curl -s -D - -o /dev/null`（`curl -I` 会 404）。
>
> P1-1 的验收：上传限速后中途 Ctrl-C，然后立刻请求 `/` 必须马上有响应（修复前会卡到重启）。
>
> ```bash
> curl --limit-rate 4k -H "Authorization: Bearer $TK" -H "Expect:" --data-binary @ui.gz \
>      -H "Content-Type: application/gzip" http://192.168.4.1/api/ui   # 中途 Ctrl-C
> curl -s -D - -o /dev/null http://192.168.4.1/                        # 必须立即响应
> ```

---

### Task 4: 上传工具与文档

**Files:**
- Create: `tools/ui_push.py`
- Modify: `docs/usb-console.md`（命令表之后补一节 "Uploading a new UI"）
- Modify: `README.md`（文档索引补一行）

**Interfaces:**
- Consumes: `POST /api/session`、`POST /api/ui`、`DELETE /api/ui`、`GET /api/ui`（Task 3）
- Produces: `python tools/ui_push.py --host H --pass P FILE` / `--revert` / `--status`

> **As merged in `09f436b`:** the snippet below is the sketch, not the tool. The shipped version
> also handles `URLError` (clean message, exit 1, no traceback), exits non-zero on `ok:false`,
> prints the source before and after an upload, and refuses a file whose gzip falls outside
> 1..64 KB before touching the network. Read `tools/ui_push.py` as the source of truth.

- [ ] **Step 1: 写 `tools/ui_push.py`**

```python
#!/usr/bin/env python3
"""Replace the SPA on a running board without reflashing it.

    python tools/ui_push.py --pass <AP password> main/www/index.html
    python tools/ui_push.py --pass <AP password> --status
    python tools/ui_push.py --pass <AP password> --revert

The file is gzipped here (Windows has no gzip CLI) because the device never inflates it.
"""
import argparse
import gzip
import json
import pathlib
import sys
import urllib.error
import urllib.request

MAX_BYTES = 64 * 1024
MIN_BYTES = 1024


def call(base, path, method="GET", body=None, token=None, ctype="application/json"):
    req = urllib.request.Request(base + path, method=method, data=body)
    if body is not None:
        req.add_header("Content-Type", ctype)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=45) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return json.loads(raw)
        except ValueError:
            return {"ok": False, "err": "HTTP %d %s" % (e.code, raw[:80])}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", help="index.html to upload")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--pass", dest="password", required=True, help="SoftAP password or API token")
    ap.add_argument("--revert", action="store_true", help="drop the uploaded UI, back to the firmware copy")
    ap.add_argument("--status", action="store_true", help="report which copy is being served")
    args = ap.parse_args()

    base = "http://" + args.host
    sess = call(base, "/api/session", "POST", json.dumps({"password": args.password}).encode())
    token = sess.get("token")
    if not token:
        sys.exit("no token: " + str(sess.get("err") or sess))

    if args.status:
        print(json.dumps(call(base, "/api/ui", token=token), indent=2))
        return
    if args.revert:
        print(json.dumps(call(base, "/api/ui", "DELETE", token=token), indent=2))
        return
    if not args.file:
        sys.exit("nothing to do: pass a file, --status or --revert")

    raw = pathlib.Path(args.file).read_bytes()
    blob = gzip.compress(raw, 9)
    print("raw %d -> gzip %d bytes" % (len(raw), len(blob)))
    if not (MIN_BYTES <= len(blob) <= MAX_BYTES):
        sys.exit("refusing: gzip must be %d..%d bytes" % (MIN_BYTES, MAX_BYTES))

    out = call(base, "/api/ui", "POST", blob, token=token, ctype="application/gzip")
    print(json.dumps(out, indent=2))
    if out.get("ok"):
        print("hard-reload the page (Ctrl-F5) to pick it up")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: 本地自检（不需要板子）**

```bash
python tools/ui_push.py --help
python tools/ui_push.py --pass x main/www/index.html --host 127.0.0.1
```

Expected: `--help` 打印用法；第二条打印 `raw 50478 -> gzip ~15662 bytes` 后以 `no token:`/连接错误退出（0.0.0.0 上没人听），**不抛 traceback**。

- [ ] **Step 3: 文档**

`docs/usb-console.md` 末尾追加：

```markdown
## Uploading a new UI

The SPA normally comes from the firmware image. To replace it without reflashing:

```bash
python tools/ui_push.py --pass <AP password> main/www/index.html   # upload
python tools/ui_push.py --pass <AP password> --status              # which copy is served
python tools/ui_push.py --pass <AP password> --revert              # back to the embedded one
```

The body must be gzip, 1..64 KB; the device never inflates it. The embedded copy stays in
the firmware as the fallback, so a broken upload -- or `records format`, which wipes the
whole FATFS partition -- cannot leave the page blank.
```

`README.md` 文档索引补：

```
tools/ui_push.py        上传新 UI（gzip → /data/www），无需重刷固件
```

- [ ] **Step 4: 推送**

```bash
git add tools/ui_push.py docs/usb-console.md README.md
git commit -m "tools: ui_push.py to replace the SPA over the AP"
git push origin feat/usb-console
```

- [ ] **Step 5: 上板端到端**

```bash
python tools/ui_push.py --pass <AP密码> main/www/index.html
python tools/ui_push.py --pass <AP密码> --status      # source: fs, size ~15662
python tools/ui_push.py --pass <AP密码> --revert
python tools/ui_push.py --pass <AP密码> --status      # source: embedded
```

Expected: 上传后浏览器硬刷新看到同一份 UI（ETag 变成 `ui-…`）；`--revert` 后 ETag 回到构建常量。

---

## 回归检查（全部任务完成后）

- [ ] `node tools/check_i18n.js` 通过（gzip 体积行应与 Task 4 的 `size` 一致）
- [ ] `node tools/test_mfg.js` 通过
- [ ] 归档/收藏/profile 的增删改不受影响（`records`、`pins`、`POST /api/profiles` 各做一次）
- [ ] `records format`（USB 命令台）后 `GET /api/ui` 回到 `embedded`，页面仍能打开
- [ ] CI 四个 job 绿，`firmware/*.bin` 只在最后统一刷新一次