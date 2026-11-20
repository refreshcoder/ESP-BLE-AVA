# UI 与固件解耦（spec）

状态：待评审 · 2026-11-20 · 分支 `feat/usb-console`

## 1. 目标

改一次网页 UI 不再需要重编固件、重刷 1.2 MB、重启、重连热点。

今天 UI 是 `EMBED_FILES` 进 app 的 gzip blob，改动必须走 CI + esptool + 重启。目标是让 UI
可以单独上传、即时生效，并且**任何失败都不会让页面打不开**。

### 非目标

- 不新建 littlefs 分区（复用现有 FATFS，避免动分区表和全量重刷）。
- 不拆分静态资源（仍是单个 `index.html`），不做前端构建链。
- 不做 UI 版本回滚管理（只有"当前文件 / 内置兜底"两态）。

## 2. 存储

复用 `main/storage.c` 已挂载的 FATFS（`MOUNT_POINT "/data"`，分区 `storage` 832 KB，带
wear levelling）。新增：

```
/data/www/index.html.gz     ← 唯一的上传产物，原样 gzip，不在设备上解压
```

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

约束：

- **只接受 gzip**（首两字节 `1f 8b`），因为服务端不做压缩也不解压；
- 大小上限 **64 KB**（实测内置版 15.7 KB，留 4 倍余量；`/data` 那 832 KB 还要和
  归档 21.5 KB + 收藏 5.4 KB 共用，静态资源不该占住 1/3），下限 1 KB（挡住空文件）；
- 写入走 `.tmp` + `rename`，绝不出现半截文件；
- `storage_init()` 挂载成功后 `mkdir("/data/www")`，失败只记日志（UI 走内置兜底）。

## 3. 服务

`h_index` 改为两态，**顺序固定**：

1. 尝试 `storage_ui_read()`：拿到 `>=1 KB` 且首两字节 `1f 8b` → 用它；
   - `ETag: "ui-<crc32>"`（`esp_rom_crc32_le`），`Cache-Control: no-cache`
     （上传后必须让浏览器重新校验，304 很便宜）；
2. 否则回落到内置 `index_html_gz`：
   - `ETag: "<APP_UI_ETAG>"`（构建常量），`Cache-Control: public, max-age=86400`（维持现状）。

两种情况都支持 `If-None-Match` → 304，都设 `Content-Encoding: gzip`。

发送用 `httpd_resp_send_chunk` **分块流式**（4 KB/块），不为整页分配堆：FATFS 读一块发一块。

## 4. 端点

| 方法 | 路径 | 作用 |
|---|---|---|
| `POST` | `/api/ui` | body = 原始 gzip 字节流（`Content-Type: application/gzip`）；校验魔数+大小 → 落盘 → 返回 `{ok, size, etag}` |
| `DELETE` | `/api/ui` | 删除上传文件，回到内置版本；返回 `{ok, source:"embedded"}` |
| `GET` | `/api/ui` | `{ok, source:"fs"\|"embedded", size, etag}`，用于确认当前生效的是哪个 |

全部要求 `auth_ok()`（`Authorization: Bearer <token>`，与其它端点一致）。

错误码：

| 情况 | 返回 |
|---|---|
| 无 token | `401` |
| body 为空 / <1 KB | `400 small body` |
| 不是 gzip（无 `1f 8b`） | `400 not gzip` |
| >64 KB | `413 too large` |
| 归档分区没挂载 | `503 archive unavailable` |
| 写盘失败 | `500 write failed` |

`records format`（USB 命令台）会格式化整个 FATFS → UI 文件随之消失，**自动回落内置版**，
所以这不是丢功能的路径，只是丢一次自定义。

## 5. 上传流程（PC 侧）

`tools/ui_push.py`（新文件，约 40 行）：

```
python tools/ui_push.py --host 192.168.4.1 --pass beaconxxxx main/www/index.html
```

做的事：`gzip -9` → `POST /api/session` 取 token（用 AP 密码）→ `POST /api/ui` 传字节流 →
打印返回的 etag；附加 `--revert` 走 `DELETE /api/ui`。

Windows 没有 gzip CLI，所以用 Python 的 `gzip` 模块，不依赖外部工具。

## 6. 失败与安全

- 上传中途断电：只有 `.tmp` 被截断，旧的 `index.html.gz` 仍完整 → 页面照常；替换路径在 remove+rename 之间有毫秒级窗口，期间断电只会回落到内置版；
- 上传前客户端先量一次 gz 体积，超限直接给笑脸错误，不浪费一次 AP 往返；
- 上传了坏 gzip：设备不解压，浏览器收到坏内容，**下一次 `DELETE /api/ui` 或 `records format`
  即可恢复**（内置兜底永远在固件里）；
- gzip 炸弹：设备从不 gunzip，风险为零（体积上限只约束 flash 占用）；
- 鉴权：与其它端点同一 token；热点本身是 WPA2。

## 7. 影响面

| 文件 | 改动 |
|---|---|
| `main/storage.c` / `storage.h` | 三个 UI 文件函数 + 启动建目录（~70 行） |
| `main/web_server.c` | `h_index` 两态 + 流式发送；`h_ui_get/post/delete` + 注册（~120 行） |
| `tools/ui_push.py` | 新增（~40 行） |
| `docs/usb-console.md`、`README.md` | 命令/端点表补一行 |
| `firmware/*.bin` | 无变化（UI 大小不再影响固件体积上限） |

不需要改：分区表、NVS、`CMakeLists.txt` 的 `EMBED_FILES`（内置版保留作为兜底）。

## 8. 验证

无法在本地跑 ESP-IDF，验收分层：

1. **CI 编译**：三个目标全绿（保证 `storage_ui_*`、`httpd_resp_send_chunk` 用法正确）；
2. **上板手工**（板子插在哪台机器上就在哪台做，全程 USB/热点皆可）：
   - `GET /api/ui` → `source:"embedded"`；
   - `ui_push.py` 上传 → 返回 `etag:"ui-xxxx"`，浏览器硬刷新后 `GET /` 的响应头
     `Cache-Control: no-cache` 且内容是**新** UI；
   - `curl -sI` 两次：第二次带 `If-None-Match` 应回 `304`；
   - `DELETE /api/ui` → `GET /api/ui` 回到 `embedded`，页面仍是旧（内置）版；
   - 故意上传非 gzip（`curl -d 'hello'`）→ `400 not gzip`；
   - `records format` 后再 `GET /api/ui` → `embedded`（UI 文件被格式化掉）；
3. **回归**：记录/收藏/profile 相关功能不受影响（存储层只加了文件名，没动 record 结构）。