#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <gio/gio.h>

#define APPEARANCE_NS     "org.freedesktop.appearance"
#define ACCENT_KEY        "accent-color"
#define TARGET_KEY        "target-apps"
#define PORTAL_IFACE      "org.freedesktop.portal.Settings"
#define EXTENSION_UUID    "user-accent-colors@fabito02"
#define CHROMALEON_SCHEMA "org.gnome.shell.extensions.chromaleon"

typedef struct { double r, g, b; } AccentColor;

static GSettings *get_settings(void) {
    gchar *dir = g_build_filename(g_get_user_data_dir(), "gnome-shell", "extensions", EXTENSION_UUID, "schemas", NULL);
    GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory(dir, NULL, TRUE, NULL);
    g_free(dir);
    if (!src) return NULL;

    GSettingsSchema *schema = g_settings_schema_source_lookup(src, CHROMALEON_SCHEMA, TRUE);
    g_settings_schema_source_unref(src);
    if (!schema) return NULL;

    GSettings *settings = g_settings_new_full(schema, NULL, NULL);
    g_settings_schema_unref(schema);
    return settings;
}

static gboolean should_hook(void) {
    static int is_target = -1;
    if (is_target != -1) return is_target;

    if (!program_invocation_short_name) return (is_target = 0);

    GSettings *settings = get_settings();
    if (!settings) return (is_target = 0);

    gchar **apps = g_settings_get_strv(settings, TARGET_KEY);
    g_object_unref(settings);

    is_target = (apps && g_strv_contains((const gchar *const *)apps, program_invocation_short_name)) ? 1 : 0;
    g_strfreev(apps);

    return is_target;
}

static gboolean get_accent_color(AccentColor *color) {
    const char *env = g_getenv("CHROMALEON_ACCENT");
    gchar *hex = NULL;

    if (!env) {
        GSettings *settings = get_settings();
        if (!settings) return FALSE;
        hex = g_settings_get_string(settings, ACCENT_KEY);
        g_object_unref(settings);
    }

    const char *raw = env ? env : hex;
    if (!raw) return FALSE;

    if (*raw == '#') raw++;
    else if (g_ascii_strncasecmp(raw, "0x", 2) == 0) raw += 2;

    unsigned int r, g, b;
    gboolean valid = (sscanf(raw, "%02x%02x%02x", &r, &g, &b) == 3);
    if (valid) {
        color->r = r / 255.0;
        color->g = g / 255.0;
        color->b = b / 255.0;
    }

    g_free(hex);
    return valid;
}

static GVariant *patch_appearance_dict(GVariant *dict, const AccentColor *c) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

    if (dict) {
        GVariantIter iter;
        g_variant_iter_init(&iter, dict);
        const gchar *key;
        GVariant *val;

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
    if (!original) return NULL;

    GVariant *dict = g_variant_get_child_value(original, 0);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sa{sv}}"));

    GVariantIter iter;
    g_variant_iter_init(&iter, dict);
    const gchar *ns;
    GVariant *settings;
    gboolean handled = FALSE;

    while (g_variant_iter_next(&iter, "{&s@a{sv}}", &ns, &settings)) {
        if (g_strcmp0(ns, APPEARANCE_NS) == 0) {
            handled = TRUE;
            g_variant_builder_add(&b, "{s@a{sv}}", ns, patch_appearance_dict(settings, c));
        } else {
            g_variant_builder_add(&b, "{s@a{sv}}", ns, settings);
        }
        g_variant_unref(settings);
    }

    if (!handled)
        g_variant_builder_add(&b, "{s@a{sv}}", APPEARANCE_NS, patch_appearance_dict(NULL, c));

    g_variant_unref(dict);
    g_variant_unref(original);

    GVariant *res = g_variant_builder_end(&b);
    return g_variant_new_tuple(&res, 1);
}

GVariant *g_dbus_proxy_call_sync(
    GDBusProxy *proxy, const gchar *method, GVariant *params,
    GDBusCallFlags flags, gint timeout, GCancellable *canc, GError **err
) {
    static GVariant *(*orig)(GDBusProxy *, const gchar *, GVariant *, GDBusCallFlags, gint, GCancellable *, GError **) = NULL;
    if (!orig) orig = dlsym(RTLD_NEXT, "g_dbus_proxy_call_sync");

    if (!should_hook()) return orig(proxy, method, params, flags, timeout, canc, err);

    gboolean is_read = (g_strcmp0(method, "Read") == 0);
    gboolean is_read_all = (g_strcmp0(method, "ReadAll") == 0);

    AccentColor color;
    if ((is_read || is_read_all) && get_accent_color(&color)) {
        if (is_read && params) {
            const gchar *ns = NULL, *key = NULL;
            g_variant_get(params, "(&s&s)", &ns, &key);
            if (g_strcmp0(ns, APPEARANCE_NS) == 0 && g_strcmp0(key, ACCENT_KEY) == 0)
                return g_variant_new("(v)", g_variant_new("(ddd)", color.r, color.g, color.b));
        }

        GVariant *res = orig(proxy, method, params, flags, timeout, canc, err);
        return (is_read_all && res && (!err || !*err)) ? patch_read_all(res, &color) : res;
    }

    return orig(proxy, method, params, flags, timeout, canc, err);
}

GVariant *g_dbus_proxy_call_finish(GDBusProxy *proxy, GAsyncResult *res, GError **err) {
    static GVariant *(*orig)(GDBusProxy *, GAsyncResult *, GError **) = NULL;
    if (!orig) orig = dlsym(RTLD_NEXT, "g_dbus_proxy_call_finish");

    GVariant *result = orig(proxy, res, err);
    if (!result || (err && *err) || !should_hook()) return result;

    if (g_strcmp0(g_dbus_proxy_get_interface_name(proxy), PORTAL_IFACE) == 0 &&
        g_variant_type_equal(g_variant_get_type(result), G_VARIANT_TYPE("(a{sa{sv}})"))) {
        AccentColor color;
        if (get_accent_color(&color)) return patch_read_all(result, &color);
    }

    return result;
}
