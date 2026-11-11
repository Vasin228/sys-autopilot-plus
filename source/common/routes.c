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
#include "server.h"

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
        char name[128] = {0};
        if (!settings_get_nickname(name, sizeof(name))) {
            http_send_error(req->fd, 500, "failed to read nickname");
            return;
        }
        http_send_json(req->fd, 200, "{\"nickname\":\"%s\"}", name);
        return;
    }
    static JsonDoc doc;
    int root = read_json_body(req, &doc);
    if (root < 0)
        return;
    char name[128] = {0};
    int t = json_obj_get(&doc, root, "nickname");
    if (t < 0 || !json_get_string(&doc, t, name, sizeof(name)) || name[0] == '\0') {
        http_send_error(req->fd, 400, "missing non-empty 'nickname'");
        return;
    }
    if (!settings_set_nickname(name))
        http_send_error(req->fd, 500, "failed to set nickname");
    else
        http_send_json(req->fd, 200, "{\"ok\":true,\"nickname\":\"%s\"}", name);
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
    http_send_json(req->fd, 200, "{\"ok\":true,\"pid\":%llu,\"path\":\"%s\"}",
                   (unsigned long long)pid, path);
}

static void handle_process_terminate(HttpRequest *req) {
    char err[128] = {0};
    if (!process_terminate(err, sizeof(err))) {
        http_send_error(req->fd, 500, err[0] ? err : "failed to terminate process");
        return;
    }
    http_send_json(req->fd, 200, "{\"ok\":true}");
}

// --- /diagnostics/* -----------------------------------------------------------

static void handle_diagnostics_crash(HttpRequest *req) {
    char tid_str[32] = {0}, clear_str[16] = {0};
    http_query_get(req, "title_id", tid_str, sizeof(tid_str));
    if (!tid_str[0]) http_query_get(req, "titleId", tid_str, sizeof(tid_str));
    http_query_get(req, "clear", clear_str, sizeof(clear_str));

    u64 tid = tid_str[0] ? strtoull(tid_str, NULL, 16) : 0;
    bool clear = (strcmp(clear_str, "true") == 0 || strcmp(clear_str, "1") == 0);

    static char log_buf[4096];
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
    http_send_json(req->fd, 200, "{\"found\":true,\"filename\":\"%s\",\"content\":\"%s\"}",
                   filename, escaped);
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
