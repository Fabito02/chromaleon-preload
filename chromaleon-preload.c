#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <gio/gio.h>

#define APPEARANCE_NS     "org.freedesktop.appearance"
#define ACCENT_KEY        "accent-color"
#define PORTAL_IFACE      "org.freedesktop.portal.Settings"
#define EXTENSION_UUID    "user-accent-colors@fabito02"
#define CHROMALEON_SCHEMA "org.gnome.shell.extensions.chromaleon"

typedef struct { double r, g, b; } AccentColor;

static gboolean should_hook(void) {
    static int cached = -1;
    if (cached != -1) return cached;

    const gchar *name = program_invocation_short_name;
    if (!name) return (cached = 0);

    gchar *path = g_build_filename(g_get_user_config_dir(), "ChromaLeon", "targets", NULL);
    gchar *raw = NULL;
    gboolean ok = g_file_get_contents(path, &raw, NULL, NULL);
    g_free(path);
    if (!ok || !raw) return (cached = 0);

    gchar **tokens = g_strsplit_set(raw, ", \t\r\n", -1);
    g_free(raw);

    cached = g_strv_contains((const gchar * const *)tokens, name) ? 1 : 0;
    g_strfreev(tokens);
    return cached;
}

static gboolean parse_hex(const char *hex, AccentColor *c) {
    if (!hex) return FALSE;
    if (*hex == '#') hex++;
    else if (g_ascii_strncasecmp(hex, "0x", 2) == 0) hex += 2;

    unsigned int r, g, b;
    if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) != 3) return FALSE;

    c->r = r / 255.0;
    c->g = g / 255.0;
    c->b = b / 255.0;
    return TRUE;
}

static gboolean get_accent_color(AccentColor *c) {
    static gboolean in_progress = FALSE;
    if (in_progress) return FALSE;
    in_progress = TRUE;

    gchar *dir = g_build_filename(g_get_user_data_dir(), "gnome-shell",
                                  "extensions", EXTENSION_UUID, "schemas", NULL);
    GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory(dir, NULL, TRUE, NULL);
    g_free(dir);

    gchar *hex = NULL;
    if (src) {
        GSettingsSchema *sc = g_settings_schema_source_lookup(src, CHROMALEON_SCHEMA, TRUE);
        g_settings_schema_source_unref(src);
        if (sc) {
            GSettings *s = g_settings_new_full(sc, NULL, NULL);
            g_settings_schema_unref(sc);
            hex = g_settings_get_string(s, ACCENT_KEY);
            g_object_unref(s);
        }
    }

    in_progress = FALSE;
    gboolean ok = parse_hex(hex, c);
    g_free(hex);
    return ok;
}

static GVariant *patch_appearance(GVariant *dict, const AccentColor *c) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

    if (dict) {
        GVariantIter iter;
        const gchar *key;
        GVariant *val;
        g_variant_iter_init(&iter, dict);
        while (g_variant_iter_next(&iter, "{&sv}", &key, &val)) {
            if (g_strcmp0(key, ACCENT_KEY) != 0)
                g_variant_builder_add(&b, "{sv}", key, val);
            g_variant_unref(val);
        }
    }

    g_variant_builder_add(&b, "{sv}", ACCENT_KEY, g_variant_new("(ddd)", c->r, c->g, c->b));
    return g_variant_builder_end(&b);
}

static GVariant *patch_read_all(GVariant *original, const AccentColor *c) {
    GVariant *dict = g_variant_get_child_value(original, 0);
    GVariantBuilder b;
    GVariantIter iter;
    const gchar *ns;
    GVariant *s;
    gboolean handled = FALSE;

    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_iter_init(&iter, dict);

    while (g_variant_iter_next(&iter, "{&s@a{sv}}", &ns, &s)) {
        if (g_strcmp0(ns, APPEARANCE_NS) == 0) {
            handled = TRUE;
            g_variant_builder_add(&b, "{s@a{sv}}", ns, patch_appearance(s, c));
        } else {
            g_variant_builder_add(&b, "{s@a{sv}}", ns, s);
        }
        g_variant_unref(s);
    }

    if (!handled)
        g_variant_builder_add(&b, "{s@a{sv}}", APPEARANCE_NS, patch_appearance(NULL, c));

    g_variant_unref(dict);
    g_variant_unref(original);

    GVariant *res = g_variant_builder_end(&b);
    return g_variant_new_tuple(&res, 1);
}

GVariant *g_dbus_proxy_call_sync(
    GDBusProxy *proxy, const gchar *method, GVariant *params,
    GDBusCallFlags flags, gint timeout, GCancellable *canc, GError **err
) {
    static GVariant *(*orig)(GDBusProxy *, const gchar *, GVariant *,
                             GDBusCallFlags, gint, GCancellable *, GError **) = NULL;
    if (!orig) orig = dlsym(RTLD_NEXT, "g_dbus_proxy_call_sync");

    gboolean is_read     = should_hook() && g_strcmp0(method, "Read") == 0;
    gboolean is_read_all = should_hook() && g_strcmp0(method, "ReadAll") == 0;
    AccentColor color;

    if (!(is_read || is_read_all) || !get_accent_color(&color))
        return orig(proxy, method, params, flags, timeout, canc, err);

    if (is_read && params) {
        const gchar *ns = NULL, *key = NULL;
        g_variant_get(params, "(&s&s)", &ns, &key);
        if (g_strcmp0(ns, APPEARANCE_NS) == 0 && g_strcmp0(key, ACCENT_KEY) == 0)
            return g_variant_new("(v)", g_variant_new("(ddd)", color.r, color.g, color.b));
    }

    GVariant *res = orig(proxy, method, params, flags, timeout, canc, err);
    return (is_read_all && res && (!err || !*err)) ? patch_read_all(res, &color) : res;
}

GVariant *g_dbus_proxy_call_finish(GDBusProxy *proxy, GAsyncResult *res, GError **err) {
    static GVariant *(*orig)(GDBusProxy *, GAsyncResult *, GError **) = NULL;
    if (!orig) orig = dlsym(RTLD_NEXT, "g_dbus_proxy_call_finish");

    GVariant *result = orig(proxy, res, err);
    if (!result || (err && *err) || !should_hook()) return result;

    AccentColor color;
    if (g_strcmp0(g_dbus_proxy_get_interface_name(proxy), PORTAL_IFACE) == 0 &&
        g_variant_type_equal(g_variant_get_type(result), G_VARIANT_TYPE("(a{sa{sv}})")) &&
        get_accent_color(&color))
        return patch_read_all(result, &color);

    return result;
}
