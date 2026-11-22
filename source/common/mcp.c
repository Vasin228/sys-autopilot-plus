#include "mcp.h"
#include "apiargs.h"
#include "base64.h"
#include "config.h"
#include "files.h"
#include "input.h"
#include "json.h"
#include "oauth.h"
#include "power.h"
#include "jstream.h"
#include "routes.h"
#include "screen.h"
#include "settings.h"
#include "titles.h"
#include "process.h"
#include "diagnostics.h"
#include "telemetry.h"
#include "network.h"
#include "memory.h"
#include "log.h"
#include "scratch.h"
#include "mcp_tools.h"

#include <ctype.h>
#include <errno.h>
#include <strings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Cap for the *reduced* JSON-RPC document (everything except a streamed
// upload_file content field, which goes straight to disk).
#define MCP_DOC_MAX 16384

// Temp destination for streamed upload content (renamed into place on
// success, deleted otherwise).
#ifndef MCP_UPLOAD_TMP
#define MCP_UPLOAD_TMP CONFIG_DIR "/.upload.tmp"
#endif

#define MCP_PROTO_LATEST "2025-06-18"

#define READ_FILE_MAX 32768
#define TAP_SEQ_MAX 32

// --- upload content sink -------------------------------------------------------

typedef struct {
    FILE *f;
    B64Decoder dec;
    size_t bytes;
    bool failed;
} UploadCtx;

static UploadCtx g_upload;

// Note: failures (bad base64, write error) latch u->failed but return 0 so
// the rest of the request still streams through and the JSON-RPC layer can
// report a proper in-band tool error instead of a protocol error.
static int upload_sink(const char *data, size_t len, void *ctx) {
    UploadCtx *u = ctx;
    static uint8_t decbuf[0x8000];
    if (u->failed)
        return 0; // keep draining
    if (!u->f) {
        files_mkdirs_for(MCP_UPLOAD_TMP);
        u->f = fopen(MCP_UPLOAD_TMP, "wb");
        if (!u->f) {
            u->failed = true;
            return 0;
        }
    }
    while (len > 0) {
        // decbuf holds the decode of up to 0xA000 input chars; bound input so
        // (n/4+1)*3 always fits.
        size_t n = len > 0xA000 ? 0xA000 : len;
        ssize_t dn = b64dec_update(&u->dec, data, n, decbuf);
        if (dn < 0 || fwrite(decbuf, 1, (size_t)dn, u->f) != (size_t)dn) {
            u->failed = true;
            return 0;
        }
        u->bytes += (size_t)dn;
        data += n;
        len -= n;
    }
    return 0;
}

// Closes and removes any leftover upload temp state. No-op when nothing was
// streamed (the common case), so non-upload requests never touch the SD card.
static void upload_cleanup(void) {
    if (g_upload.f)
        fclose(g_upload.f);
    if (g_upload.f || g_upload.bytes > 0 || g_upload.failed)
        remove(MCP_UPLOAD_TMP);
    memset(&g_upload, 0, sizeof(g_upload));
}

// --- response helpers ----------------------------------------------------------

// Sends {"jsonrpc":"2.0","id":<id>,"<key>":<value>} with exact Content-Length.
static void send_rpc_value(int fd, const char *id, const char *key,
                           const char *value, size_t value_len) {
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"%s\":",
                      id, key);
    http_send_header(fd, 200, "application/json", (size_t)hn + value_len + 1);
    http_write_all(fd, head, (size_t)hn);
    http_write_all(fd, value, value_len);
    http_write_all(fd, "}", 1);
}

static void send_rpc_error(int fd, const char *id, int code, const char *msg) {
    char val[256];
    int n = snprintf(val, sizeof(val), "{\"code\":%d,\"message\":\"%s\"}", code, msg);
    send_rpc_value(fd, id, "error", val, (size_t)n);
}

// Sends a result of the form <pre><payload><post> (payload may be huge).
static void send_result_stream(int fd, const char *id, const char *pre,
                               const char *payload, size_t payload_len,
                               const char *post) {
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id);
    size_t total = (size_t)hn + strlen(pre) + payload_len + strlen(post) + 1;
    http_send_header(fd, 200, "application/json", total);
    http_write_all(fd, head, (size_t)hn);
    http_write_all(fd, pre, strlen(pre));
    if (payload_len > 0)
        http_write_all(fd, payload, payload_len);
    http_write_all(fd, post, strlen(post));
    http_write_all(fd, "}", 1);
}

#define TEXT_PRE  "{\"content\":[{\"type\":\"text\",\"text\":\""
#define TEXT_POST_OK  "\"}],\"isError\":false}"
#define TEXT_POST_ERR "\"}],\"isError\":true}"

// Tool result with plain (unescaped) text content.
static void send_tool_text(int fd, const char *id, bool is_error, const char *text) {
    size_t tlen = strlen(text);
    size_t esc_len = json_escaped_len(text, tlen);
    char *escaped = malloc(esc_len + 1);
    if (!escaped) {
        send_rpc_error(fd, id, -32603, "out of memory");
        return;
    }
    json_escape(text, tlen, escaped, esc_len + 1);
    send_result_stream(fd, id, TEXT_PRE, escaped, esc_len,
                       is_error ? TEXT_POST_ERR : TEXT_POST_OK);
    free(escaped);
}

static void send_tool_ok(int fd, const char *id, const char *text) {
    send_tool_text(fd, id, false, text);
}

static void send_tool_error(int fd, const char *id, const char *text) {
    send_tool_text(fd, id, true, text);
}

static void send_input_tool_result(int fd, const char *id, Result rc, const char *ok_msg) {
    if (R_FAILED(rc)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "input failed (rc=0x%x)", rc);
        send_tool_error(fd, id, msg);
    } else {
        send_tool_ok(fd, id, ok_msg);
    }
}

static int get_int_or(const JsonDoc *doc, int obj, const char *key, int fallback);

// Streams a result whose content is [text, image] (the screenshot-after-input
// case). text must not require JSON escaping (static messages only).
static void send_tool_text_and_image(int fd, const char *id, const char *text,
                                     const u8 *jpeg, u64 jpeg_size) {
    static const char img_pre[] = "\"},{\"type\":\"image\",\"data\":\"";
    static const char img_post[] = "\",\"mimeType\":\"image/jpeg\"}],\"isError\":false}";
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id);
    size_t b64len = b64_encoded_len((size_t)jpeg_size);
    size_t total = (size_t)hn + sizeof(TEXT_PRE) - 1 + strlen(text) +
                   sizeof(img_pre) - 1 + b64len + sizeof(img_post) - 1 + 1;

    http_send_header(fd, 200, "application/json", total);
    http_write_all(fd, head, (size_t)hn);
    http_write_all(fd, TEXT_PRE, sizeof(TEXT_PRE) - 1);
    http_write_all(fd, text, strlen(text));
    http_write_all(fd, img_pre, sizeof(img_pre) - 1);

    static char enc[4096];
    for (size_t off = 0; off < jpeg_size; off += 3072) {
        size_t chunk = jpeg_size - off > 3072 ? 3072 : (size_t)(jpeg_size - off);
        size_t n = b64_encode(jpeg + off, chunk, enc);
        if (!http_write_all(fd, enc, n))
            return;
    }

    http_write_all(fd, img_post, sizeof(img_post) - 1);
    http_write_all(fd, "}", 1);
}

// Input result with an optional trailing screenshot ({"screenshot":true} in
// the tool arguments), saving the agent a separate screenshot round trip.
static void send_input_result_opt_screenshot(HttpRequest *req, const char *id,
                                             Result rc, const char *ok_msg,
                                             const JsonDoc *doc, int args) {
    bool want = false;
    int tok = json_obj_get(doc, args, "screenshot");
    if (tok >= 0)
        json_get_bool(doc, tok, &want);

    if (R_FAILED(rc) || !want) {
        send_input_tool_result(req->fd, id, rc, ok_msg);
        return;
    }

    int delay = get_int_or(doc, args, "screenshotDelayMs", 250);
    if (delay < 0) delay = 0;
    if (delay > INPUT_MAX_DURATION_MS) delay = INPUT_MAX_DURATION_MS;
    if (delay > 0)
        svcSleepThread((s64)delay * 1000000LL);

    const u8 *jpeg = NULL;
    u64 size = 0;
    Result cap_rc = screen_capture_jpeg(ViLayerStack_Screenshot, &jpeg, &size);
    if (R_FAILED(cap_rc)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s (screenshot failed: rc=0x%x)", ok_msg, cap_rc);
        send_tool_ok(req->fd, id, msg);
        return;
    }
    send_tool_text_and_image(req->fd, id, ok_msg, jpeg, size);
}

// --- tools -----------------------------------------------------------------------

static void tool_screenshot(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    ViLayerStack stack = ViLayerStack_Screenshot;
    char val[24];
    int tok = json_obj_get(doc, args, "stack");
    if (tok >= 0 && json_get_string(doc, tok, val, sizeof(val)) &&
        !screen_parse_stack(val, &stack)) {
        send_tool_error(req->fd, id, "invalid 'stack'");
        return;
    }

    const u8 *jpeg = NULL;
    u64 size = 0;
    Result rc = screen_capture_jpeg(stack, &jpeg, &size);
    if (R_FAILED(rc)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "capture failed (rc=0x%x)", rc);
        send_tool_error(req->fd, id, msg);
        return;
    }

    static const char pre[] = "{\"content\":[{\"type\":\"image\",\"data\":\"";
    static const char post[] = "\",\"mimeType\":\"image/jpeg\"}],\"isError\":false}";
    char head[96];
    int hn = snprintf(head, sizeof(head), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id);
    size_t b64len = b64_encoded_len((size_t)size);
    size_t total = (size_t)hn + sizeof(pre) - 1 + b64len + sizeof(post) - 1 + 1;

    http_send_header(req->fd, 200, "application/json", total);
    http_write_all(req->fd, head, (size_t)hn);
    http_write_all(req->fd, pre, sizeof(pre) - 1);

    // Encode in multiples of 3 so chunk concatenation equals whole-buffer
    // encoding (padding only on the final chunk).
    static char enc[4096];
    for (size_t off = 0; off < size; off += 3072) {
        size_t chunk = size - off > 3072 ? 3072 : (size_t)(size - off);
        size_t n = b64_encode(jpeg + off, chunk, enc);
        if (!http_write_all(req->fd, enc, n))
            return;
    }

    http_write_all(req->fd, post, sizeof(post) - 1);
    http_write_all(req->fd, "}", 1);
}

static void tool_tap_buttons(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    uint64_t mask;
    const char *err = NULL;
    if (!args_get_buttons(doc, args, &mask, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    int duration = args_get_duration(doc, args, INPUT_DEFAULT_TAP_MS);
    send_input_result_opt_screenshot(req, id, input_tap(mask, duration), "ok",
                                     doc, args);
}

static int get_int_or(const JsonDoc *doc, int obj, const char *key, int fallback) {
    long long v;
    int tok = json_obj_get(doc, obj, key);
    if (tok >= 0 && json_get_int(doc, tok, &v))
        return (int)v;
    return fallback;
}

static void tool_tap_sequence(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    int taps = json_obj_get(doc, args, "taps");
    int n = json_arr_len(doc, taps);
    if (n <= 0) {
        send_tool_error(req->fd, id, "missing 'taps' (non-empty array)");
        return;
    }
    if (n > TAP_SEQ_MAX) {
        send_tool_error(req->fd, id, "too many taps (max 32)");
        return;
    }
    for (int i = 0; i < n; i++) {
        int el = json_arr_get(doc, taps, i);
        uint64_t mask;
        const char *err = NULL;
        if (!args_get_buttons(doc, el, &mask, &err)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "step %d: %s", i, err);
            send_tool_error(req->fd, id, msg);
            return;
        }
        int duration = args_get_duration(doc, el, INPUT_DEFAULT_TAP_MS);
        Result rc = input_tap(mask, duration);
        if (R_FAILED(rc)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "step %d: input failed (rc=0x%x)", i, rc);
            send_tool_error(req->fd, id, msg);
            return;
        }
        if (i + 1 < n) {
            int delay = get_int_or(doc, el, "delayAfterMs", 150);
            if (delay < 0) delay = 0;
            if (delay > INPUT_MAX_DURATION_MS) delay = INPUT_MAX_DURATION_MS;
            svcSleepThread((s64)delay * 1000000LL);
        }
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "performed %d taps", n);
    send_input_result_opt_screenshot(req, id, 0, msg, doc, args);
}

static void tool_hold_release(HttpRequest *req, const char *id, const JsonDoc *doc,
                              int args, bool hold) {
    uint64_t mask;
    const char *err = NULL;
    if (!args_get_buttons(doc, args, &mask, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    send_input_result_opt_screenshot(req, id,
                                     hold ? input_hold(mask) : input_release(mask),
                                     "ok", doc, args);
}

static void tool_set_stick(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    int side, duration;
    float x, y;
    const char *err = NULL;
    if (!args_get_stick(doc, args, &side, &x, &y, &duration, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    send_input_result_opt_screenshot(req, id, input_stick(side, x, y, duration),
                                     "ok", doc, args);
}

static void tool_touch_screen(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    int x, y, x2, y2, duration;
    const char *err = NULL;
    if (!args_get_touch(doc, args, &x, &y, &x2, &y2, &duration,
                        INPUT_DEFAULT_TAP_MS, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    send_input_result_opt_screenshot(req, id, input_touch(x, y, x2, y2, duration),
                                     "ok", doc, args);
}

static void tool_status(HttpRequest *req, const char *id) {
    u32 ver = hosversionGet();
    char text[256];
    int n = snprintf(text, sizeof(text),
             "version: %s\nfirmware: %u.%u.%u\ncontrollerAttached: %s\nuptimeSeconds: %llu",
             routes_app_version(),
             HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
             input_is_attached() ? "true" : "false",
             (unsigned long long)routes_uptime_seconds());
    uint32_t pct = 0;
    bool charging = false;
    if (n > 0 && (size_t)n < sizeof(text) && settings_get_battery(&pct, &charging))
        snprintf(text + n, sizeof(text) - (size_t)n,
                 "\nbatteryPercent: %u\ncharging: %s",
                 pct, charging ? "true" : "false");
    send_tool_ok(req->fd, id, text);
}

// Extracts the "path" argument and resolves it. Sends a tool error on failure.
static bool get_path_arg(HttpRequest *req, const char *id, const JsonDoc *doc,
                         int args, char *fspath, size_t fspath_sz) {
    char path[512];
    int tok = json_obj_get(doc, args, "path");
    if (tok < 0 || !json_get_string(doc, tok, path, sizeof(path))) {
        send_tool_error(req->fd, id, "missing 'path'");
        return false;
    }
    const char *err = NULL;
    if (!files_resolve(path, fspath, fspath_sz, &err)) {
        send_tool_error(req->fd, id, err);
        return false;
    }
    return true;
}

static void tool_list_directory(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;

    // Trim a trailing slash (but keep the root "sdmc:/").
    size_t plen = strlen(fspath);
    size_t rootlen = strlen(FILES_ROOT) + 1;
    if (plen > rootlen && fspath[plen - 1] == '/')
        fspath[plen - 1] = '\0';

    const char *err = NULL;
    size_t len = 0;
    char *json = files_build_listing(fspath, fspath + strlen(FILES_ROOT), &len, &err);
    if (!json) {
        send_tool_error(req->fd, id, err);
        return;
    }
    size_t esc_len = json_escaped_len(json, len);
    char *escaped = malloc(esc_len + 1);
    if (!escaped) {
        free(json);
        send_rpc_error(req->fd, id, -32603, "out of memory");
        return;
    }
    json_escape(json, len, escaped, esc_len + 1);
    free(json);
    send_result_stream(req->fd, id, TEXT_PRE, escaped, esc_len, TEXT_POST_OK);
    free(escaped);
}

// Replaces invalid UTF-8 sequences with '?' so the output is always a legal
// JSON string payload.
static void sanitize_utf8(char *buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)buf[i];
        size_t extra;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else { buf[i++] = '?'; continue; }

        bool ok = i + extra < len; // full sequence present
        for (size_t k = 1; ok && k <= extra; k++)
            ok = ((unsigned char)buf[i + k] & 0xC0) == 0x80;
        if (ok)
            i += extra + 1;
        else
            buf[i++] = '?';
    }
}

static void tool_read_file(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;

    struct stat st;
    if (stat(fspath, &st) != 0 || S_ISDIR(st.st_mode)) {
        send_tool_error(req->fd, id, "no such file");
        return;
    }

    long long fsize = (long long)st.st_size;
    long long offset = get_int_or(doc, args, "offset", 0);
    long long length = get_int_or(doc, args, "length", READ_FILE_MAX);
    if (offset < 0) {
        offset = fsize + offset;
        if (offset < 0)
            offset = 0;
    }
    if (offset > fsize)
        offset = fsize;
    long long avail = fsize - offset;
    if (length < 0 || length > avail)
        length = avail;
    if (length > READ_FILE_MAX)
        length = READ_FILE_MAX;

    char *data = malloc((size_t)length + 1);
    if (!data) {
        send_rpc_error(req->fd, id, -32603, "out of memory");
        return;
    }
    FILE *f = fopen(fspath, "rb");
    if (!f) {
        free(data);
        send_tool_error(req->fd, id, "open failed");
        return;
    }
    if (offset > 0)
        fseek(f, (long)offset, SEEK_SET);
    size_t got = fread(data, 1, (size_t)length, f);
    fclose(f);

    sanitize_utf8(data, got);

    size_t esc_len = json_escaped_len(data, got);
    char *escaped = malloc(esc_len + 1);
    if (!escaped) {
        free(data);
        send_rpc_error(req->fd, id, -32603, "out of memory");
        return;
    }
    json_escape(data, got, escaped, esc_len + 1);
    free(data);
    send_result_stream(req->fd, id, TEXT_PRE, escaped, esc_len, TEXT_POST_OK);
    free(escaped);
}

static void tool_upload_file(HttpRequest *req, const char *id, const JsonDoc *doc,
                             int args, bool content_streamed) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;

    if (!content_streamed) {
        send_tool_error(req->fd, id, "missing 'content' (base64 string)");
        return;
    }
    if (g_upload.failed || !b64dec_finish(&g_upload.dec)) {
        send_tool_error(req->fd, id, "invalid base64 content");
        return;
    }
    if (g_upload.f) {
        fclose(g_upload.f);
        g_upload.f = NULL;
    } else {
        // Empty base64 string (content: "") — create empty temp file cleanly.
        FILE *ef = fopen(MCP_UPLOAD_TMP, "wb");
        if (!ef) {
            send_tool_error(req->fd, id, "failed to create empty upload file");
            return;
        }
        fclose(ef);
    }

    files_mkdirs_for(fspath);
    remove(fspath); // rename() does not overwrite on all newlib targets
    if (rename(MCP_UPLOAD_TMP, fspath) != 0) {
        send_tool_error(req->fd, id, "failed to move upload into place");
        return;
    }

    char msg[820];
    snprintf(msg, sizeof(msg), "wrote %zu bytes to %s", g_upload.bytes,
             fspath + strlen(FILES_ROOT));
    memset(&g_upload, 0, sizeof(g_upload));
    LOGF("mcp: %s\n", msg);
    send_tool_ok(req->fd, id, msg);
}

static void tool_delete_file(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;
    const char *err = NULL;
    if (!files_delete_path(fspath, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }
    send_tool_ok(req->fd, id, "deleted");
}

// Computes a file's SHA-256, streamed so memory use stays constant regardless
// of file size. With an optional "expected" hash, also reports whether they
// match, so the agent can verify an upload in a single round-trip.
static void tool_hash_file(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char fspath[768];
    if (!get_path_arg(req, id, doc, args, fspath, sizeof(fspath)))
        return;

    char hexbuf[65];
    long long size = 0;
    const char *err = NULL;
    if (!files_hash_sha256(fspath, hexbuf, &size, &err)) {
        send_tool_error(req->fd, id, err);
        return;
    }

    // Optional case-insensitive comparison against an expected digest.
    char expected[80];
    int etok = args >= 0 ? json_obj_get(doc, args, "expected") : -1;
    bool have_expected = etok >= 0 && json_get_string(doc, etok, expected, sizeof(expected));

    char msg[256];
    if (have_expected) {
        bool matched = strcasecmp(expected, hexbuf) == 0;
        snprintf(msg, sizeof(msg),
                 "{\"algorithm\":\"sha256\",\"hash\":\"%s\",\"size\":%lld,\"matched\":%s}",
                 hexbuf, size, matched ? "true" : "false");
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"algorithm\":\"sha256\",\"hash\":\"%s\",\"size\":%lld}",
                 hexbuf, size);
    }
    send_tool_ok(req->fd, id, msg);
}

// Mints a bearer token over the already-authenticated MCP channel so agents
// can use the raw HTTP API (e.g. curl for large file uploads) without being
// given credentials out of band.
static void tool_create_token(HttpRequest *req, const char *id) {
    char token[65];
    if (!oauth_mint_token(token, sizeof(token), "via create_token tool")) {
        send_tool_error(req->fd, id, "failed to persist token");
        return;
    }
    char host[160];
    snprintf(host, sizeof(host), "%s", req->host[0] ? req->host : "<switch-ip>:<port>");
    char text[640];
    snprintf(text, sizeof(text),
             "token: %s\n\n"
             "Use it as a Bearer credential with the raw HTTP API, e.g. to "
             "upload a file:\n"
             "  curl -H 'Authorization: Bearer %s' -T myapp.nro "
             "'http://%s/files?path=/switch/myapp.nro'\n\n"
             "The token does not expire; revoke it by deleting its line from "
             "config/sys-autopilot/tokens.txt on the SD card.",
             token, token, host);
    send_tool_ok(req->fd, id, text);
}

static void tool_revoke_token(HttpRequest *req, const char *id,
                              const JsonDoc *doc, int args) {
    char token[160];
    int tok = json_obj_get(doc, args, "token");
    if (tok < 0 || !json_get_string(doc, tok, token, sizeof(token))) {
        send_tool_error(req->fd, id, "missing 'token'");
        return;
    }
    if (!oauth_revoke_token(token)) {
        send_tool_error(req->fd, id,
                        "unknown token (note: the static config.ini token "
                        "cannot be revoked this way)");
        return;
    }
    LOGF("mcp: revoked a token\n");
    send_tool_ok(req->fd, id, "token revoked");
}

// --- system settings tools ---------------------------------------------------

static void tool_get_theme(HttpRequest *req, const char *id) {
    bool dark = false;
    if (!settings_get_theme(&dark)) {
        send_tool_error(req->fd, id, "failed to read theme");
        return;
    }
    send_tool_ok(req->fd, id, dark ? "dark" : "light");
}

static void tool_set_theme(HttpRequest *req, const char *id,
                           const JsonDoc *doc, int args) {
    char val[16] = {0};
    int t = json_obj_get(doc, args, "theme");
    if (t < 0 || !json_get_string(doc, t, val, sizeof(val))) {
        send_tool_error(req->fd, id, "missing 'theme' (\"light\" or \"dark\")");
        return;
    }
    bool dark;
    if (strcasecmp(val, "dark") == 0)       dark = true;
    else if (strcasecmp(val, "light") == 0) dark = false;
    else { send_tool_error(req->fd, id, "'theme' must be \"light\" or \"dark\""); return; }

    if (!settings_set_theme(dark))
        send_tool_error(req->fd, id, "failed to set theme");
    else
        send_tool_ok(req->fd, id,
                     dark ? "theme set to dark (applies after the HOME menu reloads — "
                            "sleep/wake or reboot)"
                          : "theme set to light (applies after the HOME menu reloads — "
                            "sleep/wake or reboot)");
}

static void tool_get_nickname(HttpRequest *req, const char *id) {
    char name[128] = {0};
    if (!settings_get_nickname(name, sizeof(name)))
        send_tool_error(req->fd, id, "failed to read nickname");
    else
        send_tool_ok(req->fd, id, name);
}

static void tool_set_nickname(HttpRequest *req, const char *id,
                              const JsonDoc *doc, int args) {
    char name[128] = {0};
    int t = json_obj_get(doc, args, "nickname");
    if (t < 0 || !json_get_string(doc, t, name, sizeof(name)) || name[0] == '\0') {
        send_tool_error(req->fd, id, "missing non-empty 'nickname'");
        return;
    }
    if (!settings_set_nickname(name))
        send_tool_error(req->fd, id, "failed to set nickname");
    else {
        char msg[160];
        snprintf(msg, sizeof(msg), "nickname set to: %s", name);
        send_tool_ok(req->fd, id, msg);
    }
}

// Shared get/set for the normalized 0..1 float settings (brightness, volume).
static void tool_get_float(HttpRequest *req, const char *id, const char *label,
                           bool (*get)(float *)) {
    float v = 0.0f;
    if (!get(&v)) {
        send_tool_error(req->fd, id, "failed to read setting");
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%s: %.2f", label, v);
    send_tool_ok(req->fd, id, msg);
}

static void tool_set_float(HttpRequest *req, const char *id, const JsonDoc *doc,
                           int args, const char *key, const char *label,
                           bool (*set)(float)) {
    int t = json_obj_get(doc, args, key);
    double v;
    if (t < 0 || !json_get_double(doc, t, &v)) {
        char err[80];
        snprintf(err, sizeof(err), "missing numeric '%s' (0.0 - 1.0)", key);
        send_tool_error(req->fd, id, err);
        return;
    }
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    if (!set((float)v)) {
        send_tool_error(req->fd, id, "failed to set setting");
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%s set to %.2f", label, v);
    send_tool_ok(req->fd, id, msg);
}

static void tool_airplane_mode(HttpRequest *req, const char *id) {
    if (!settings_disable_wireless()) {
        send_tool_error(req->fd, id, "failed to disable wireless");
        return;
    }
    send_tool_ok(req->fd, id,
                 "wireless disabled. NOTE: the server is now unreachable until "
                 "wireless is re-enabled physically on the console.");
}

// list_installed_titles pages so a console with hundreds of games doesn't
// flood the model's context; the default matches the old fixed cap.
#define TITLES_PAGE_DEFAULT 256
#define TITLES_PAGE_MAX     1000
#define TITLE_LINE_MAX      160

typedef struct {
    char *text;
    size_t pos;
    int count;
} TitlesText;

static bool titles_text_visit(const TitleInfo *t, void *ctx) {
    TitlesText *tt = ctx;
    const char *storage =
        t->storage_id == 5 ? "sd" :
        t->storage_id == 4 ? "nand" :
        t->storage_id == 2 ? "gamecard" : "other";
    tt->pos += (size_t)snprintf(tt->text + tt->pos, TITLE_LINE_MAX,
        "%s%016llx  v%-6u  %-8s  %s", tt->count ? "\n" : "",
        (unsigned long long)t->title_id, t->version, storage,
        t->name[0] ? t->name : "(name unavailable)");
    tt->count++;
    return true;
}

static void tool_list_titles(HttpRequest *req, const char *id,
                             const JsonDoc *doc, int args) {
    int offset = get_int_or(doc, args, "offset", 0);
    int limit = get_int_or(doc, args, "limit", TITLES_PAGE_DEFAULT);
    if (offset < 0)
        offset = 0;
    if (limit < 1)
        limit = 1;
    if (limit > TITLES_PAGE_MAX)
        limit = TITLES_PAGE_MAX;

    // Each line is bounded (fixed fields + an 80-byte name), so the page fits.
    size_t cap = (size_t)limit * TITLE_LINE_MAX + 160;
    TitlesText tt = { malloc(cap), 0, 0 };
    if (!tt.text) {
        send_tool_error(req->fd, id, "out of memory");
        return;
    }
    tt.text[0] = '\0';
    int total = 0;
    char err[128] = {0};
    if (!titles_foreach(offset, limit, titles_text_visit, &tt, &total,
                        err, sizeof(err))) {
        free(tt.text);
        send_tool_error(req->fd, id, err[0] ? err : "failed to list titles");
        return;
    }
    char *end = tt.text + tt.pos;
    size_t left = cap - tt.pos;
    if (total == 0)
        snprintf(end, left, "(no titles installed)");
    else if (tt.count == 0)
        snprintf(end, left, "(no titles at offset %d; %d installed)", offset, total);
    else if (offset + tt.count < total)
        snprintf(end, left, "\n(showing %d-%d of %d installed titles; call again "
                 "with offset=%d for more)", offset + 1, offset + tt.count, total,
                 offset + tt.count);
    else if (offset > 0)
        snprintf(end, left, "\n(showing %d-%d of %d installed titles)",
                 offset + 1, offset + tt.count, total);
    send_tool_ok(req->fd, id, tt.text);
    free(tt.text);
}

static void tool_get_dns(HttpRequest *req, const char *id) {
    DnsConfig c;
    char err[96];
    if (!network_get_dns(&c, err, sizeof(err))) {
        send_tool_error(req->fd, id, err);
        return;
    }
    char text[256];
    if (c.is_automatic)
        snprintf(text, sizeof(text), "automatic (DHCP): primary=%s secondary=%s",
                 c.primary, c.secondary);
    else
        snprintf(text, sizeof(text), "manual: primary=%s secondary=%s",
                 c.primary, c.secondary[0] ? c.secondary : "(none)");
    send_tool_ok(req->fd, id, text);
}

static void tool_set_dns(HttpRequest *req, const char *id,
                         const JsonDoc *doc, int args) {
    bool automatic = false;
    int t = json_obj_get(doc, args, "automatic");
    if (t >= 0) json_get_bool(doc, t, &automatic);
    char primary[64] = {0}, secondary[64] = {0};
    t = json_obj_get(doc, args, "primary");
    if (t >= 0) json_get_string(doc, t, primary, sizeof(primary));
    t = json_obj_get(doc, args, "secondary");
    if (t >= 0) json_get_string(doc, t, secondary, sizeof(secondary));

    if (!automatic && !primary[0]) {
        send_tool_error(req->fd, id, "provide 'primary' (IPv4) or set 'automatic':true");
        return;
    }
    char err[96];
    if (!network_set_dns(automatic, primary, secondary, err, sizeof(err))) {
        send_tool_error(req->fd, id, err);
        return;
    }
    char text[256];
    if (automatic)
        snprintf(text, sizeof(text), "DNS set to automatic (DHCP)");
    else
        snprintf(text, sizeof(text), "DNS set to manual: %s%s%s", primary,
                 secondary[0] ? ", " : "", secondary);
    send_tool_ok(req->fd, id, text);
}

static void tool_get_auto_time(HttpRequest *req, const char *id) {
    bool en = false;
    if (!settings_get_auto_time(&en))
        send_tool_error(req->fd, id, "failed to read auto-time");
    else
        send_tool_ok(req->fd, id, en ? "enabled" : "disabled");
}

static void tool_set_auto_time(HttpRequest *req, const char *id,
                               const JsonDoc *doc, int args) {
    int t = json_obj_get(doc, args, "enabled");
    bool en;
    if (t < 0 || !json_get_bool(doc, t, &en)) {
        send_tool_error(req->fd, id, "missing boolean 'enabled'");
        return;
    }
    if (!settings_set_auto_time(en))
        send_tool_error(req->fd, id, "failed to set auto-time");
    else
        send_tool_ok(req->fd, id, en ? "internet time sync enabled"
                                     : "internet time sync disabled");
}

static void tool_get_datetime(HttpRequest *req, const char *id) {
    DateTime dt = {0};
    if (!settings_get_datetime(&dt)) {
        send_tool_error(req->fd, id, "failed to read date/time");
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg),
             "%04d-%02d-%02d %02d:%02d:%02d %s",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
             dt.timezone);
    send_tool_ok(req->fd, id, msg);
}

static int dt_field(const JsonDoc *doc, int args, const char *key, int def) {
    int t = json_obj_get(doc, args, key);
    long long v;
    if (t >= 0 && json_get_int(doc, t, &v))
        return (int)v;
    return def;
}

static void tool_set_datetime(HttpRequest *req, const char *id,
                              const JsonDoc *doc, int args) {
    // Start from the current value so callers may set only some fields.
    DateTime dt = {0};
    settings_get_datetime(&dt);
    dt.year   = dt_field(doc, args, "year",   dt.year);
    dt.month  = dt_field(doc, args, "month",  dt.month);
    dt.day    = dt_field(doc, args, "day",    dt.day);
    dt.hour   = dt_field(doc, args, "hour",   dt.hour);
    dt.minute = dt_field(doc, args, "minute", dt.minute);
    dt.second = dt_field(doc, args, "second", dt.second);

    if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
        dt.hour < 0 || dt.hour > 23 || dt.minute < 0 || dt.minute > 59 ||
        dt.second < 0 || dt.second > 59 || dt.year < 2000 || dt.year > 2100) {
        send_tool_error(req->fd, id, "invalid date/time fields");
        return;
    }
    if (!settings_set_datetime(&dt)) {
        send_tool_error(req->fd, id, "failed to set the clock");
        return;
    }
    char msg[160];
    snprintf(msg, sizeof(msg),
             "clock set to %04d-%02d-%02d %02d:%02d:%02d %s",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
             dt.timezone);
    send_tool_ok(req->fd, id, msg);
}

static void tool_power(HttpRequest *req, const char *id, PowerAction action,
                       const char *ok_msg) {
    if (!power_actions_available()) {
        send_tool_error(req->fd, id, "power control unavailable");
        return;
    }
    send_tool_ok(req->fd, id, ok_msg);
    power_schedule(action); // executed after this response is flushed
}

static void tool_get_running_process(HttpRequest *req, const char *id) {
    bool running = false;
    u64 pid = 0, title_id = 0;
    char err[128] = {0};
    if (!process_get_running(&running, &pid, &title_id, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to get running process");
        return;
    }
    char text[128];
    if (running) {
        snprintf(text, sizeof(text), "running: true, pid: %llu, title_id: %016llx",
                 (unsigned long long)pid, (unsigned long long)title_id);
    } else {
        snprintf(text, sizeof(text), "running: false (no active application)");
    }
    send_tool_ok(req->fd, id, text);
}

static void tool_launch_title(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char tid_str[32] = {0};
    int t = json_obj_get(doc, args, "title_id");
    if (t < 0) t = json_obj_get(doc, args, "titleId");
    if (t < 0 || !json_get_string(doc, t, tid_str, sizeof(tid_str))) {
        send_tool_error(req->fd, id, "missing 'title_id'");
        return;
    }
    u64 tid = strtoull(tid_str, NULL, 16);
    if (!tid) {
        send_tool_error(req->fd, id, "invalid 'title_id'");
        return;
    }
    u64 pid = 0;
    char err[128] = {0};
    if (!process_launch_title(tid, &pid, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to launch title");
        return;
    }
    char text[128];
    snprintf(text, sizeof(text), "launched title %016llx (pid %llu)",
             (unsigned long long)tid, (unsigned long long)pid);
    send_tool_ok(req->fd, id, text);
}

static void tool_launch_nro(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char path[256] = {0}, cmd_args[256] = {0}, host_tid_str[32] = {0};
    int t = json_obj_get(doc, args, "path");
    if (t < 0 || !json_get_string(doc, t, path, sizeof(path)) || path[0] == '\0') {
        send_tool_error(req->fd, id, "missing 'path'");
        return;
    }
    t = json_obj_get(doc, args, "args");
    if (t >= 0) json_get_string(doc, t, cmd_args, sizeof(cmd_args));
    t = json_obj_get(doc, args, "host_title_id");
    if (t < 0) t = json_obj_get(doc, args, "hostTitleId");
    if (t >= 0) json_get_string(doc, t, host_tid_str, sizeof(host_tid_str));
    u64 host_tid = host_tid_str[0] ? strtoull(host_tid_str, NULL, 16) : 0;

    u64 pid = 0;
    char err[128] = {0};
    if (!process_launch_nro(path, cmd_args, host_tid, &pid, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to launch NRO");
        return;
    }
    char text[320];
    snprintf(text, sizeof(text), "launched %s via nextload (pid %llu)", path, (unsigned long long)pid);
    send_tool_ok(req->fd, id, text);
}

static void tool_terminate_process(HttpRequest *req, const char *id) {
    char err[128] = {0};
    if (!process_terminate(err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to terminate process");
        return;
    }
    send_tool_ok(req->fd, id, "terminated active application");
}

static void tool_get_last_crash_report(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char tid_str[32] = {0};
    bool clear = false;
    if (args >= 0) {
        int t = json_obj_get(doc, args, "title_id");
        if (t < 0) t = json_obj_get(doc, args, "titleId");
        if (t >= 0) json_get_string(doc, t, tid_str, sizeof(tid_str));
        t = json_obj_get(doc, args, "clear");
        if (t >= 0) json_get_bool(doc, t, &clear);
    }
    u64 tid = tid_str[0] ? strtoull(tid_str, NULL, 16) : 0;

    static char log_buf[12288];
    char filename[128] = {0};
    bool found = false;
    char err[128] = {0};

    char *body = log_buf + 256;
    size_t body_max = sizeof(log_buf) - 256;

    if (!diagnostics_get_last_crash(tid, clear, body, body_max,
                                    filename, sizeof(filename), &found, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to read crash report");
        return;
    }

    if (!found) {
        send_tool_ok(req->fd, id, "(no crash reports found)");
        return;
    }

    int hlen = snprintf(log_buf, 256, "=== Crash Report: %s ===\n", filename);
    if (hlen > 0 && (size_t)hlen < 256) {
        memmove(log_buf + hlen, body, strlen(body) + 1);
    }
    send_tool_ok(req->fd, id, log_buf);
}

// --- JSON-RPC dispatch -------------------------------------------------------

static void tool_get_telemetry(HttpRequest *req, const char *id) {
    TelemetryStats stats;
    if (!telemetry_collect(&stats)) {
        send_tool_error(req->fd, id, "failed to collect telemetry");
        return;
    }
    char json_buf[1024];
    int len = telemetry_format_json(&stats, json_buf, sizeof(json_buf));
    if (len < 0) {
        send_tool_error(req->fd, id, "failed to format telemetry");
        return;
    }
    send_tool_ok(req->fd, id, json_buf);
}

static void handle_initialize(HttpRequest *req, const char *id, const JsonDoc *doc, int params) {
    // Respond with the client's protocol version when we know it; otherwise
    // the latest we support.
    char proto[24] = MCP_PROTO_LATEST;
    int vtok = json_obj_get(doc, params, "protocolVersion");
    if (vtok >= 0) {
        char v[24];
        if (json_get_string(doc, vtok, v, sizeof(v)) &&
            (strcmp(v, "2024-11-05") == 0 || strcmp(v, "2025-03-26") == 0 ||
             strcmp(v, "2025-06-18") == 0))
            snprintf(proto, sizeof(proto), "%s", v);
    }

    char val[256];
    int n = snprintf(val, sizeof(val),
                     "{\"protocolVersion\":\"%s\","
                     "\"capabilities\":{\"tools\":{}},"
                     "\"serverInfo\":{\"name\":\"sys-autopilot\",\"version\":\"%s\"}}",
                     proto, routes_app_version());
    send_rpc_value(req->fd, id, "result", val, (size_t)n);
}

static bool mcp_parse_u64(const char *s, uint64_t *out) {
    if (!s || !s[0]) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '\0') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno == ERANGE || end == s) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

static bool mcp_parse_i64(const char *s, int64_t *out) {
    if (!s || !s[0]) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return false;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 0);
    if (errno == ERANGE || end == s) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

static bool mcp_parse_hex(const char *hex, uint8_t *out, size_t cap, size_t *out_len) {
    if (!hex) return false;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    size_t len = strlen(hex);
    if (len == 0 || (len & 1) != 0 || len / 2 > cap) return false;
    for (size_t i = 0; i < len; i += 2) {
        if (!isxdigit((unsigned char)hex[i]) || !isxdigit((unsigned char)hex[i + 1]))
            return false;
        char b[3] = { hex[i], hex[i + 1], '\0' };
        out[i / 2] = (uint8_t)strtoul(b, NULL, 16);
    }
    *out_len = len / 2;
    return true;
}

static size_t mcp_encode_typed(long long v, const char *type_str, uint8_t *buf) {
    if (type_str && strcasecmp(type_str, "u8") == 0) {
        uint8_t u = (uint8_t)v; memcpy(buf, &u, 1); return 1;
    } else if (type_str && strcasecmp(type_str, "u16") == 0) {
        uint16_t u = (uint16_t)v; memcpy(buf, &u, 2); return 2;
    } else if (type_str && strcasecmp(type_str, "u64") == 0) {
        uint64_t u = (uint64_t)v; memcpy(buf, &u, 8); return 8;
    }
    uint32_t u = (uint32_t)v; memcpy(buf, &u, 4); return 4;
}

static void tool_get_memory_layout(HttpRequest *req, const char *id) {
    MemoryLayout layout;
    char err[128] = {0};
    if (!memory_get_layout(&layout, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to get memory layout");
        return;
    }
    char text[640];
    snprintf(text, sizeof(text),
             "Application PID: %llu (0x%llX)\n"
             "Title ID: %016llX\n"
             "Build ID: %s\n"
             "Modules:  %u\n"
             "RTLD Base:     0x%llX (size %llu)\n"
             "Main NSO Base: 0x%llX (size %llu)\n"
             "Heap Base:     0x%llX (size %llu)\n"
             "Alias Base:    0x%llX (size %llu)",
             (unsigned long long)layout.pid, (unsigned long long)layout.pid,
             (unsigned long long)layout.title_id,
             layout.build_id,
             layout.module_count,
             (unsigned long long)layout.rtld_base, (unsigned long long)layout.rtld_size,
             (unsigned long long)layout.main_base, (unsigned long long)layout.main_size,
             (unsigned long long)layout.heap_base, (unsigned long long)layout.heap_size,
             (unsigned long long)layout.alias_base, (unsigned long long)layout.alias_size);
    send_tool_ok(req->fd, id, text);
}

static void tool_query_memory(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char addr_str[32] = {0}, base_str[32] = {0};
    int t = json_obj_get(doc, args, "address");
    if (t < 0) t = json_obj_get(doc, args, "offset");
    if (t < 0) {
        send_tool_error(req->fd, id, "missing 'address'");
        return;
    }
    if (!json_get_string(doc, t, addr_str, sizeof(addr_str))) {
        long long v = 0;
        if (json_get_int(doc, t, &v)) snprintf(addr_str, sizeof(addr_str), "%llu", (unsigned long long)v);
    }
    uint64_t addr = 0;
    if (!mcp_parse_u64(addr_str, &addr)) {
        send_tool_error(req->fd, id, "invalid 'address'");
        return;
    }

    t = json_obj_get(doc, args, "base");
    if (t >= 0) json_get_string(doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    MemoryRegionInfo region;
    char err[128] = {0};
    if (!memory_query(addr, base, &region, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to query memory");
        return;
    }

    char text[256];
    snprintf(text, sizeof(text),
             "Memory region for 0x%llX:\n"
             "Base: 0x%llX\n"
             "Size: %llu bytes\n"
             "Type: %u, Perm: %u\n"
             "Permissions: %s%s%s",
             (unsigned long long)addr,
             (unsigned long long)region.base_address,
             (unsigned long long)region.size,
             region.type, region.perm,
             region.readable ? "R" : "-",
             region.writable ? "W" : "-",
             region.executable ? "X" : "-");
    send_tool_ok(req->fd, id, text);
}

static void tool_peek_memory(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char addr_str[32] = {0}, base_str[32] = {0};
    int t = json_obj_get(doc, args, "address");
    if (t < 0) t = json_obj_get(doc, args, "offset");
    if (t < 0) {
        send_tool_error(req->fd, id, "missing 'address'");
        return;
    }
    if (!json_get_string(doc, t, addr_str, sizeof(addr_str))) {
        long long v = 0;
        if (json_get_int(doc, t, &v)) snprintf(addr_str, sizeof(addr_str), "%llu", (unsigned long long)v);
    }
    uint64_t addr = 0;
    if (!mcp_parse_u64(addr_str, &addr)) {
        send_tool_error(req->fd, id, "invalid 'address'");
        return;
    }

    size_t size = 4;
    t = json_obj_get(doc, args, "size");
    if (t >= 0) {
        long long s = 0;
        if (json_get_int(doc, t, &s) && s > 0) size = (size_t)s;
    }
    if (size > 1024) size = 1024;

    t = json_obj_get(doc, args, "base");
    if (t >= 0) json_get_string(doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    static uint8_t buf[1024];
    uint64_t real_addr = 0;
    char err[128] = {0};
    if (!memory_read(addr, base, buf, size, &real_addr, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to read memory");
        return;
    }

    static char hex[2049];
    static const char hx[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; i++) {
        hex[i * 2]     = hx[buf[i] >> 4];
        hex[i * 2 + 1] = hx[buf[i] & 0xF];
    }
    hex[size * 2] = '\0';

    uint32_t val32 = 0;
    uint64_t val64 = 0;
    if (size >= 4) memcpy(&val32, buf, 4);
    if (size >= 8) memcpy(&val64, buf, 8);

    static char text[2304];
    snprintf(text, sizeof(text),
             "Read %zu bytes at 0x%llX (real 0x%llX):\n"
             "HEX: %s\n"
             "u32: %u (0x%08X)\n"
             "u64: %llu (0x%016llX)",
             size, (unsigned long long)addr, (unsigned long long)real_addr, hex,
             val32, val32,
             (unsigned long long)val64, (unsigned long long)val64);
    send_tool_ok(req->fd, id, text);
}

static void tool_poke_memory(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char addr_str[32] = {0}, base_str[32] = {0}, data_str[512] = {0};
    int t = json_obj_get(doc, args, "address");
    if (t < 0) t = json_obj_get(doc, args, "offset");
    if (t < 0) {
        send_tool_error(req->fd, id, "missing 'address'");
        return;
    }
    if (!json_get_string(doc, t, addr_str, sizeof(addr_str))) {
        long long v = 0;
        if (json_get_int(doc, t, &v)) snprintf(addr_str, sizeof(addr_str), "%llu", (unsigned long long)v);
    }
    uint64_t addr = 0;
    if (!mcp_parse_u64(addr_str, &addr)) {
        send_tool_error(req->fd, id, "invalid 'address'");
        return;
    }

    t = json_obj_get(doc, args, "base");
    if (t >= 0) json_get_string(doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    static uint8_t buf[256];
    size_t size = 0;

    t = json_obj_get(doc, args, "data");
    if (t >= 0 && json_get_string(doc, t, data_str, sizeof(data_str))) {
        if (!mcp_parse_hex(data_str, buf, sizeof(buf), &size)) {
            send_tool_error(req->fd, id, "invalid hex 'data' (must be even-length hex string)");
            return;
        }
    } else {
        t = json_obj_get(doc, args, "value");
        if (t >= 0) {
            long long v = 0;
            if (json_get_int(doc, t, &v)) {
                char type_str[16] = {0};
                int tt = json_obj_get(doc, args, "type");
                if (tt >= 0) json_get_string(doc, tt, type_str, sizeof(type_str));
                size = mcp_encode_typed(v, type_str, buf);
            }
        }
    }

    if (size == 0) {
        send_tool_error(req->fd, id, "missing 'data' (hex) or 'value'");
        return;
    }

    uint64_t real_addr = 0;
    char err[128] = {0};
    if (!memory_write(addr, base, buf, size, &real_addr, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "failed to write memory");
        return;
    }

    char text[256];
    snprintf(text, sizeof(text), "Wrote %zu bytes to 0x%llX (real 0x%llX)",
             size, (unsigned long long)addr, (unsigned long long)real_addr);
    send_tool_ok(req->fd, id, text);
}

static bool mcp_parse_offsets(const JsonDoc *doc, int args, int64_t out[16], size_t *out_count) {
    int arr = json_obj_get(doc, args, "offsets");
    if (arr < 0 || doc->tok[arr].type != JSMN_ARRAY) return false;
    int n = doc->tok[arr].size;
    if (n <= 0 || n > 16) return false;
    int elem = arr + 1;
    for (int i = 0; i < n; i++) {
        char s[32] = {0};
        if (json_get_string(doc, elem, s, sizeof(s))) {
            if (!mcp_parse_i64(s, &out[i])) return false;
        } else {
            long long v = 0;
            if (!json_get_int(doc, elem, &v)) return false;
            out[i] = (int64_t)v;
        }
        elem = json_skip(doc, elem);
    }
    *out_count = (size_t)n;
    return true;
}

static void tool_pointer_peek(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char base_str[32] = "main";
    int t = json_obj_get(doc, args, "base");
    if (t >= 0) json_get_string(doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    int64_t offsets[16];
    size_t count = 0;
    if (!mcp_parse_offsets(doc, args, offsets, &count)) {
        send_tool_error(req->fd, id, "missing or invalid 'offsets' array (1..16 elements)");
        return;
    }

    size_t size = 4;
    t = json_obj_get(doc, args, "size");
    if (t >= 0) {
        long long s = 0;
        if (json_get_int(doc, t, &s) && s > 0) size = (size_t)s;
    }
    if (size > 1024) size = 1024;

    uint64_t resolved = 0;
    char err[128] = {0};
    if (!memory_resolve_pointer(base, offsets, count, &resolved, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "pointer resolution failed");
        return;
    }

    static uint8_t buf[1024];
    if (!memory_read(resolved, MemoryBase_Absolute, buf, size, NULL, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "pointer read failed");
        return;
    }

    static char hex[2049];
    static const char hx[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; i++) {
        hex[i * 2]     = hx[buf[i] >> 4];
        hex[i * 2 + 1] = hx[buf[i] & 0xF];
    }
    hex[size * 2] = '\0';

    uint32_t val32 = 0;
    uint64_t val64 = 0;
    if (size >= 4) memcpy(&val32, buf, 4);
    if (size >= 8) memcpy(&val64, buf, 8);

    static char text[2304];
    snprintf(text, sizeof(text),
             "Pointer resolved to 0x%llX (read %zu bytes):\n"
             "HEX: %s\n"
             "u32: %u (0x%08X)\n"
             "u64: %llu (0x%016llX)",
             (unsigned long long)resolved, size, hex,
             val32, val32,
             (unsigned long long)val64, (unsigned long long)val64);
    send_tool_ok(req->fd, id, text);
}

static void tool_pointer_poke(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char base_str[32] = "main";
    int t = json_obj_get(doc, args, "base");
    if (t >= 0) json_get_string(doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    int64_t offsets[16];
    size_t count = 0;
    if (!mcp_parse_offsets(doc, args, offsets, &count)) {
        send_tool_error(req->fd, id, "missing or invalid 'offsets' array (1..16 elements)");
        return;
    }

    static uint8_t buf[256];
    size_t size = 0;
    char data_str[512] = {0};
    t = json_obj_get(doc, args, "data");
    if (t >= 0 && json_get_string(doc, t, data_str, sizeof(data_str))) {
        if (!mcp_parse_hex(data_str, buf, sizeof(buf), &size)) {
            send_tool_error(req->fd, id, "invalid hex 'data'");
            return;
        }
    } else {
        t = json_obj_get(doc, args, "value");
        if (t >= 0) {
            long long v = 0;
            if (json_get_int(doc, t, &v)) {
                char type_str[16] = {0};
                int tt = json_obj_get(doc, args, "type");
                if (tt >= 0) json_get_string(doc, tt, type_str, sizeof(type_str));
                size = mcp_encode_typed(v, type_str, buf);
            }
        }
    }
    if (size == 0) {
        send_tool_error(req->fd, id, "missing 'data' (hex) or 'value'");
        return;
    }

    uint64_t resolved = 0;
    char err[128] = {0};
    if (!memory_resolve_pointer(base, offsets, count, &resolved, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "pointer resolution failed");
        return;
    }
    if (!memory_write(resolved, MemoryBase_Absolute, buf, size, NULL, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "pointer write failed");
        return;
    }

    char text[256];
    snprintf(text, sizeof(text), "Pointer resolved to 0x%llX: wrote %zu bytes",
             (unsigned long long)resolved, size);
    send_tool_ok(req->fd, id, text);
}

static void tool_search_memory(HttpRequest *req, const char *id, const JsonDoc *doc, int args) {
    char base_str[32] = "heap";
    int t = json_obj_get(doc, args, "base");
    if (t >= 0) json_get_string(doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    uint8_t needle[64];
    size_t needle_len = 0;
    char data_str[140] = {0};
    t = json_obj_get(doc, args, "data");
    if (t >= 0 && json_get_string(doc, t, data_str, sizeof(data_str))) {
        if (!mcp_parse_hex(data_str, needle, sizeof(needle), &needle_len)) {
            send_tool_error(req->fd, id, "invalid hex 'data' (1..64 bytes)");
            return;
        }
    } else {
        t = json_obj_get(doc, args, "value");
        if (t >= 0) {
            long long v = 0;
            if (json_get_int(doc, t, &v)) {
                char type_str[16] = {0};
                int tt = json_obj_get(doc, args, "type");
                if (tt >= 0) json_get_string(doc, tt, type_str, sizeof(type_str));
                needle_len = mcp_encode_typed(v, type_str, needle);
            }
        }
    }
    if (needle_len == 0) {
        send_tool_error(req->fd, id, "missing 'data' (hex) or 'value'");
        return;
    }

    size_t max_results = 16;
    t = json_obj_get(doc, args, "maxResults");
    if (t < 0) t = json_obj_get(doc, args, "max_results");
    if (t >= 0) {
        long long mr = 0;
        if (json_get_int(doc, t, &mr) && mr > 0) max_results = (size_t)mr;
    }
    if (max_results > 64) max_results = 64;

    uint64_t addrs[64];
    size_t count = 0;
    char err[128] = {0};
    if (!memory_search(base, needle, needle_len, addrs, max_results, &count, err, sizeof(err))) {
        send_tool_error(req->fd, id, err[0] ? err : "memory search failed");
        return;
    }

    char text[2048];
    size_t pos = 0;
    int w = snprintf(text, sizeof(text), "Found %zu match(es):", count);
    if (w > 0 && (size_t)w < sizeof(text)) pos = (size_t)w;
    for (size_t i = 0; i < count && pos + 24 < sizeof(text); i++) {
        w = snprintf(text + pos, sizeof(text) - pos, "\n0x%llX", (unsigned long long)addrs[i]);
        if (w > 0 && (size_t)w < sizeof(text) - pos) pos += (size_t)w;
    }
    send_tool_ok(req->fd, id, text);
}

static void handle_tools_call(HttpRequest *req, const char *id, const JsonDoc *doc,
                              int params, bool content_streamed) {
    char name[40] = "";
    int ntok = json_obj_get(doc, params, "name");
    if (ntok < 0 || !json_get_string(doc, ntok, name, sizeof(name))) {
        send_rpc_error(req->fd, id, -32602, "missing tool name");
        return;
    }
    int args = json_obj_get(doc, params, "arguments"); // may be -1

    if (strcmp(name, "screenshot") == 0)            tool_screenshot(req, id, doc, args);
    else if (strcmp(name, "tap_buttons") == 0)      tool_tap_buttons(req, id, doc, args);
    else if (strcmp(name, "tap_sequence") == 0)     tool_tap_sequence(req, id, doc, args);
    else if (strcmp(name, "hold_buttons") == 0)     tool_hold_release(req, id, doc, args, true);
    else if (strcmp(name, "release_buttons") == 0)  tool_hold_release(req, id, doc, args, false);
    else if (strcmp(name, "set_stick") == 0)        tool_set_stick(req, id, doc, args);
    else if (strcmp(name, "touch_screen") == 0)     tool_touch_screen(req, id, doc, args);
    else if (strcmp(name, "clear_input") == 0)
        send_input_result_opt_screenshot(req, id, input_clear(), "ok", doc, args);
    else if (strcmp(name, "controller_handshake") == 0)
        send_input_result_opt_screenshot(req, id, input_handshake(), "ok", doc, args);
    else if (strcmp(name, "status") == 0)           tool_status(req, id);
    else if (strcmp(name, "list_directory") == 0)   tool_list_directory(req, id, doc, args);
    else if (strcmp(name, "read_file") == 0)        tool_read_file(req, id, doc, args);
    else if (strcmp(name, "upload_file") == 0)      tool_upload_file(req, id, doc, args, content_streamed);
    else if (strcmp(name, "delete_file") == 0)      tool_delete_file(req, id, doc, args);
    else if (strcmp(name, "hash_file") == 0)        tool_hash_file(req, id, doc, args);
    else if (strcmp(name, "create_token") == 0)     tool_create_token(req, id);
    else if (strcmp(name, "revoke_token") == 0)     tool_revoke_token(req, id, doc, args);
    else if (strcmp(name, "sleep") == 0)
        tool_power(req, id, PowerAction_Sleep,
                   "entering sleep mode; the server will be unreachable until "
                   "a human wakes the console");
    else if (strcmp(name, "restart") == 0)
        tool_power(req, id, PowerAction_Restart,
                   "rebooting; the server returns once the console boots back "
                   "into CFW (bootloader menus may require human intervention)");
    else if (strcmp(name, "power_off") == 0)
        tool_power(req, id, PowerAction_PowerOff,
                   "powering off; a human must press the power button to turn "
                   "the console back on");
    else if (strcmp(name, "get_theme") == 0)        tool_get_theme(req, id);
    else if (strcmp(name, "set_theme") == 0)        tool_set_theme(req, id, doc, args);
    else if (strcmp(name, "get_nickname") == 0)     tool_get_nickname(req, id);
    else if (strcmp(name, "set_nickname") == 0)     tool_set_nickname(req, id, doc, args);
    else if (strcmp(name, "get_brightness") == 0)
        tool_get_float(req, id, "brightness", settings_get_brightness);
    else if (strcmp(name, "set_brightness") == 0)
        tool_set_float(req, id, doc, args, "brightness", "brightness", settings_set_brightness);
    else if (strcmp(name, "get_volume") == 0)
        tool_get_float(req, id, "volume", settings_get_volume);
    else if (strcmp(name, "set_volume") == 0)
        tool_set_float(req, id, doc, args, "volume", "volume", settings_set_volume);
    else if (strcmp(name, "airplane_mode") == 0)    tool_airplane_mode(req, id);
    else if (strcmp(name, "list_installed_titles") == 0) tool_list_titles(req, id, doc, args);
    else if (strcmp(name, "get_dns") == 0)          tool_get_dns(req, id);
    else if (strcmp(name, "set_dns") == 0)          tool_set_dns(req, id, doc, args);
    else if (strcmp(name, "get_auto_time") == 0)    tool_get_auto_time(req, id);
    else if (strcmp(name, "set_auto_time") == 0)    tool_set_auto_time(req, id, doc, args);
    else if (strcmp(name, "get_datetime") == 0)     tool_get_datetime(req, id);
    else if (strcmp(name, "set_datetime") == 0)     tool_set_datetime(req, id, doc, args);
    else if (strcmp(name, "get_running_process") == 0)   tool_get_running_process(req, id);
    else if (strcmp(name, "launch_title") == 0)          tool_launch_title(req, id, doc, args);
    else if (strcmp(name, "launch_nro") == 0)            tool_launch_nro(req, id, doc, args);
    else if (strcmp(name, "terminate_process") == 0)     tool_terminate_process(req, id);
    else if (strcmp(name, "get_last_crash_report") == 0) tool_get_last_crash_report(req, id, doc, args);
    else if (strcmp(name, "get_telemetry") == 0)         tool_get_telemetry(req, id);
    else if (strcmp(name, "get_memory_layout") == 0)     tool_get_memory_layout(req, id);
    else if (strcmp(name, "query_memory") == 0)          tool_query_memory(req, id, doc, args);
    else if (strcmp(name, "peek_memory") == 0)           tool_peek_memory(req, id, doc, args);
    else if (strcmp(name, "poke_memory") == 0)           tool_poke_memory(req, id, doc, args);
    else if (strcmp(name, "pointer_peek") == 0)          tool_pointer_peek(req, id, doc, args);
    else if (strcmp(name, "pointer_poke") == 0)          tool_pointer_poke(req, id, doc, args);
    else if (strcmp(name, "search_memory") == 0)         tool_search_memory(req, id, doc, args);
    else send_rpc_error(req->fd, id, -32602, "unknown tool");
}

void mcp_handle_post(HttpRequest *req) {
    static char doc_buf[MCP_DOC_MAX];
    char *chunk = (char *)g_scratch_buf;
    const size_t chunk_sz = 0x8000;
    static JsonDoc doc;

    if (!req->has_content_length) {
        http_send_error(req->fd, 411, "Content-Length required");
        return;
    }

    memset(&g_upload, 0, sizeof(g_upload));
    b64dec_init(&g_upload.dec);

    Jstream js;
    jstream_init(&js, doc_buf, sizeof(doc_buf), upload_sink, &g_upload);

    ssize_t n;
    int jerr = 0;
    while ((n = http_read_body(req, chunk, chunk_sz)) > 0) {
        jerr = jstream_feed(&js, chunk, (size_t)n);
        if (jerr)
            break;
    }
    bool content_streamed = false;
    if (!jerr && n >= 0)
        jerr = jstream_finish(&js, &content_streamed);

    if (jerr || n < 0) {
        upload_cleanup();
        const char *msg = (jerr == JSTREAM_EDOC) ? "request too large"
                                                 : "parse error";
        send_rpc_error(req->fd, "null", -32700, msg);
        return;
    }

    if (json_parse(&doc, doc_buf, js.doc_len) != 0 || doc.ntok < 1 ||
        doc.tok[0].type != JSMN_OBJECT) {
        upload_cleanup();
        send_rpc_error(req->fd, "null", -32700, "parse error");
        return;
    }

    // id: echoed verbatim (number or string); absent => notification.
    char id[80] = "null";
    bool has_id = false;
    int id_tok = json_obj_get(&doc, 0, "id");
    if (id_tok >= 0 && json_raw(&doc, id_tok, id, sizeof(id)))
        has_id = true;

    char method[48] = "";
    int mtok = json_obj_get(&doc, 0, "method");
    if (mtok < 0 || !json_get_string(&doc, mtok, method, sizeof(method))) {
        upload_cleanup();
        send_rpc_error(req->fd, id, -32600, "missing method");
        return;
    }

    int params = json_obj_get(&doc, 0, "params"); // may be -1

    LOGF("mcp: %s\n", method);

    if (!has_id || strncmp(method, "notifications/", 14) == 0) {
        // Notification: accept and discard.
        upload_cleanup();
        http_send_header(req->fd, 202, "text/plain", 0);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(req, id, &doc, params);
    } else if (strcmp(method, "ping") == 0) {
        send_rpc_value(req->fd, id, "result", "{}", 2);
    } else if (strcmp(method, "tools/list") == 0) {
        send_rpc_value(req->fd, id, "result", kMcpToolsJson, strlen(kMcpToolsJson));
    } else if (strcmp(method, "tools/call") == 0) {
        if (params < 0)
            send_rpc_error(req->fd, id, -32602, "missing params");
        else
            handle_tools_call(req, id, &doc, params, content_streamed);
    } else {
        send_rpc_error(req->fd, id, -32601, "method not found");
    }

    // Whatever happened, no upload temp state may survive the request.
    upload_cleanup();
}

void mcp_handle_get(HttpRequest *req) {
    // We have no server-initiated messages, so we decline the optional
    // server->client SSE stream with 405 (spec-sanctioned; capable clients
    // proceed over POST). Force the connection closed so the client retires
    // this socket cleanly rather than reusing it into a reset.
    static const char body[] = "{\"error\":\"method not allowed\"}";
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 405 Method Not Allowed\r\n"
                     "Server: sys-autopilot\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     sizeof(body) - 1);
    http_write_all(req->fd, hdr, (size_t)n);
    http_write_all(req->fd, body, sizeof(body) - 1);
    req->keep_alive = false;
}
