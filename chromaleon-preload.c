#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <glib.h>
#include <gio/gio.h>

#define APPEARANCE_NS     "org.freedesktop.appearance"
#define ACCENT_KEY        "accent-color"
#define PORTAL_IFACE      "org.freedesktop.portal.Settings"
#define EXTENSION_UUID    "user-accent-colors@fabito02"
#define CHROMALEON_SCHEMA "org.gnome.shell.extensions.chromaleon"
#define CHROMALEON_PATH   "/org/gnome/shell/extensions/chromaleon/"

typedef struct {
    double r, g, b;
} AccentColor;

static gboolean parse_hex_color(const char *hex, AccentColor *color) {
    if (!hex) return FALSE;
    if (*hex == '#') hex++;

    int ri, gi, bi;
    if (g_ascii_strncasecmp(hex, "0x", 2) == 0) hex += 2;

    if (sscanf(hex, "%02x%02x%02x", &ri, &gi, &bi) == 3) {
        color->r = ri / 255.0;
        color->g = gi / 255.0;
        color->b = bi / 255.0;
        return TRUE;
    }
    return FALSE;
}

static GVariant *build_accent_variant(const AccentColor *color) {
    return g_variant_new_variant(g_variant_new("(ddd)", color->r, color->g, color->b));
}

/* Settings.Read returns (v), requiring double-variant encapsulation: (v -> v -> (ddd)) */
static GVariant *build_read_response(const AccentColor *color) {
    GVariant *var = g_variant_new_variant(build_accent_variant(color));
    return g_variant_new_tuple(&var, 1);
}

static GSettings *get_chromaleon_settings(void) {
    GSettingsSchemaSource *default_source = g_settings_schema_source_get_default();
    GSettingsSchema *schema = NULL;

    if (default_source) {
        schema = g_settings_schema_source_lookup(default_source, CHROMALEON_SCHEMA, TRUE);
    }

    if (!schema) {
        const gchar *user_dir = g_get_user_data_dir();
        const gchar *const *sys_dirs = g_get_system_data_dirs();
        guint total_dirs = sys_dirs ? g_strv_length((gchar **)sys_dirs) + 1 : 1;

        for (guint i = 0; i < total_dirs && !schema; i++) {
            const gchar *base = (i == 0) ? user_dir : sys_dirs[i - 1];
            gchar *path = g_build_filename(base, "gnome-shell", "extensions", EXTENSION_UUID, "schemas", NULL);

            if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
                GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory(path, default_source, TRUE, NULL);
                if (src) {
                    schema = g_settings_schema_source_lookup(src, CHROMALEON_SCHEMA, TRUE);
                    g_settings_schema_source_unref(src);
                }
            }
            g_free(path);
        }
    }

    if (!schema) return NULL;

    const gchar *path = g_settings_schema_get_path(schema);
    GSettings *settings = g_settings_new_full(schema, NULL, path ? NULL : CHROMALEON_PATH);
    g_settings_schema_unref(schema);

    return settings;
}

static gboolean get_chromaleon_accent_color(AccentColor *color) {
    const char *env = g_getenv("CHROMALEON_ACCENT");
    if (env && parse_hex_color(env, color)) {
        return TRUE;
    }

    GSettings *settings = get_chromaleon_settings();
    if (!settings) return FALSE;

    gchar *hex_str = g_settings_get_string(settings, ACCENT_KEY);
    g_object_unref(settings);

    gboolean valid = parse_hex_color(hex_str, color);
    g_free(hex_str);
    return valid;
}

static GVariant *modify_appearance_dict(GVariant *dict, const AccentColor *color) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    gboolean injected = FALSE;

    if (dict) {
        GVariantIter iter;
        g_variant_iter_init(&iter, dict);
        const gchar *key;
        GVariant *val;

        while (g_variant_iter_next(&iter, "{&s@v}", &key, &val)) {
            if (g_strcmp0(key, ACCENT_KEY) == 0) {
                g_variant_builder_add(&builder, "{s@v}", key, build_accent_variant(color));
                injected = TRUE;
            } else {
                g_variant_builder_add(&builder, "{s@v}", key, val);
            }
            g_variant_unref(val);
        }
    }

    if (!injected) {
        g_variant_builder_add(&builder, "{s@v}", ACCENT_KEY, build_accent_variant(color));
    }

    return g_variant_builder_end(&builder);
}

static GVariant *modify_read_all_response(GVariant *original_res, const AccentColor *color) {
    if (!original_res) return NULL;

    GVariant *dict = g_variant_get_child_value(original_res, 0);
    GVariantBuilder root_builder;
    g_variant_builder_init(&root_builder, G_VARIANT_TYPE("a{sa{sv}}"));

    GVariantIter iter;
    g_variant_iter_init(&iter, dict);
    const gchar *ns;
    GVariant *settings;
    gboolean handled = FALSE;

    while (g_variant_iter_next(&iter, "{&s@a{sv}}", &ns, &settings)) {
        if (g_strcmp0(ns, APPEARANCE_NS) == 0) {
            handled = TRUE;
            g_variant_builder_add(&root_builder, "{s@a{sv}}", ns, modify_appearance_dict(settings, color));
        } else {
            g_variant_builder_add(&root_builder, "{s@a{sv}}", ns, settings);
        }
        g_variant_unref(settings);
    }

    if (!handled) {
        g_variant_builder_add(&root_builder, "{s@a{sv}}", APPEARANCE_NS, modify_appearance_dict(NULL, color));
    }

    g_variant_unref(dict);
    g_variant_unref(original_res);

    GVariant *res = g_variant_builder_end(&root_builder);
    return g_variant_new_tuple(&res, 1);
}

GVariant *g_dbus_proxy_call_sync(
    GDBusProxy *proxy,
    const gchar *method_name,
    GVariant *parameters,
    GDBusCallFlags flags,
    gint timeout_msec,
    GCancellable *cancellable,
    GError **error
) {
    static GVariant *(*orig_func)(GDBusProxy *, const gchar *, GVariant *, GDBusCallFlags, gint, GCancellable *, GError **) = NULL;
    if (!orig_func) orig_func = dlsym(RTLD_NEXT, "g_dbus_proxy_call_sync");

    gboolean is_read = (g_strcmp0(method_name, "Read") == 0);
    gboolean is_read_all = (g_strcmp0(method_name, "ReadAll") == 0);

    if (!is_read && !is_read_all) {
        return orig_func(proxy, method_name, parameters, flags, timeout_msec, cancellable, error);
    }

    AccentColor color;
    if (!get_chromaleon_accent_color(&color)) {
        return orig_func(proxy, method_name, parameters, flags, timeout_msec, cancellable, error);
    }

    if (is_read && parameters) {
        const gchar *ns = NULL, *key = NULL;
        g_variant_get(parameters, "(&s&s)", &ns, &key);

        if (g_strcmp0(ns, APPEARANCE_NS) == 0 && g_strcmp0(key, ACCENT_KEY) == 0) {
            return build_read_response(&color);
        }
    }

    GVariant *res = orig_func(proxy, method_name, parameters, flags, timeout_msec, cancellable, error);

    if (is_read_all && res && (!error || !*error)) {
        return modify_read_all_response(res, &color);
    }

    return res;
}

GVariant *g_dbus_proxy_call_finish(
    GDBusProxy *proxy,
    GAsyncResult *res,
    GError **error
) {
    static GVariant *(*orig_func)(GDBusProxy *, GAsyncResult *, GError **) = NULL;
    if (!orig_func) orig_func = dlsym(RTLD_NEXT, "g_dbus_proxy_call_finish");

    GVariant *result = orig_func(proxy, res, error);
    if (!result || (error && *error)) return result;

    const gchar *iface = g_dbus_proxy_get_interface_name(proxy);
    if (g_strcmp0(iface, PORTAL_IFACE) != 0) return result;

    const GVariantType *type = g_variant_get_type(result);
    if (!g_variant_type_equal(type, G_VARIANT_TYPE("(a{sa{sv}})"))) return result;

    AccentColor color;
    if (!get_chromaleon_accent_color(&color)) return result;

    return modify_read_all_response(result, &color);
}
