#include "apiargs.h"
#include "buttons.h"

#include <string.h>
#include <strings.h>

bool args_get_buttons(const JsonDoc *doc, int obj, uint64_t *out_mask,
                      const char **err) {
    int arr = json_obj_get(doc, obj, "buttons");
    if (arr < 0) {
        *err = "missing 'buttons' (array of button names)";
        return false;
    }
    int n = json_arr_len(doc, arr);
    if (n <= 0) {
        *err = "'buttons' must be a non-empty array";
        return false;
    }
    uint64_t mask = 0;
    for (int i = 0; i < n; i++) {
        int tok = json_arr_get(doc, arr, i);
        char name[24];
        uint64_t m;
        if (!json_get_string(doc, tok, name, sizeof(name)) ||
            !button_from_name(name, &m)) {
            *err = "unknown button name in 'buttons'";
            return false;
        }
        mask |= m;
    }
    *out_mask = mask;
    return true;
}

int args_get_duration(const JsonDoc *doc, int obj, int fallback) {
    long long v;
    int tok = json_obj_get(doc, obj, "durationMs");
    if (tok >= 0 && json_get_int(doc, tok, &v))
        return (int)v;
    return fallback;
}

bool args_get_stick(const JsonDoc *doc, int obj, int *out_side, float *out_x,
                    float *out_y, int *out_duration, const char **err) {
    char side[16] = "";
    int tok = json_obj_get(doc, obj, "side");
    if (tok < 0 || !json_get_string(doc, tok, side, sizeof(side))) {
        *err = "missing 'side' (\"left\" or \"right\")";
        return false;
    }
    if (strcasecmp(side, "left") == 0)
        *out_side = 0;
    else if (strcasecmp(side, "right") == 0)
        *out_side = 1;
    else {
        *err = "invalid 'side' (\"left\" or \"right\")";
        return false;
    }

    double x = 0.0, y = 0.0;
    tok = json_obj_get(doc, obj, "x");
    if (tok >= 0 && !json_get_double(doc, tok, &x)) {
        *err = "'x' must be a number";
        return false;
    }
    tok = json_obj_get(doc, obj, "y");
    if (tok >= 0 && !json_get_double(doc, tok, &y)) {
        *err = "'y' must be a number";
        return false;
    }
    *out_x = (float)x;
    *out_y = (float)y;
    *out_duration = args_get_duration(doc, obj, 0);
    return true;
}

static bool get_coord(const JsonDoc *doc, int obj, const char *key, int max_val,
                      bool required, bool *present, int *out, const char **err) {
    int tok = json_obj_get(doc, obj, key);
    if (tok < 0) {
        if (present) *present = false;
        if (required) {
            *err = "missing 'x' or 'y' touchscreen coordinates";
            return false;
        }
        return true;
    }
    long long v = 0;
    if (!json_get_int(doc, tok, &v) || v < 0 || v >= max_val) {
        *err = "touchscreen coordinates must be integers in 0..1279 (x) and 0..719 (y)";
        return false;
    }
    if (present) *present = true;
    *out = (int)v;
    return true;
}

bool args_get_touch(const JsonDoc *doc, int obj, int *out_x, int *out_y,
                    int *out_x2, int *out_y2, int *out_duration,
                    int default_duration, const char **err) {
    if (!get_coord(doc, obj, "x", 1280, true, NULL, out_x, err) ||
        !get_coord(doc, obj, "y", 720, true, NULL, out_y, err))
        return false;

    bool has_x2 = false, has_y2 = false;
    int x2 = -1, y2 = -1;
    if (!get_coord(doc, obj, "x2", 1280, false, &has_x2, &x2, err) ||
        !get_coord(doc, obj, "y2", 720, false, &has_y2, &y2, err))
        return false;

    if (has_x2 != has_y2) {
        *err = "both 'x2' and 'y2' are required for a swipe";
        return false;
    }
    *out_x2 = has_x2 ? x2 : -1;
    *out_y2 = has_y2 ? y2 : -1;
    *out_duration = args_get_duration(doc, obj, default_duration);
    return true;
}
