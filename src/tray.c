#include "tray.h"
#include "vertere.h"

#include <unistd.h>

static constexpr unsigned char TRAY_INTROSPECTION_XML[] = {
#embed "../data/tray.xml"
    , 0
};

struct _VertereTray {
    GDBusNodeInfo *introspection;
    GDBusConnection *bus;
    char *bus_name;
    char *icons_dir;
    guint own_name_id;
    guint watcher_id;
    guint sni_reg_id;
    guint menu_reg_id;
    VertereTrayCallback on_action;
    gpointer user_data;
    GDestroyNotify destroy_data;
};

[[nodiscard]]
static char *resolve_icons_dir(void) {
    g_autofree char *exe = g_file_read_link("/proc/self/exe", nullptr);
    if (exe == nullptr) {
        return g_strdup("");
    }
    g_autofree char *bin_dir = g_path_get_dirname(exe);
    g_autofree char *prefix = g_path_get_dirname(bin_dir);
    return g_build_filename(prefix, "share", "icons", nullptr);
}

static GVariant *build_menu_props(int32_t id) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));

    switch (id) {
    case 0:
        g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
        break;
    case VERTERE_TRAY_SHOT:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Translate a screen region"));
        break;
    case VERTERE_TRAY_CLIP:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Translate the clipboard"));
        break;
    case VERTERE_TRAY_SELECTION:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Translate the selection"));
        break;
    case 4:
    case 7:
        g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
        break;
    case VERTERE_TRAY_HISTORY:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("History\xe2\x80\xa6"));
        break;
    case VERTERE_TRAY_SETTINGS:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Settings\xe2\x80\xa6"));
        break;
    case VERTERE_TRAY_QUIT:
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Quit"));
        break;
    default:
        break;
    }
    return g_variant_builder_end(&b);
}

static GVariant *build_menu_node(int32_t id, int32_t depth) {
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));

    if (id == 0 && depth != 0) {
        int32_t next_depth = depth > 0 ? depth - 1 : -1;
        static constexpr int32_t ITEM_IDS[] = {
            VERTERE_TRAY_SHOT,
            VERTERE_TRAY_CLIP,
            VERTERE_TRAY_SELECTION,
            4,
            VERTERE_TRAY_HISTORY,
            VERTERE_TRAY_SETTINGS,
            7,
            VERTERE_TRAY_QUIT,
        };
        for (size_t i = 0; i < _Countof(ITEM_IDS); i++) {
            g_variant_builder_add(&children, "v", build_menu_node(ITEM_IDS[i], next_depth));
        }
    }

    return g_variant_new("(i@a{sv}@av)", id, build_menu_props(id), g_variant_builder_end(&children));
}

static GVariant *sni_get_property(
    GDBusConnection *,
    const char *,
    const char *,
    const char *,
    const char *property_name,
    GError **,
    gpointer user_data
) {
    VertereTray *tray = user_data;
    if (g_strcmp0(property_name, "Category") == 0) {
        return g_variant_new_string("ApplicationStatus");
    }
    if (g_strcmp0(property_name, "Id") == 0) {
        return g_variant_new_string(VERTERE_APP_ID);
    }
    if (g_strcmp0(property_name, "Title") == 0) {
        return g_variant_new_string("Vertere");
    }
    if (g_strcmp0(property_name, "Status") == 0) {
        return g_variant_new_string("Active");
    }
    if (g_strcmp0(property_name, "WindowId") == 0) {
        return g_variant_new_uint32(0);
    }
    if (g_strcmp0(property_name, "IconName") == 0) {
        return g_variant_new_string(VERTERE_APP_ID);
    }
    if (g_strcmp0(property_name, "IconThemePath") == 0) {
        return g_variant_new_string(tray->icons_dir);
    }
    if (g_strcmp0(property_name, "Menu") == 0) {
        return g_variant_new_object_path("/MenuBar");
    }
    if (g_strcmp0(property_name, "ItemIsMenu") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    return nullptr;
}

static void sni_method_call(
    GDBusConnection *,
    const char *,
    const char *,
    const char *,
    const char *,
    GVariant *,
    GDBusMethodInvocation *invocation,
    gpointer
) {
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static void handle_menu_event(VertereTray *tray, int32_t id, const char *event_id) {
    if (g_strcmp0(event_id, "clicked") != 0 || tray->on_action == nullptr) {
        return;
    }
    switch (id) {
    case VERTERE_TRAY_SHOT:
    case VERTERE_TRAY_CLIP:
    case VERTERE_TRAY_SELECTION:
    case VERTERE_TRAY_HISTORY:
    case VERTERE_TRAY_SETTINGS:
    case VERTERE_TRAY_QUIT:
        tray->on_action((VertereTrayAction)id, tray->user_data);
        break;
    default:
        break;
    }
}

static GVariant *menu_get_property(
    GDBusConnection *,
    const char *,
    const char *,
    const char *,
    const char *property_name,
    GError **,
    gpointer
) {
    if (g_strcmp0(property_name, "Version") == 0) {
        return g_variant_new_uint32(3);
    }
    if (g_strcmp0(property_name, "TextDirection") == 0) {
        return g_variant_new_string("ltr");
    }
    if (g_strcmp0(property_name, "Status") == 0) {
        return g_variant_new_string("normal");
    }
    if (g_strcmp0(property_name, "IconThemePath") == 0) {
        const char *empty[] = {nullptr};
        return g_variant_new_strv(empty, 0);
    }
    return nullptr;
}

static void menu_method_call(
    GDBusConnection *,
    const char *,
    const char *,
    const char *,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation,
    gpointer user_data
) {
    VertereTray *tray = user_data;

    if (g_strcmp0(method_name, "GetLayout") == 0) {
        int32_t parent_id = 0;
        int32_t depth = -1;
        g_autoptr(GVariant) prop_names = nullptr;
        g_variant_get(parameters, "(ii@as)", &parent_id, &depth, &prop_names);

        GVariant *layout = build_menu_node(parent_id, depth);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@*)", 1U, layout));
        return;
    }

    if (g_strcmp0(method_name, "GetGroupProperties") == 0) {
        g_autoptr(GVariantIter) iter = nullptr;
        g_autoptr(GVariant) prop_names = nullptr;
        g_variant_get(parameters, "(ai@as)", &iter, &prop_names);

        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        int32_t id = 0;
        while (g_variant_iter_loop(iter, "i", &id)) {
            g_variant_builder_add(&out, "(i@a{sv})", id, build_menu_props(id));
        }
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&out)));
        return;
    }

    if (g_strcmp0(method_name, "GetProperty") == 0) {
        int32_t id = 0;
        const char *name = nullptr;
        g_variant_get(parameters, "(i&s)", &id, &name);

        g_autoptr(GVariant) props = g_variant_ref_sink(build_menu_props(id));
        g_autoptr(GVariant) val = g_variant_lookup_value(props, name, nullptr);
        if (val != nullptr) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", val));
        } else {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.DBus.Error.InvalidArgs", "unknown property");
        }
        return;
    }

    if (g_strcmp0(method_name, "Event") == 0) {
        int32_t id = 0;
        const char *event_id = nullptr;
        g_autoptr(GVariant) data = nullptr;
        uint32_t timestamp = 0;
        g_variant_get(parameters, "(i&svu)", &id, &event_id, &data, &timestamp);
        handle_menu_event(tray, id, event_id);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (g_strcmp0(method_name, "EventGroup") == 0) {
        g_autoptr(GVariantIter) iter = nullptr;
        g_variant_get(parameters, "(a(isvu))", &iter);
        int32_t id = 0;
        const char *event_id = nullptr;
        GVariant *data = nullptr;
        uint32_t timestamp = 0;
        while (g_variant_iter_loop(iter, "(i&svu)", &id, &event_id, &data, &timestamp)) {
            handle_menu_event(tray, id, event_id);
        }
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai)", g_variant_builder_end(&errors)));
        return;
    }

    if (g_strcmp0(method_name, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }

    if (g_strcmp0(method_name, "AboutToShowGroup") == 0) {
        GVariantBuilder empty1;
        GVariantBuilder empty2;
        g_variant_builder_init(&empty1, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&empty2, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(@ai@ai)",
                          g_variant_builder_end(&empty1),
                          g_variant_builder_end(&empty2)));
        return;
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static void on_registered(GObject *source, GAsyncResult *res, gpointer) {
    g_autoptr(GError) err = nullptr;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (reply == nullptr && !g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)) {
        g_printerr("vertere: cannot register tray item: %s\n", err->message);
    }
}

static void register_with_watcher(VertereTray *tray) {
    if (tray->bus == nullptr || tray->sni_reg_id == 0) {
        return;
    }
    g_dbus_connection_call(
        tray->bus,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher",
        "RegisterStatusNotifierItem",
        g_variant_new("(s)", tray->bus_name),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        on_registered,
        nullptr);
}

static void on_bus_acquired(GDBusConnection *connection, const char *, gpointer user_data) {
    VertereTray *tray = user_data;
    tray->bus = g_object_ref(connection);

    static const GDBusInterfaceVTable sni_vtable = {
        .method_call = sni_method_call,
        .get_property = sni_get_property,
    };
    static const GDBusInterfaceVTable menu_vtable = {
        .method_call = menu_method_call,
        .get_property = menu_get_property,
    };

    g_autoptr(GError) err = nullptr;
    GDBusInterfaceInfo *sni_iface = g_dbus_node_info_lookup_interface(
        tray->introspection, "org.kde.StatusNotifierItem");
    GDBusInterfaceInfo *menu_iface = g_dbus_node_info_lookup_interface(
        tray->introspection, "com.canonical.dbusmenu");

    tray->sni_reg_id = g_dbus_connection_register_object(
        connection, "/StatusNotifierItem", sni_iface, &sni_vtable, tray, nullptr, &err);
    if (tray->sni_reg_id == 0) {
        g_printerr("vertere: cannot register StatusNotifierItem: %s\n", err->message);
        return;
    }

    tray->menu_reg_id = g_dbus_connection_register_object(
        connection, "/MenuBar", menu_iface, &menu_vtable, tray, nullptr, &err);
    if (tray->menu_reg_id == 0) {
        g_printerr("vertere: cannot register DBusMenu: %s\n", err->message);
        return;
    }

    register_with_watcher(tray);
}

static void on_watcher_appeared(GDBusConnection *, const char *, const char *, gpointer user_data) {
    register_with_watcher(user_data);
}

VertereTray *vertere_tray_start(
    VertereTrayCallback on_action,
    gpointer user_data,
    GDestroyNotify destroy_data
) {
    g_autoptr(GError) err = nullptr;
    GDBusNodeInfo *introspection = g_dbus_node_info_new_for_xml(
        (const char *)TRAY_INTROSPECTION_XML, &err);
    if (introspection == nullptr) {
        g_printerr("vertere: cannot parse tray D-Bus XML: %s\n", err->message);
        if (destroy_data != nullptr && user_data != nullptr) {
            destroy_data(user_data);
        }
        return nullptr;
    }

    VertereTray *tray = g_new0(VertereTray, 1);
    tray->introspection = introspection;
    tray->bus_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());
    tray->icons_dir = resolve_icons_dir();
    tray->on_action = on_action;
    tray->user_data = user_data;
    tray->destroy_data = destroy_data;

    tray->own_name_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        tray->bus_name,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        nullptr,
        nullptr,
        tray,
        nullptr);

    tray->watcher_id = g_bus_watch_name(
        G_BUS_TYPE_SESSION,
        "org.kde.StatusNotifierWatcher",
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_watcher_appeared,
        nullptr,
        tray,
        nullptr);

    return tray;
}

void vertere_tray_stop(VertereTray *tray) {
    if (tray == nullptr) {
        return;
    }
    if (tray->watcher_id != 0) {
        g_bus_unwatch_name(tray->watcher_id);
    }
    if (tray->bus != nullptr) {
        if (tray->sni_reg_id != 0) {
            g_dbus_connection_unregister_object(tray->bus, tray->sni_reg_id);
        }
        if (tray->menu_reg_id != 0) {
            g_dbus_connection_unregister_object(tray->bus, tray->menu_reg_id);
        }
        g_clear_object(&tray->bus);
    }
    if (tray->own_name_id != 0) {
        g_bus_unown_name(tray->own_name_id);
    }
    if (tray->destroy_data != nullptr && tray->user_data != nullptr) {
        tray->destroy_data(tray->user_data);
    }
    g_clear_pointer(&tray->introspection, g_dbus_node_info_unref);
    g_free(tray->bus_name);
    g_free(tray->icons_dir);
    g_free(tray);
}
