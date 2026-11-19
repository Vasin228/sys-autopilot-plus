#include "routes.h"
#include "apiargs.h"
#include "files.h"
#include "input.h"
#include "install.h"
#include "titles.h"
#include "network.h"
#include "json.h"
#include "mcp.h"
#include "oauth.h"
#include "power.h"
#include "screen.h"
#include "settings.h"
#include "log.h"
#include "process.h"
#include "diagnostics.h"
#include "telemetry.h"
#include "memory.h"
#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

// Injected by the Makefile from package.json (the changesets-managed version).
#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif

// Cap for JSON request bodies on the REST input endpoints.
#define REST_BODY_MAX 16384

static u64 g_boot_tick;

__attribute__((constructor)) static void init_boot_tick(void) {
    g_boot_tick = armGetSystemTick();
}

const char *routes_app_version(void) {
    return APP_VERSION;
}

u64 routes_uptime_seconds(void) {
    return armTicksToNs(armGetSystemTick() - g_boot_tick) / 1000000000ULL;
}

// Reads and parses a JSON request body into *doc. An absent/empty body parses
// as an empty object. Returns the root token index, or -1 after sending an
// error response.
static int read_json_body(HttpRequest *req, JsonDoc *doc) {
    static char body[REST_BODY_MAX + 1];
    size_t total = 0;

    if (req->has_content_length && req->content_length > REST_BODY_MAX) {
        http_send_error(req->fd, 413, "request body too large");
        return -1;
    }
    ssize_t n;
    while (total < REST_BODY_MAX &&
           (n = http_read_body(req, body + total, REST_BODY_MAX - total)) > 0)
        total += (size_t)n;

    if (total == 0) {
        strcpy(body, "{}");
        total = 2;
    }
    if (json_parse(doc, body, total) != 0 || doc->ntok < 1 ||
        doc->tok[0].type != JSMN_OBJECT) {
        http_send_error(req->fd, 400, "request body must be a JSON object");
        return -1;
    }
    return 0; // root token
}

// --- /screenshot -------------------------------------------------------------

static void handle_screenshot(HttpRequest *req) {
    ViLayerStack stack = ViLayerStack_Screenshot;
    char val[32];
    if (http_query_get(req, "stack", val, sizeof(val))) {
        if (!screen_parse_stack(val, &stack)) {
            http_send_error(req->fd, 400, "invalid 'stack' (use screenshot|default|lcd|recording|lastframe)");
            return;
        }
    }

    const u8 *jpeg = NULL;
    u64 size = 0;
    Result rc = screen_capture_jpeg(stack, &jpeg, &size);
    if (R_FAILED(rc)) {
        http_send_json(req->fd, 500, "{\"error\":\"capture failed\",\"rc\":\"0x%x\"}", rc);
        return;
    }
    http_send_response(req->fd, 200, "image/jpeg", jpeg, (size_t)size);
}

static void handle_stream(HttpRequest *req) {
    ViLayerStack stack = ViLayerStack_Screenshot;
    char val[32];
    if (http_query_get(req, "stack", val, sizeof(val))) {
        if (!screen_parse_stack(val, &stack)) {
            http_send_error(req->fd, 400, "invalid 'stack'");
            return;
        }
    }
    int fps = 10;
    if (http_query_get(req, "fps", val, sizeof(val))) {
        fps = atoi(val);
        if (fps < 1) fps = 1;
        if (fps > 30) fps = 30;
    }

    static const char stream_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Server: sys-autopilot\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "\r\n";
    if (!http_write_all(req->fd, stream_hdr, sizeof(stream_hdr) - 1))
        return;

    const u8 *jpeg = NULL;
    u64 size = 0;
    Result rc = screen_capture_jpeg(stack, &jpeg, &size);
    if (R_SUCCEEDED(rc) && size > 0) {
        char part_hdr[128];
        int plen = snprintf(part_hdr, sizeof(part_hdr),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %llu\r\n\r\n",
            (unsigned long long)size);
        http_write_all(req->fd, part_hdr, plen);
        http_write_all(req->fd, jpeg, (size_t)size);
        http_write_all(req->fd, "\r\n", 2);
    }

    server_start_stream(req->fd, fps, stack);
    req->fd = -1;
}

static void handle_telemetry(HttpRequest *req) {
    TelemetryStats stats;
    if (!telemetry_collect(&stats)) {
        http_send_error(req->fd, 500, "failed to collect telemetry");
        return;
    }
    char json_buf[1024];
    int len = telemetry_format_json(&stats, json_buf, sizeof(json_buf));
    if (len < 0) {
        http_send_error(req->fd, 500, "failed to format telemetry");
        return;
    }
    http_send_response(req->fd, 200, "application/json", json_buf, (size_t)len);
}

// --- /input/* ----------------------------------------------------------------

static void send_input_result(HttpRequest *req, Result rc) {
    if (R_FAILED(rc))
        http_send_json(req->fd, 500, "{\"error\":\"input failed\",\"rc\":\"0x%x\"}", rc);
    else
        http_send_json(req->fd, 200, "{\"ok\":true}");
}

static void handle_input_tap(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    u64 mask;
    const char *err = NULL;
    if (!args_get_buttons(&doc, root, &mask, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    send_input_result(req, input_tap(mask, args_get_duration(&doc, root,
                                                             INPUT_DEFAULT_TAP_MS)));
}

static void handle_input_hold_release(HttpRequest *req, bool hold) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    u64 mask;
    const char *err = NULL;
    if (!args_get_buttons(&doc, root, &mask, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    send_input_result(req, hold ? input_hold(mask) : input_release(mask));
}

static void handle_input_hold(HttpRequest *req)    { handle_input_hold_release(req, true); }
static void handle_input_release(HttpRequest *req) { handle_input_hold_release(req, false); }

static void handle_input_stick(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    int side, duration;
    float x, y;
    const char *err = NULL;
    if (!args_get_stick(&doc, root, &side, &x, &y, &duration, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    send_input_result(req, input_stick(side, x, y, duration));
}

static void handle_input_touch(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    int x, y, x2, y2, duration;
    const char *err = NULL;
    if (!args_get_touch(&doc, root, &x, &y, &x2, &y2, &duration,
                        INPUT_DEFAULT_TAP_MS, &err)) {
        http_send_error(req->fd, 400, err);
        return;
    }
    send_input_result(req, input_touch(x, y, x2, y2, duration));
}

// --- /status -----------------------------------------------------------------

static void handle_status(HttpRequest *req) {
    u32 ver = hosversionGet();
    uint32_t pct = 0;
    bool charging = false;
    bool have_batt = settings_get_battery(&pct, &charging);
    if (have_batt) {
        http_send_json(req->fd, 200,
                       "{\"version\":\"" APP_VERSION "\","
                       "\"firmware\":\"%u.%u.%u\","
                       "\"controllerAttached\":%s,"
                       "\"uptimeSeconds\":%llu,"
                       "\"batteryPercent\":%u,"
                       "\"charging\":%s}",
                       HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
                       input_is_attached() ? "true" : "false",
                       (unsigned long long)routes_uptime_seconds(),
                       pct, charging ? "true" : "false");
    } else {
        http_send_json(req->fd, 200,
                       "{\"version\":\"" APP_VERSION "\","
                       "\"firmware\":\"%u.%u.%u\","
                       "\"controllerAttached\":%s,"
                       "\"uptimeSeconds\":%llu}",
                       HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver),
                       input_is_attached() ? "true" : "false",
                       (unsigned long long)routes_uptime_seconds());
    }
}

// --- /settings/* --------------------------------------------------------------

static void handle_settings_theme(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        bool dark = false;
        if (!settings_get_theme(&dark)) {
            http_send_error(req->fd, 500, "failed to read theme");
            return;
        }
        http_send_json(req->fd, 200, "{\"theme\":\"%s\"}", dark ? "dark" : "light");
        return;
    }
    // POST {"theme":"light"|"dark"}
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    char val[16] = {0};
    int t = json_obj_get(&doc, root, "theme");
    if (t < 0 || !json_get_string(&doc, t, val, sizeof(val))) {
        http_send_error(req->fd, 400, "missing 'theme' (\"light\" or \"dark\")");
        return;
    }
    bool dark;
    if (strcasecmp(val, "dark") == 0)       dark = true;
    else if (strcasecmp(val, "light") == 0) dark = false;
    else { http_send_error(req->fd, 400, "'theme' must be \"light\" or \"dark\""); return; }

    if (!settings_set_theme(dark))
        http_send_error(req->fd, 500, "failed to set theme");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"theme\":\"%s\"}", dark ? "dark" : "light");
}

static void handle_settings_nickname(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        char name[128] = {0}, esc[256] = {0};
        if (!settings_get_nickname(name, sizeof(name))) {
            http_send_error(req->fd, 500, "failed to read nickname");
            return;
        }
        if (json_escape(name, strlen(name), esc, sizeof(esc)) == (size_t)-1)
            snprintf(esc, sizeof(esc), "%s", name);
        http_send_json(req->fd, 200, "{\"nickname\":\"%s\"}", esc);
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    char name[128] = {0}, esc[256] = {0};
    int t = json_obj_get(&doc, root, "nickname");
    if (t < 0 || !json_get_string(&doc, t, name, sizeof(name)) || name[0] == '\0') {
        http_send_error(req->fd, 400, "missing non-empty 'nickname'");
        return;
    }
    if (!settings_set_nickname(name)) {
        http_send_error(req->fd, 500, "failed to set nickname");
    } else {
        if (json_escape(name, strlen(name), esc, sizeof(esc)) == (size_t)-1)
            snprintf(esc, sizeof(esc), "%s", name);
        http_send_json(req->fd, 200, "{\"ok\":true,\"nickname\":\"%s\"}", esc);
    }
}

// Shared get/set for the two normalized 0..1 float settings.
static void handle_settings_float(HttpRequest *req, const char *key,
                                  bool (*get)(float *), bool (*set)(float)) {
    if (strcmp(req->method, "GET") == 0) {
        float v = 0.0f;
        if (!get(&v)) {
            http_send_error(req->fd, 500, "failed to read setting");
            return;
        }
        http_send_json(req->fd, 200, "{\"%s\":%.3f}", key, v);
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    int t = json_obj_get(&doc, root, key);
    double v;
    if (t < 0 || !json_get_double(&doc, t, &v)) {
        http_send_error(req->fd, 400, "missing numeric value (0.0 - 1.0)");
        return;
    }
    if (!set((float)v))
        http_send_error(req->fd, 500, "failed to set setting");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"%s\":%.3f}", key,
                       v < 0 ? 0 : (v > 1 ? 1 : v));
}

static void handle_settings_brightness(HttpRequest *req) {
    handle_settings_float(req, "brightness",
                          settings_get_brightness, settings_set_brightness);
}

static void handle_settings_volume(HttpRequest *req) {
    handle_settings_float(req, "volume",
                          settings_get_volume, settings_set_volume);
}

static void handle_settings_airplane(HttpRequest *req) {
    // Enable-only: disabling wireless cuts our own connectivity.
    if (!settings_disable_wireless()) {
        http_send_error(req->fd, 500, "failed to disable wireless");
        return;
    }
    http_send_json(req->fd, 200,
                   "{\"ok\":true,\"note\":\"wireless disabled; the server is now "
                   "unreachable until wireless is re-enabled on the console\"}");
}

static void handle_settings_auto_time(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        bool en = false;
        if (!settings_get_auto_time(&en)) {
            http_send_error(req->fd, 500, "failed to read auto-time");
            return;
        }
        http_send_json(req->fd, 200, "{\"autoTime\":%s}", en ? "true" : "false");
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    int t = json_obj_get(&doc, root, "autoTime");
    bool en;
    if (t < 0 || !json_get_bool(&doc, t, &en)) {
        http_send_error(req->fd, 400, "missing boolean 'autoTime'");
        return;
    }
    if (!settings_set_auto_time(en))
        http_send_error(req->fd, 500, "failed to set auto-time");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"autoTime\":%s}", en ? "true" : "false");
}

// Reads an integer field from a JSON object; returns def if absent/invalid.
static int json_field_int(const JsonDoc *doc, int obj, const char *key, int def) {
    int t = json_obj_get(doc, obj, key);
    long long v;
    if (t >= 0 && json_get_int(doc, t, &v))
        return (int)v;
    return def;
}

static void handle_settings_datetime(HttpRequest *req) {
    if (strcmp(req->method, "GET") == 0) {
        DateTime dt = {0};
        if (!settings_get_datetime(&dt)) {
            http_send_error(req->fd, 500, "failed to read date/time");
            return;
        }
        http_send_json(req->fd, 200,
                       "{\"year\":%d,\"month\":%d,\"day\":%d,"
                       "\"hour\":%d,\"minute\":%d,\"second\":%d,"
                       "\"timezone\":\"%s\"}",
                       dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                       dt.timezone);
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;

    DateTime dt = {0};
    settings_get_datetime(&dt);
    dt.year   = json_field_int(&doc, root, "year",   dt.year);
    dt.month  = json_field_int(&doc, root, "month",  dt.month);
    dt.day    = json_field_int(&doc, root, "day",    dt.day);
    dt.hour   = json_field_int(&doc, root, "hour",   dt.hour);
    dt.minute = json_field_int(&doc, root, "minute", dt.minute);
    dt.second = json_field_int(&doc, root, "second", dt.second);

    if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
        dt.hour < 0 || dt.hour > 23 || dt.minute < 0 || dt.minute > 59 ||
        dt.second < 0 || dt.second > 59 || dt.year < 2000 || dt.year > 2100) {
        http_send_error(req->fd, 400, "invalid date/time fields");
        return;
    }
    if (!settings_set_datetime(&dt))
        http_send_error(req->fd, 500, "failed to set the clock");
    else
        http_send_json(req->fd, 200,
                       "{\"ok\":true,\"year\":%d,\"month\":%d,\"day\":%d,"
                       "\"hour\":%d,\"minute\":%d,\"second\":%d,\"timezone\":\"%s\"}",
                       dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                       dt.timezone);
}

// --- /titles -----------------------------------------------------------------

// Maps an NcmStorageId to the short label used in the JSON response.
static const char *storage_label(uint8_t storage_id) {
    switch (storage_id) {
        case 5: return "sd";
        case 4: return "nand";
        case 2: return "gamecard";
        default: return "other";
    }
}

// Heap-grown JSON body for /titles (the list has no fixed upper bound).
typedef struct {
    char  *buf;
    size_t len, cap;
    int    count;
} TitlesJson;

static bool titles_json_append(TitlesJson *j, const char *s, size_t n) {
    if (j->len + n + 1 > j->cap) {
        size_t newcap = j->cap ? j->cap : 4096;
        while (j->len + n + 1 > newcap)
            newcap *= 2;
        char *nb = realloc(j->buf, newcap);
        if (!nb)
            return false;
        j->buf = nb;
        j->cap = newcap;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
    return true;
}

static bool titles_json_visit(const TitleInfo *t, void *ctx) {
    TitlesJson *j = ctx;
    char name[sizeof(t->name) * 6 + 1]; // worst case: every byte -> \u00XX
    json_escape(t->name, strlen(t->name), name, sizeof(name));
    char entry[sizeof(name) + 128];
    int n = snprintf(entry, sizeof(entry),
        "%s{\"titleId\":\"%016llx\",\"version\":%u,\"storage\":\"%s\",\"name\":\"%s\"}",
        j->count ? "," : "", (unsigned long long)t->title_id,
        t->version, storage_label(t->storage_id), name);
    if (n < 0 || (size_t)n >= sizeof(entry))
        return false;
    j->count++;
    return titles_json_append(j, entry, (size_t)n);
}

// Parses an optional non-negative integer query parameter. Returns false (and
// sends a 400) when it's present but malformed.
static bool query_nonneg_int(HttpRequest *req, const char *key, int *out) {
    char val[16];
    if (!http_query_get(req, key, val, sizeof(val)) || !val[0])
        return true;
    char *end;
    long v = strtol(val, &end, 10);
    if (*end || v < 0 || v > 0x7fffffffL) {
        char msg[64];
        snprintf(msg, sizeof(msg), "'%s' must be a non-negative integer", key);
        http_send_error(req->fd, 400, msg);
        return false;
    }
    *out = (int)v;
    return true;
}

// GET /titles[?offset=N][&limit=M]. Returns every installed title by default;
// offset/limit select a window. "total" is always the full installed count.
static void handle_titles(HttpRequest *req) {
    int offset = 0, limit = -1;
    if (!query_nonneg_int(req, "offset", &offset) ||
        !query_nonneg_int(req, "limit", &limit))
        return;

    TitlesJson j = {0};
    char err[128] = {0};
    int total = 0;
    if (!titles_json_append(&j, "{\"titles\":[", 11)) {
        http_send_error(req->fd, 500, "out of memory");
        return;
    }
    if (!titles_foreach(offset, limit, titles_json_visit, &j, &total,
                        err, sizeof(err))) {
        free(j.buf);
        http_send_error(req->fd, 500, err[0] ? err : "out of memory listing titles");
        return;
    }
    char tail[64];
    int tn = snprintf(tail, sizeof(tail), "],\"total\":%d,\"offset\":%d}", total, offset);
    if (!titles_json_append(&j, tail, (size_t)tn)) {
        free(j.buf);
        http_send_error(req->fd, 500, "out of memory");
        return;
    }
    http_send_response(req->fd, 200, "application/json", j.buf, j.len);
    free(j.buf);
}

// --- /network/dns ------------------------------------------------------------

static void handle_dns_get(HttpRequest *req) {
    DnsConfig c;
    char err[96];
    if (!network_get_dns(&c, err, sizeof(err))) {
        http_send_error(req->fd, 500, err);
        return;
    }
    http_send_json(req->fd, 200,
        "{\"automatic\":%s,\"primary\":\"%s\",\"secondary\":\"%s\"}",
        c.is_automatic ? "true" : "false", c.primary, c.secondary);
}

// POST {"automatic":true} | {"primary":"1.2.3.4","secondary":"5.6.7.8"}
static void handle_dns_set(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;

    bool automatic = false;
    int t = json_obj_get(&doc, root, "automatic");
    if (t >= 0)
        json_get_bool(&doc, t, &automatic);

    // Buffers sized generously: json_get_string needs headroom (it reserves a
    // few bytes for escape expansion), so a 16-byte buffer would reject a full
    // 15-char dotted IPv4. network_set_dns validates the actual format.
    char primary[64] = {0}, secondary[64] = {0};
    t = json_obj_get(&doc, root, "primary");
    if (t >= 0) json_get_string(&doc, t, primary, sizeof(primary));
    t = json_obj_get(&doc, root, "secondary");
    if (t >= 0) json_get_string(&doc, t, secondary, sizeof(secondary));

    if (!automatic && !primary[0]) {
        http_send_error(req->fd, 400, "provide 'primary' (IPv4) or set 'automatic':true");
        return;
    }

    char err[96];
    if (!network_set_dns(automatic, primary, secondary, err, sizeof(err)))
        http_send_error(req->fd, 500, err);
    else if (automatic)
        http_send_json(req->fd, 200, "{\"ok\":true,\"automatic\":true}");
    else
        http_send_json(req->fd, 200,
            "{\"ok\":true,\"automatic\":false,\"primary\":\"%s\",\"secondary\":\"%s\"}",
            primary, secondary);
}

// --- /install ----------------------------------------------------------------

// Adapts the HTTP request body into the installer's sequential read callback.
static long install_body_read(void *ctx, void *buf, size_t len) {
    return (long)http_read_body((HttpRequest *)ctx, buf, len);
}

static void handle_install(HttpRequest *req) {
    if (!req->has_content_length) {
        http_send_error(req->fd, 411, "Content-Length required (stream the NSP/XCI with curl -T)");
        return;
    }

    // ?storage=sd|nand (default sd)
    InstallStorage storage = INSTALL_STORAGE_SD;
    char sval[8];
    if (http_query_get(req, "storage", sval, sizeof(sval)) &&
        strcasecmp(sval, "nand") == 0)
        storage = INSTALL_STORAGE_NAND;

    InstallResult res;
    install_stream(install_body_read, req, req->content_length, storage, &res);

    char esc[256];
    json_escape(res.message, strlen(res.message), esc, sizeof(esc));
    if (res.ok) {
        http_send_json(req->fd, 200,
                       "{\"ok\":true,\"titleId\":\"%016llx\",\"version\":%u,\"message\":\"%s\"}",
                       (unsigned long long)res.title_id, res.version, esc);
    } else {
        http_send_json(req->fd, res.http_status ? res.http_status : 500,
                       "{\"ok\":false,\"error\":\"%s\"}", esc);
    }
}

// --- /power/* ------------------------------------------------------------------

static void handle_power(HttpRequest *req, PowerAction action, const char *note) {
    if (!power_actions_available()) {
        http_send_error(req->fd, 500, "power control unavailable");
        return;
    }
    http_send_json(req->fd, 200, "{\"ok\":true,\"note\":\"%s\"}", note);
    power_schedule(action); // executed after this response is flushed
}

static void handle_power_sleep(HttpRequest *req) {
    handle_power(req, PowerAction_Sleep,
                 "entering sleep; server unreachable until console is woken physically");
}

static void handle_power_restart(HttpRequest *req) {
    handle_power(req, PowerAction_Restart,
                 "rebooting; server returns after the console boots back into CFW (bootloader menus may require manual intervention)");
}

static void handle_power_off(HttpRequest *req) {
    handle_power(req, PowerAction_PowerOff,
                 "powering off; physical power button required to turn back on");
}

// --- /process/* ---------------------------------------------------------------

static void handle_process_current(HttpRequest *req) {
    bool running = false;
    u64 pid = 0, title_id = 0;
    char err[128] = {0};
    if (!process_get_running(&running, &pid, &title_id, err, sizeof(err))) {
        http_send_error(req->fd, 500, err[0] ? err : "failed to get running process");
        return;
    }
    if (running) {
        http_send_json(req->fd, 200, "{\"running\":true,\"pid\":%llu,\"titleId\":\"%016llx\"}",
                       (unsigned long long)pid, (unsigned long long)title_id);
    } else {
        http_send_json(req->fd, 200, "{\"running\":false}");
    }
}

static void handle_process_launch(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    char tid_str[32] = {0};
    int t = json_obj_get(&doc, root, "title_id");
    if (t < 0) t = json_obj_get(&doc, root, "titleId");
    if (t < 0 || !json_get_string(&doc, t, tid_str, sizeof(tid_str))) {
        http_send_error(req->fd, 400, "missing 'title_id' (16-hex string)");
        return;
    }
    u64 tid = strtoull(tid_str, NULL, 16);
    if (!tid) {
        http_send_error(req->fd, 400, "invalid 'title_id'");
        return;
    }
    u64 pid = 0;
    char err[128] = {0};
    if (!process_launch_title(tid, &pid, err, sizeof(err))) {
        http_send_error(req->fd, 500, err[0] ? err : "failed to launch title");
        return;
    }
    http_send_json(req->fd, 200, "{\"ok\":true,\"pid\":%llu,\"titleId\":\"%016llx\"}",
                   (unsigned long long)pid, (unsigned long long)tid);
}

static void handle_process_launch_nro(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    char path[256] = {0}, args[256] = {0}, host_tid_str[32] = {0};
    int t = json_obj_get(&doc, root, "path");
    if (t < 0 || !json_get_string(&doc, t, path, sizeof(path)) || path[0] == '\0') {
        http_send_error(req->fd, 400, "missing 'path' to .nro");
        return;
    }
    t = json_obj_get(&doc, root, "args");
    if (t >= 0) json_get_string(&doc, t, args, sizeof(args));
    t = json_obj_get(&doc, root, "host_title_id");
    if (t < 0) t = json_obj_get(&doc, root, "hostTitleId");
    if (t >= 0) json_get_string(&doc, t, host_tid_str, sizeof(host_tid_str));
    u64 host_tid = host_tid_str[0] ? strtoull(host_tid_str, NULL, 16) : 0;

    u64 pid = 0;
    char err[128] = {0};
    if (!process_launch_nro(path, args, host_tid, &pid, err, sizeof(err))) {
        http_send_error(req->fd, 500, err[0] ? err : "failed to launch NRO");
        return;
    }
    char esc_path[512];
    if (json_escape(path, strlen(path), esc_path, sizeof(esc_path)) == (size_t)-1)
        snprintf(esc_path, sizeof(esc_path), "%s", path);
    http_send_json(req->fd, 200, "{\"ok\":true,\"pid\":%llu,\"path\":\"%s\"}",
                   (unsigned long long)pid, esc_path);
}

static void handle_process_terminate(HttpRequest *req) {
    char err[128] = {0};
    if (!process_terminate(err, sizeof(err))) {
        http_send_error(req->fd, 500, err[0] ? err : "failed to terminate process");
        return;
    }
    http_send_json(req->fd, 200, "{\"ok\":true}");
}

// --- /memory/* ----------------------------------------------------------------

static int mem_err_status(const char *err) {
    return (err && strstr(err, "no running application")) ? 409 : 500;
}

static bool parse_u64_strict(const char *s, uint64_t *out) {
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

static bool parse_i64_strict(const char *s, int64_t *out) {
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

static bool parse_hex_strict(const char *hex, uint8_t *out, size_t cap, size_t *out_len) {
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

static size_t encode_typed_value(long long v, const char *type_str, uint8_t *buf) {
    if (type_str && strcasecmp(type_str, "u8") == 0) {
        uint8_t u = (uint8_t)v; memcpy(buf, &u, 1); return 1;
    } else if (type_str && strcasecmp(type_str, "u16") == 0) {
        uint16_t u = (uint16_t)v; memcpy(buf, &u, 2); return 2;
    } else if (type_str && strcasecmp(type_str, "u64") == 0) {
        uint64_t u = (uint64_t)v; memcpy(buf, &u, 8); return 8;
    }
    uint32_t u = (uint32_t)v; memcpy(buf, &u, 4); return 4;
}

static void handle_memory_layout(HttpRequest *req) {
    MemoryLayout layout;
    char err[128] = {0};
    if (!memory_get_layout(&layout, err, sizeof(err))) {
        http_send_error(req->fd, mem_err_status(err), err[0] ? err : "failed to get memory layout");
        return;
    }
    http_send_json(req->fd, 200,
        "{\"ok\":true,\"running\":%s,\"pid\":%llu,\"titleId\":\"%016llX\","
        "\"moduleCount\":%u,"
        "\"rtldBase\":\"0x%llX\",\"rtldSize\":%llu,"
        "\"mainBase\":\"0x%llX\",\"mainSize\":%llu,"
        "\"heapBase\":\"0x%llX\",\"heapSize\":%llu,"
        "\"aliasBase\":\"0x%llX\",\"aliasSize\":%llu,"
        "\"buildId\":\"%s\"}",
        layout.running ? "true" : "false",
        (unsigned long long)layout.pid,
        (unsigned long long)layout.title_id,
        layout.module_count,
        (unsigned long long)layout.rtld_base,
        (unsigned long long)layout.rtld_size,
        (unsigned long long)layout.main_base,
        (unsigned long long)layout.main_size,
        (unsigned long long)layout.heap_base,
        (unsigned long long)layout.heap_size,
        (unsigned long long)layout.alias_base,
        (unsigned long long)layout.alias_size,
        layout.build_id);
}

static void handle_memory_query(HttpRequest *req) {
    char addr_str[32] = {0}, base_str[32] = {0};

    if (strcmp(req->method, "POST") == 0) {
        static JsonDoc doc;
        int root = read_json_body(req, &doc);
        if (root < 0) return;
        int t = json_obj_get(&doc, root, "address");
        if (t < 0) t = json_obj_get(&doc, root, "offset");
        if (t >= 0) {
            if (!json_get_string(&doc, t, addr_str, sizeof(addr_str))) {
                long long v = 0;
                if (json_get_int(&doc, t, &v)) snprintf(addr_str, sizeof(addr_str), "%llu", (unsigned long long)v);
            }
        }
        t = json_obj_get(&doc, root, "base");
        if (t >= 0) json_get_string(&doc, t, base_str, sizeof(base_str));
    } else {
        http_query_get(req, "address", addr_str, sizeof(addr_str));
        if (!addr_str[0]) http_query_get(req, "offset", addr_str, sizeof(addr_str));
        http_query_get(req, "base", base_str, sizeof(base_str));
    }

    uint64_t addr = 0;
    if (!parse_u64_strict(addr_str, &addr)) {
        http_send_error(req->fd, 400, "missing or invalid 'address'");
        return;
    }

    MemoryBase base = memory_parse_base(base_str);

    MemoryRegionInfo region;
    char err[128] = {0};
    if (!memory_query(addr, base, &region, err, sizeof(err))) {
        http_send_error(req->fd, mem_err_status(err), err[0] ? err : "failed to query memory");
        return;
    }

    http_send_json(req->fd, 200,
        "{\"ok\":true,\"address\":\"0x%llX\",\"regionAddress\":\"0x%llX\",\"regionSize\":%llu,\"type\":%u,\"perm\":%u,\"readable\":%s,\"writable\":%s,\"executable\":%s}",
        (unsigned long long)addr, (unsigned long long)region.base_address, (unsigned long long)region.size,
        region.type, region.perm,
        region.readable ? "true" : "false",
        region.writable ? "true" : "false",
        region.executable ? "true" : "false");
}

static void handle_memory_peek(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0) return;

    char addr_str[32] = {0}, base_str[32] = {0};
    int t = json_obj_get(&doc, root, "address");
    if (t < 0) t = json_obj_get(&doc, root, "offset");
    if (t < 0) {
        http_send_error(req->fd, 400, "missing 'address'");
        return;
    }
    if (!json_get_string(&doc, t, addr_str, sizeof(addr_str))) {
        long long v = 0;
        if (json_get_int(&doc, t, &v)) {
            snprintf(addr_str, sizeof(addr_str), "%llu", (unsigned long long)v);
        }
    }
    uint64_t addr = 0;
    if (!parse_u64_strict(addr_str, &addr)) {
        http_send_error(req->fd, 400, "invalid 'address'");
        return;
    }

    size_t size = 4;
    t = json_obj_get(&doc, root, "size");
    if (t >= 0) {
        long long s = 0;
        if (json_get_int(&doc, t, &s) && s > 0) size = (size_t)s;
    }
    if (size > 4096) size = 4096;

    t = json_obj_get(&doc, root, "base");
    if (t >= 0) json_get_string(&doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    static uint8_t buf[4096];
    uint64_t real_addr = 0;
    char err[128] = {0};
    if (!memory_read(addr, base, buf, size, &real_addr, err, sizeof(err))) {
        http_send_error(req->fd, mem_err_status(err), err[0] ? err : "failed to read memory");
        return;
    }

    static char hex_buf[8193];
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        hex_buf[i * 2]     = hx[buf[i] >> 4];
        hex_buf[i * 2 + 1] = hx[buf[i] & 0xF];
    }
    hex_buf[size * 2] = '\0';

    uint32_t val32 = 0;
    if (size >= 4) memcpy(&val32, buf, sizeof(val32));
    uint64_t val64 = 0;
    if (size >= 8) memcpy(&val64, buf, sizeof(val64));

    static char resp[8448];
    int n = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"address\":\"0x%llX\",\"realAddress\":\"0x%llX\",\"size\":%zu,\"hex\":\"%s\",\"u32\":%u,\"u64\":\"0x%llX\"}",
        (unsigned long long)addr, (unsigned long long)real_addr, size, hex_buf, val32, (unsigned long long)val64);
    if (n > 0 && (size_t)n < sizeof(resp))
        http_send_response(req->fd, 200, "application/json", resp, (size_t)n);
    else
        http_send_error(req->fd, 500, "response too large");
}

static void handle_memory_poke(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0) return;

    char addr_str[32] = {0}, base_str[32] = {0}, data_str[1024] = {0};
    int t = json_obj_get(&doc, root, "address");
    if (t < 0) t = json_obj_get(&doc, root, "offset");
    if (t < 0) {
        http_send_error(req->fd, 400, "missing 'address'");
        return;
    }
    if (!json_get_string(&doc, t, addr_str, sizeof(addr_str))) {
        long long v = 0;
        if (json_get_int(&doc, t, &v)) snprintf(addr_str, sizeof(addr_str), "%llu", (unsigned long long)v);
    }
    uint64_t addr = 0;
    if (!parse_u64_strict(addr_str, &addr)) {
        http_send_error(req->fd, 400, "invalid 'address'");
        return;
    }

    t = json_obj_get(&doc, root, "base");
    if (t >= 0) json_get_string(&doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    static uint8_t buf[512];
    size_t size = 0;

    t = json_obj_get(&doc, root, "data");
    if (t >= 0 && json_get_string(&doc, t, data_str, sizeof(data_str))) {
        if (!parse_hex_strict(data_str, buf, sizeof(buf), &size)) {
            http_send_error(req->fd, 400, "invalid hex 'data' (must be even-length hex string)");
            return;
        }
    } else {
        t = json_obj_get(&doc, root, "value");
        if (t >= 0) {
            long long v = 0;
            if (json_get_int(&doc, t, &v)) {
                char type_str[16] = {0};
                int tt = json_obj_get(&doc, root, "type");
                if (tt >= 0) json_get_string(&doc, tt, type_str, sizeof(type_str));
                size = encode_typed_value(v, type_str, buf);
            }
        }
    }

    if (size == 0) {
        http_send_error(req->fd, 400, "missing 'data' (hex) or 'value'");
        return;
    }

    uint64_t real_addr = 0;
    char err[128] = {0};
    if (!memory_write(addr, base, buf, size, &real_addr, err, sizeof(err))) {
        http_send_error(req->fd, mem_err_status(err), err[0] ? err : "failed to write memory");
        return;
    }

    http_send_json(req->fd, 200,
        "{\"ok\":true,\"address\":\"0x%llX\",\"realAddress\":\"0x%llX\",\"written\":%zu}",
        (unsigned long long)addr, (unsigned long long)real_addr, size);
}

static void handle_memory_pointer(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0) return;

    char base_str[32] = "main";
    int t = json_obj_get(&doc, root, "base");
    if (t >= 0) json_get_string(&doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    int arr = json_obj_get(&doc, root, "offsets");
    if (arr < 0 || doc.tok[arr].type != JSMN_ARRAY) {
        http_send_error(req->fd, 400, "missing 'offsets' array");
        return;
    }
    int n_off = doc.tok[arr].size;
    if (n_off <= 0 || n_off > 16) {
        http_send_error(req->fd, 400, "'offsets' must have 1..16 elements");
        return;
    }

    int64_t offsets[16];
    int elem = arr + 1;
    for (int i = 0; i < n_off; i++) {
        char s[32] = {0};
        if (json_get_string(&doc, elem, s, sizeof(s))) {
            if (!parse_i64_strict(s, &offsets[i])) {
                http_send_error(req->fd, 400, "invalid offset in 'offsets'");
                return;
            }
        } else {
            long long v = 0;
            if (!json_get_int(&doc, elem, &v)) {
                http_send_error(req->fd, 400, "invalid offset in 'offsets'");
                return;
            }
            offsets[i] = (int64_t)v;
        }
        elem = json_skip(&doc, elem);
    }

    uint64_t resolved = 0;
    char err[128] = {0};
    if (!memory_resolve_pointer(base, offsets, (size_t)n_off, &resolved, err, sizeof(err))) {
        http_send_error(req->fd, mem_err_status(err), err[0] ? err : "pointer resolution failed");
        return;
    }

    // Optional write (data or value)
    char data_str[1024] = {0};
    int td = json_obj_get(&doc, root, "data");
    int tv = json_obj_get(&doc, root, "value");
    if (td >= 0 || tv >= 0) {
        static uint8_t wbuf[512];
        size_t wsize = 0;
        if (td >= 0 && json_get_string(&doc, td, data_str, sizeof(data_str))) {
            if (!parse_hex_strict(data_str, wbuf, sizeof(wbuf), &wsize)) {
                http_send_error(req->fd, 400, "invalid hex 'data'");
                return;
            }
        } else if (tv >= 0) {
            long long v = 0;
            if (json_get_int(&doc, tv, &v)) {
                char type_str[16] = {0};
                int tt = json_obj_get(&doc, root, "type");
                if (tt >= 0) json_get_string(&doc, tt, type_str, sizeof(type_str));
                wsize = encode_typed_value(v, type_str, wbuf);
            }
        }
        if (wsize == 0) {
            http_send_error(req->fd, 400, "invalid write payload");
            return;
        }
        if (!memory_write(resolved, MemoryBase_Absolute, wbuf, wsize, NULL, err, sizeof(err))) {
            http_send_error(req->fd, mem_err_status(err), err[0] ? err : "pointer write failed");
            return;
        }
        http_send_json(req->fd, 200,
            "{\"ok\":true,\"resolvedAddress\":\"0x%llX\",\"written\":%zu}",
            (unsigned long long)resolved, wsize);
        return;
    }

    // Optional read (size)
    int ts = json_obj_get(&doc, root, "size");
    if (ts >= 0) {
        long long s = 0;
        if (!json_get_int(&doc, ts, &s) || s <= 0) {
            http_send_error(req->fd, 400, "invalid 'size'");
            return;
        }
        size_t rsize = (size_t)s > 4096 ? 4096 : (size_t)s;
        static uint8_t rbuf[4096];
        if (!memory_read(resolved, MemoryBase_Absolute, rbuf, rsize, NULL, err, sizeof(err))) {
            http_send_error(req->fd, mem_err_status(err), err[0] ? err : "pointer read failed");
            return;
        }
        static char hex_buf[8193];
        static const char hx[] = "0123456789abcdef";
        for (size_t i = 0; i < rsize; i++) {
            hex_buf[i * 2]     = hx[rbuf[i] >> 4];
            hex_buf[i * 2 + 1] = hx[rbuf[i] & 0xF];
        }
        hex_buf[rsize * 2] = '\0';
        uint32_t val32 = 0;
        uint64_t val64 = 0;
        if (rsize >= 4) memcpy(&val32, rbuf, 4);
        if (rsize >= 8) memcpy(&val64, rbuf, 8);
        static char resp[8448];
        int n = snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"resolvedAddress\":\"0x%llX\",\"size\":%zu,\"hex\":\"%s\",\"u32\":%u,\"u64\":\"0x%llX\"}",
            (unsigned long long)resolved, rsize, hex_buf, val32, (unsigned long long)val64);
        if (n > 0 && (size_t)n < sizeof(resp))
            http_send_response(req->fd, 200, "application/json", resp, (size_t)n);
        else
            http_send_error(req->fd, 500, "response too large");
        return;
    }

    http_send_json(req->fd, 200, "{\"ok\":true,\"resolvedAddress\":\"0x%llX\"}",
                   (unsigned long long)resolved);
}

static void handle_memory_search(HttpRequest *req) {
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0) return;

    char base_str[32] = "heap";
    int t = json_obj_get(&doc, root, "base");
    if (t >= 0) json_get_string(&doc, t, base_str, sizeof(base_str));
    MemoryBase base = memory_parse_base(base_str);

    uint8_t needle[64];
    size_t needle_len = 0;
    char data_str[140] = {0};
    t = json_obj_get(&doc, root, "data");
    if (t >= 0 && json_get_string(&doc, t, data_str, sizeof(data_str))) {
        if (!parse_hex_strict(data_str, needle, sizeof(needle), &needle_len)) {
            http_send_error(req->fd, 400, "invalid hex 'data' (1..64 bytes)");
            return;
        }
    } else {
        t = json_obj_get(&doc, root, "value");
        if (t >= 0) {
            long long v = 0;
            if (json_get_int(&doc, t, &v)) {
                char type_str[16] = {0};
                int tt = json_obj_get(&doc, root, "type");
                if (tt >= 0) json_get_string(&doc, tt, type_str, sizeof(type_str));
                needle_len = encode_typed_value(v, type_str, needle);
            }
        }
    }
    if (needle_len == 0) {
        http_send_error(req->fd, 400, "missing 'data' (hex) or 'value'");
        return;
    }

    size_t max_results = 16;
    t = json_obj_get(&doc, root, "maxResults");
    if (t < 0) t = json_obj_get(&doc, root, "max_results");
    if (t >= 0) {
        long long mr = 0;
        if (json_get_int(&doc, t, &mr) && mr > 0) max_results = (size_t)mr;
    }
    if (max_results > 64) max_results = 64;

    uint64_t addrs[64];
    size_t count = 0;
    char err[128] = {0};
    if (!memory_search(base, needle, needle_len, addrs, max_results, &count, err, sizeof(err))) {
        http_send_error(req->fd, mem_err_status(err), err[0] ? err : "memory search failed");
        return;
    }

    char resp[2048];
    size_t pos = 0;
    int w = snprintf(resp, sizeof(resp), "{\"ok\":true,\"count\":%zu,\"matches\":[", count);
    if (w > 0 && (size_t)w < sizeof(resp)) pos = (size_t)w;
    for (size_t i = 0; i < count && pos + 24 < sizeof(resp); i++) {
        w = snprintf(resp + pos, sizeof(resp) - pos, "%s\"0x%llX\"",
                     i ? "," : "", (unsigned long long)addrs[i]);
        if (w > 0 && (size_t)w < sizeof(resp) - pos) pos += (size_t)w;
    }
    if (pos + 3 < sizeof(resp)) {
        memcpy(resp + pos, "]}", 2);
        pos += 2;
    }
    http_send_response(req->fd, 200, "application/json", resp, pos);
}

// --- /diagnostics/* -----------------------------------------------------------

static void handle_diagnostics_crash(HttpRequest *req) {
    char tid_str[32] = {0}, clear_str[16] = {0};
    http_query_get(req, "title_id", tid_str, sizeof(tid_str));
    if (!tid_str[0]) http_query_get(req, "titleId", tid_str, sizeof(tid_str));
    http_query_get(req, "clear", clear_str, sizeof(clear_str));

    u64 tid = tid_str[0] ? strtoull(tid_str, NULL, 16) : 0;
    bool clear = (strcmp(clear_str, "true") == 0 || strcmp(clear_str, "1") == 0);

    static char log_buf[12288];
    char filename[128] = {0};
    bool found = false;
    char err[128] = {0};

    if (!diagnostics_get_last_crash(tid, clear, log_buf, sizeof(log_buf),
                                   filename, sizeof(filename), &found, err, sizeof(err))) {
        http_send_error(req->fd, 500, err[0] ? err : "failed to read crash report");
        return;
    }

    if (!found) {
        http_send_json(req->fd, 200, "{\"found\":false}");
        return;
    }

    size_t llen = strlen(log_buf);
    size_t esc_len = json_escaped_len(log_buf, llen);
    char *escaped = malloc(esc_len + 1);
    if (!escaped) {
        http_send_error(req->fd, 500, "out of memory");
        return;
    }
    json_escape(log_buf, llen, escaped, esc_len + 1);
    char prefix[256];
    int plen = snprintf(prefix, sizeof(prefix), "{\"found\":true,\"filename\":\"%s\",\"content\":\"", filename);
    const char *suffix = "\"}";
    if (plen > 0 && (size_t)plen < sizeof(prefix)) {
        http_send_header(req->fd, 200, "application/json", (size_t)plen + esc_len + 2);
        http_write_all(req->fd, prefix, (size_t)plen);
        http_write_all(req->fd, escaped, esc_len);
        http_write_all(req->fd, suffix, 2);
    } else {
        http_send_error(req->fd, 500, "filename too long");
    }
    free(escaped);
}

// --- dispatch ----------------------------------------------------------------

typedef struct {
    const char *method;
    const char *path;
    void (*handler)(HttpRequest *req);
} Route;

static void handle_controller_attach(HttpRequest *req) { send_input_result(req, input_attach()); }
static void handle_controller_detach(HttpRequest *req) { send_input_result(req, input_detach()); }
static void handle_controller_handshake(HttpRequest *req) { send_input_result(req, input_handshake()); }
static void handle_input_clear(HttpRequest *req)       { send_input_result(req, input_clear()); }

static const Route kRoutes[] = {
    { "GET",    "/screenshot",        handle_screenshot },
    { "GET",    "/stream",            handle_stream },
    { "GET",    "/telemetry",         handle_telemetry },
    { "GET",    "/status",            handle_status },
    { "POST",   "/input/tap",         handle_input_tap },
    { "POST",   "/input/hold",        handle_input_hold },
    { "POST",   "/input/release",     handle_input_release },
    { "POST",   "/input/stick",       handle_input_stick },
    { "POST",   "/input/touch",       handle_input_touch },
    { "POST",   "/input/clear",       handle_input_clear },
    { "POST",   "/controller/attach", handle_controller_attach },
    { "POST",   "/controller/detach", handle_controller_detach },
    { "POST",   "/controller/handshake", handle_controller_handshake },
    { "POST",   "/controller/pair",      handle_controller_handshake },
    { "GET",    "/files",             files_handle_get },
    { "GET",    "/files/hash",        files_handle_hash },
    { "PUT",    "/files",             files_handle_put },
    { "DELETE", "/files",             files_handle_delete },
    { "GET",    "/titles",            handle_titles },
    { "GET",    "/process",           handle_process_current },
    { "POST",   "/process/launch",    handle_process_launch },
    { "POST",   "/process/launch_title", handle_process_launch },
    { "POST",   "/process/launch-nro",handle_process_launch_nro },
    { "POST",   "/process/launch_nro",handle_process_launch_nro },
    { "POST",   "/process/terminate", handle_process_terminate },
    { "GET",    "/memory/layout",     handle_memory_layout },
    { "GET",    "/memory/query",      handle_memory_query },
    { "POST",   "/memory/query",      handle_memory_query },
    { "POST",   "/memory/peek",       handle_memory_peek },
    { "POST",   "/memory/read",       handle_memory_peek },
    { "POST",   "/memory/poke",       handle_memory_poke },
    { "POST",   "/memory/write",      handle_memory_poke },
    { "POST",   "/memory/pointer",    handle_memory_pointer },
    { "POST",   "/memory/search",     handle_memory_search },
    { "GET",    "/diagnostics/crash", handle_diagnostics_crash },
    { "GET",    "/network/dns",       handle_dns_get },
    { "POST",   "/network/dns",       handle_dns_set },
    { "POST",   "/install",           handle_install },
    { "PUT",    "/install",           handle_install },
    { "POST",   "/mcp",               mcp_handle_post },
    { "GET",    "/mcp",               mcp_handle_get },
    { "POST",   "/power/sleep",       handle_power_sleep },
    { "POST",   "/power/restart",     handle_power_restart },
    { "POST",   "/power/off",         handle_power_off },
    { "GET",    "/settings/theme",        handle_settings_theme },
    { "POST",   "/settings/theme",        handle_settings_theme },
    { "GET",    "/settings/nickname",     handle_settings_nickname },
    { "POST",   "/settings/nickname",     handle_settings_nickname },
    { "GET",    "/settings/brightness",   handle_settings_brightness },
    { "POST",   "/settings/brightness",   handle_settings_brightness },
    { "GET",    "/settings/volume",       handle_settings_volume },
    { "POST",   "/settings/volume",       handle_settings_volume },
    { "POST",   "/settings/airplane",     handle_settings_airplane },
    { "GET",    "/settings/auto-time",    handle_settings_auto_time },
    { "POST",   "/settings/auto-time",    handle_settings_auto_time },
    { "GET",    "/settings/datetime",     handle_settings_datetime },
    { "POST",   "/settings/datetime",     handle_settings_datetime },
    { "POST",   "/oauth/register",    oauth_handle_register },
    { "GET",    "/oauth/authorize",   oauth_handle_authorize_get },
    { "POST",   "/oauth/authorize",   oauth_handle_authorize_post },
    { "POST",   "/oauth/token",       oauth_handle_token },
};

// CORS preflight: browsers send OPTIONS with no Authorization header before
// cross-origin requests (e.g. web-based MCP clients).
static void handle_options(HttpRequest *req) {
    static const char hdr[] =
        "HTTP/1.1 204 No Content\r\n"
        "Server: sys-autopilot\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type, Mcp-Protocol-Version\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Connection: close\r\n"
        "\r\n";
    http_write_all(req->fd, hdr, sizeof(hdr) - 1);
}

void routes_handle(HttpRequest *req) {
    if (strcmp(req->method, "OPTIONS") == 0) {
        handle_options(req);
        return;
    }

    // OAuth discovery documents (clients probe both the bare path and the
    // RFC 9728 path-suffix variant, e.g. .../oauth-protected-resource/mcp).
    if (strcmp(req->method, "GET") == 0) {
        if (strncmp(req->path, "/.well-known/oauth-protected-resource", 38) == 0) {
            oauth_handle_protected_resource(req);
            return;
        }
        if (strncmp(req->path, "/.well-known/oauth-authorization-server", 39) == 0 ||
            strncmp(req->path, "/.well-known/openid-configuration", 33) == 0) {
            oauth_handle_as_metadata(req);
            return;
        }
    }

    bool path_found = false;
    for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); i++) {
        if (strcmp(req->path, kRoutes[i].path) != 0)
            continue;
        path_found = true;
        if (strcmp(req->method, kRoutes[i].method) == 0) {
            kRoutes[i].handler(req);
            return;
        }
    }
    if (path_found)
        http_send_error(req->fd, 405, "method not allowed");
    else
        http_send_error(req->fd, 404, "not found");
}
