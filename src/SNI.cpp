#include "SNI.h"
#include "Log.h"
#include "Widget.h"
#include "Config.h"

#ifdef WITH_SNI

#include <sni-watcher.h>
#include <sni-item.h>
#include <gio/gio.h>
#include <libdbusmenu-gtk/menu.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <unordered_set>

namespace SNI
{
    sniWatcher* watcherSkeleton;
    guint watcherID;
    GDBusConnection* dbusConnection = nullptr;

    guint hostID;

    struct Item
    {
        std::string name;
        std::string object;
        size_t w = 0;
        size_t h = 0;
        GdkPixbuf* pixbuf = nullptr;

        std::string tooltip = "";

        std::string menuObjectPath = "";

        Texture* textureWidget = nullptr;
        EventBox* gtkEvent = nullptr;
        GtkMenu* dbusMenu = nullptr;

        int watcherID = -1;
        int propertyChangeWatcherID = -1;

        bool gathering = false;
        std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    };
    std::vector<std::unique_ptr<Item>> items;

    std::unordered_map<std::string, Item> clientsToQuery;
    std::unordered_set<std::string> reloadedNames;

    // Gtk stuff
    // TODO: Investigate if the two box approach is still needed, since we now actually add/delete items
    Widget* parentBox;
    Widget* iconBox;

    // Swap channels for the render format and create a pixbuf out of it
    static GdkPixbuf* ToPixbuf(uint8_t* sniData, int32_t width, int32_t height)
    {
        for (size_t i = 0; i < size_t(width) * size_t(height); i++)
        {
            struct Px
            {
                // This should be bgra...
                // Since source is ARGB32 in network order(=big-endian)
                // and x86 Linux is little-endian, we *should* swap b and r...
                uint8_t a, r, g, b;
            };
            Px& pixel = ((Px*)sniData)[i];
            // Swap to create rgba
            pixel = {pixel.r, pixel.g, pixel.b, pixel.a};
        }
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_data(
            sniData, GDK_COLORSPACE_RGB, true, 8, width, height, width * 4,
            +[](uint8_t* data, void*)
            {
                delete[] data;
            },
            nullptr);
        if (!pixbuf)
            delete[] sniData;
        return pixbuf;
    }

    static bool ItemMatchesFilter(const Item& item, const std::string& filter, bool& wasExplicitOverride)
    {
        auto& filterString = item.tooltip == "" ? item.object : item.tooltip;
        if (filterString.find(filter) != std::string::npos)
        {
            wasExplicitOverride = true;
            return true;
        }
        else if (filter == "*" && !wasExplicitOverride)
        {
            return true;
        }
        return false;
    }

    template<typename OnFinishFn>
    inline void GatherItemProperties(Item& item, OnFinishFn&& onFinish)
    {
        // Only gather once.
        if (item.gathering)
            return;
        item.gathering = true;
        struct AsyncData
        {
            Item& item;
            std::decay_t<OnFinishFn> onFinish;
            std::weak_ptr<int> lifetime;
        };
        auto onAsyncResult = [](GObject* source, GAsyncResult* result, void* dataPtr)
        {
            std::unique_ptr<AsyncData> data((AsyncData*)dataPtr);

            GError* err = nullptr;
            g_autoptr(GVariant) allPropertiesWrapped = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &err);
            if (!data->lifetime.expired())
                data->item.gathering = false;
            if (err)
            {
                LOG("SNI: g_dbus_connection_call failed with: " << err->message);
                g_error_free(err);
                return;
            }
            if (data->lifetime.expired() || !allPropertiesWrapped)
                return;

            // Unwrap tuple
            g_autoptr(GVariant) allProperties = g_variant_get_child_value(allPropertiesWrapped, 0);
            auto getProperty = [&](const std::string_view& prop)
            {
                return g_variant_lookup_value(allProperties, prop.data(), nullptr);
            };

            // Query tooltip(Steam e.g. doesn't have one)
            GVariant* tooltip = getProperty("ToolTip");
            if (tooltip)
            {
                const gchar* title = nullptr;
                if (g_variant_is_of_type(tooltip, G_VARIANT_TYPE("(sa(iiay)ss)")))
                {
                    // According to spec, ToolTip is of type (sa(iiab)ss) => 4 children
                    // Most icons only set the "title" component (e.g. Discord, KeePassXC, ...)
                    g_variant_get_child(tooltip, 2, "&s", &title);
                }
                else if (g_variant_is_of_type(tooltip, G_VARIANT_TYPE_STRING))
                {
                    // TeamViewer only exposes a string, which is not according to spec!
                    title = g_variant_get_string(tooltip, nullptr);
                }

                if (title != nullptr)
                {
                    data->item.tooltip = title;
                }
                else
                {
                    LOG("SNI: Error querying tooltip");
                }
                LOG("SNI: Tooltip: " << data->item.tooltip);
                g_variant_unref(tooltip);
            }

            if (data->item.tooltip.empty())
            {
                LOG("SNI: No tooltip found, using title as tooltip");
                // No tooltip, use title as tooltip
                g_autoptr(GVariant) title = getProperty("Title");
                if (title && g_variant_is_of_type(title, G_VARIANT_TYPE_STRING))
                {
                    const gchar* titleStr = g_variant_get_string(title, nullptr);
                    if (titleStr != nullptr)
                    {
                        data->item.tooltip = titleStr;
                    }
                    else
                    {
                        LOG("SNI: Error querying title");
                    }
                    LOG("SNI: Fallback tooltip: " << data->item.tooltip);
                }
            }

            // Query menu
            g_autoptr(GVariant) menuPath = getProperty("Menu");
            if (menuPath && g_variant_is_of_type(menuPath, G_VARIANT_TYPE_OBJECT_PATH))
            {
                const char* objectPath;
                g_variant_get(menuPath, "&o", &objectPath);
                LOG("SNI: Menu object path: " << objectPath);

                data->item.menuObjectPath = objectPath;

            }

            bool wasExplicitOverride = false;
            for (auto& [filter, disabled] : Config::Get().sniDisabled)
            {
                if (ItemMatchesFilter(data->item, filter, wasExplicitOverride))
                {
                    if (disabled)
                    {
                        LOG("SNI: Disabling item due to config");
                        // We're done here.
                        return;
                    }
                }
            }

            // First try icon theme querying
            std::string iconName;
            wasExplicitOverride = false;
            for (auto& [filter, name] : Config::Get().sniIconNames)
            {
                if (ItemMatchesFilter(data->item, filter, wasExplicitOverride))
                {
                    iconName = name;
                }
            }
            if (iconName == "")
            {
                g_autoptr(GVariant) iconNameVar = getProperty("IconName");
                if (iconNameVar && g_variant_is_of_type(iconNameVar, G_VARIANT_TYPE_STRING))
                {
                    iconName = g_variant_get_string(iconNameVar, nullptr);

                }
            }
            bool gotPixbuf = false;
            if (iconName != "")
            {
                GdkPixbuf* pixbuf = nullptr;
                if (std::filesystem::path(iconName).is_absolute())
                {
                    // The icon name is an absolute path. This is not according to spec, but some apps (e.g. Spotube) still do it this way.
                    LOG("SNI: Warning: IconName shouldn't be a full path!");
                    GError* err = nullptr;
                    pixbuf = gdk_pixbuf_new_from_file(iconName.c_str(), &err);
                    if (err)
                    {
                        LOG("SNI: gdk_pixbuf_new_from_file failed: " << err->message);
                        g_error_free(err);
                    }
                }
                else
                {
                    GError* err = nullptr;
                    GtkIconTheme* defaultTheme = gtk_icon_theme_get_default();
                    pixbuf = gtk_icon_theme_load_icon(defaultTheme, iconName.c_str(), 64, GTK_ICON_LOOKUP_FORCE_SVG, &err);
                    if (err)
                    {
                        LOG("SNI: gtk_icon_theme_load_icon failed: " << err->message);
                        g_error_free(err);
                    }
                }

                if (pixbuf)
                {
                    LOG("SNI: Creating icon from \"" << iconName << "\"");
                    g_clear_object(&data->item.pixbuf);
                    data->item.pixbuf = pixbuf;
                    data->item.w = gdk_pixbuf_get_width(pixbuf);
                    data->item.h = gdk_pixbuf_get_height(pixbuf);
                    gotPixbuf = true;
                }
            }

            if (!gotPixbuf)
            {
                // IconName failed to load, try IconPixmap as a fallback
                g_autoptr(GVariant) iconPixmap = getProperty("IconPixmap");
                if (!iconPixmap || !g_variant_is_of_type(iconPixmap, G_VARIANT_TYPE("a(iiay)")))
                {
                    // All icon locations have failed, bail.
                    LOG("SNI: Cannot create item due to missing icon!");
                    return;
                }
                for (gsize i = 0; i < g_variant_n_children(iconPixmap); ++i)
                {
                    int width;
                    int height;
                    g_autoptr(GVariant) entry = g_variant_get_child_value(iconPixmap, i);
                    g_autoptr(GVariant) bytes = nullptr;
                    g_variant_get(entry, "(ii@ay)", &width, &height, &bytes);
                    gsize length = 0;
                    const void* pixels = g_variant_get_fixed_array(bytes, &length, 1);
                    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4 ||
                        size_t(height) > std::numeric_limits<size_t>::max() / (size_t(width) * 4) ||
                        length != size_t(width) * size_t(height) * 4)
                    {
                        LOG("SNI: Invalid icon pixmap dimensions or byte count");
                        continue;
                    }
                    uint8_t* iconData = new uint8_t[length];
                    memcpy(iconData, pixels, length);
                    GdkPixbuf* pixbuf = ToPixbuf(iconData, width, height);
                    if (!pixbuf)
                        continue;
                    g_clear_object(&data->item.pixbuf);
                    data->item.pixbuf = pixbuf;
                    data->item.w = width;
                    data->item.h = height;
                    LOG("SNI: Creating icon from pixmap");
                    break;
                }
            }

            if (data->item.pixbuf)
                data->onFinish(data->item);
        };

        // The tuples will be owned by g_dbus_connection_call, so no cleanup needed
        AsyncData* data = new AsyncData{item, onFinish, item.lifetime};
        GError* err = nullptr;
        GVariant* params[1];
        params[0] = g_variant_new_string("org.kde.StatusNotifierItem");
        GVariant* paramsTuple = g_variant_new_tuple(params, 1);
        g_dbus_connection_call(dbusConnection, item.name.c_str(), item.object.c_str(), "org.freedesktop.DBus.Properties", "GetAll", paramsTuple,
                               G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, +onAsyncResult, data);
        if (err)
        {
            LOG("SNI: g_dbus_connection_call failed with: " << err->message);
            g_error_free(err);
        }
    }

    static void DestroyItem(Item& item)
    {
        item.lifetime.reset();
        if (item.watcherID != -1)
            g_bus_unwatch_name(item.watcherID);
        if (item.propertyChangeWatcherID != -1)
            g_dbus_connection_signal_unsubscribe(dbusConnection, item.propertyChangeWatcherID);
        g_clear_object(&item.pixbuf);
        // dbus menu will be deleted by automatically when the parent widget is destroyed
    }

    static void AddSNIItem(Item& item)
    {
        if (!item.pixbuf)
            return;
        auto eventBox = Widget::Create<EventBox>();
        item.gtkEvent = eventBox.get();

        eventBox->SetOnCreate(
            [&](Widget& w)
            {
                // Create the menu if it wasn't already created
                if (!item.dbusMenu)
                {
                    item.dbusMenu = (GtkMenu*)dbusmenu_gtkmenu_new(item.name.data(), item.menuObjectPath.data());
                    gtk_menu_attach_to_widget(item.dbusMenu, item.gtkEvent->Get(), nullptr);
                }
                auto clickFn = [](GtkWidget*, GdkEventButton* event, void* data) -> gboolean
                {
                    if (event->button == 1)
                    {
                        GtkMenu* menu = (GtkMenu*)data;

                        gtk_menu_popup_at_pointer(menu, (GdkEvent*)event);
                        LOG("SNI: Opened popup!");
                    }
                    return GDK_EVENT_STOP;
                };
                g_signal_connect(w.Get(), "button-release-event", G_CALLBACK(+clickFn), item.dbusMenu);
            });

        LOG("SNI: Add " << item.name << " to widget");
        auto texture = Widget::Create<Texture>();
        bool wasExplicitOverride = false;
        int size = 24;
        for (auto& [filter, iconSize] : Config::Get().sniIconSizes)
        {
            if (ItemMatchesFilter(item, filter, wasExplicitOverride))
            {
                size = iconSize;
            }
        }
        wasExplicitOverride = false;
        for (auto& [filter, padding] : Config::Get().sniPaddingTop)
        {
            if (ItemMatchesFilter(item, filter, wasExplicitOverride))
            {
                texture->AddPaddingTop(padding);
            }
        }
        bool rotatedIcons = (Config::Get().location == 'L' || Config::Get().location == 'R') && Config::Get().iconsAlwaysUp;
        Utils::SetTransform(*texture, {size, true, Alignment::Fill}, {size, true, Alignment::Fill, 0, rotatedIcons ? 6 : 0});
        texture->SetBuf(item.pixbuf, item.w, item.h);
        texture->SetTooltip(item.tooltip);
        texture->SetAngle(Utils::GetAngle() == 270 ? 90 : 0);

        item.textureWidget = texture.get();

        eventBox->AddChild(std::move(texture));
        iconBox->AddChild(std::move(eventBox));
    }

    static void RemoveSNIItem(Item& item)
    {
        if (item.gtkEvent)
            iconBox->RemoveChild(item.gtkEvent);
    }

    static TimerResult UpdateWidgets(Box&);

    void WidgetSNI(Widget& parent)
    {
        if (RuntimeConfig::Get().hasSNI == false || Config::Get().enableSNI == false)
        {
            return;
        }
        // Add parent box
        auto box = Widget::Create<Box>();
        box->AddClass("widget");
        box->AddClass("sni");
        Utils::SetTransform(*box, {-1, false, Alignment::Fill});

        auto container = Widget::Create<Box>();
        container->AddTimer<Box>(UpdateWidgets, 1000, TimerDispatchBehaviour::LateDispatch);
        container->SetSpacing({4, false});
        container->SetOrientation(Utils::GetOrientation());
        Utils::SetTransform(*container, {-1, true, Alignment::Fill});

        iconBox = container.get();
        parentBox = box.get();
        box->AddChild(std::move(container));
        parent.AddChild(std::move(box));
    }

    static void DBusNameVanished(GDBusConnection*, const char* name, void*)
    {
        auto it = std::find_if(items.begin(), items.end(),
                               [&](const std::unique_ptr<Item>& item)
                               {
                                   return item->name == name;
                               });
        if (it != items.end())
        {
            LOG("SNI: " << name << " vanished!");
            RemoveSNIItem(*it->get());
            DestroyItem(*it->get());
            items.erase(it);
            return;
        }

        auto toRegisterIt = clientsToQuery.find(name);
        if (toRegisterIt != clientsToQuery.end())
        {
            clientsToQuery.erase(toRegisterIt);
        }

        LOG("SNI: Cannot remove unregistered bus name " << name);
        return;
    }

    static void ItemPropertyChanged(GDBusConnection*, const char* senderName, const char* object, const char*, const char* signalName, GVariant*,
                                    void* name)
    {
        LOG("SNI: Reloading " << (const char*)name << " " << object << " (Sender: " << senderName << ")");

        // Check if we're interested.
        if (strcmp(signalName, "NewIcon") != 0 && strcmp(signalName, "NewToolTip") != 0)
            return;

        GError* err = nullptr;
        g_dbus_connection_flush_sync(dbusConnection, nullptr, &err);
        if (err)
        {
            LOG("SNI: g_dbus_connection_call_sync failed: " << err->message);
            g_error_free(err);
        }
        auto itemIt = std::find_if(items.begin(), items.end(),
                                   [&](const std::unique_ptr<Item>& item)
                                   {
                                       return item->name == (const char*)name;
                                   });
        if (itemIt == items.end())
        {
            LOG("SNI: Couldn't update " << name << "!");
            return;
        }
        GatherItemProperties(**itemIt,
                             [](Item& item)
                             {
                                 item.textureWidget->SetBuf(item.pixbuf, item.w, item.h);
                                 item.textureWidget->SetTooltip(item.tooltip);
                             });
    }

    static TimerResult UpdateWidgets(Box&)
    {
        // Flush connection, so we hopefully don't deadlock with any client
        // TODO: Figure out if this is still needed when using gathering async
        GError* err = nullptr;
        g_dbus_connection_flush_sync(dbusConnection, nullptr, &err);
        if (err)
        {
            LOG("SNI: g_dbus_connection_call_sync failed: " << err->message);
            g_error_free(err);
        }

        if (RuntimeConfig::Get().hasSNI == false || Config::Get().enableSNI == false)
        {
            // Don't bother
            return TimerResult::Delete;
        }
        for (auto& [name, client] : clientsToQuery)
        {
            LOG("SNI: Creating Item " << client.name << " " << client.object);
            std::unique_ptr<Item> item = std::make_unique<Item>();
            item->name = std::move(client.name);
            item->object = std::move(client.object);
            Item& itemRef = *item;
            items.push_back(std::move(item));
            itemRef.watcherID = g_bus_watch_name_on_connection(dbusConnection, itemRef.name.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
                                                              DBusNameVanished, nullptr, nullptr);
            auto onGatherFinish = [](Item& item)
            {
                if (item.pixbuf == nullptr)
                {
                    return;
                }
                // Add handler for icon change
                char* staticBuf = new char[item.name.size() + 1]{0x0};
                memcpy(staticBuf, item.name.c_str(), item.name.size());
                LOG("SNI: Allocating static name buffer for " << item.name);
                item.propertyChangeWatcherID = g_dbus_connection_signal_subscribe(
                    dbusConnection, item.name.c_str(), "org.kde.StatusNotifierItem", nullptr, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                    ItemPropertyChanged, staticBuf,
                    +[](void* ptr)
                    {
                        LOG("SNI: Delete static name buffer for " << (char*)ptr);
                        delete[] (char*)ptr;
                    });
                AddSNIItem(item);
            };
            GatherItemProperties(itemRef, onGatherFinish);
        }
        clientsToQuery.clear();
        return TimerResult::Ok;
    }

    // Methods
    static bool RegisterItem(sniWatcher* watcher, GDBusMethodInvocation* invocation, const char* service)
    {
        std::string name;
        std::string object;
        if (strncmp(service, "/", 1) == 0)
        {
            // service is object (used by e.g. ayatana -> steam, discord)
            object = service;
            name = g_dbus_method_invocation_get_sender(invocation);
        }
        else
        {
            // service is bus name (used by e.g. Telegram)
            name = service;
            object = "/StatusNotifierItem";
        }
        auto it = std::find_if(items.begin(), items.end(),
                               [&](const std::unique_ptr<Item>& item)
                               {
                                   return item->name == name && item->object == object;
                               });
        if (it != items.end())
        {
            LOG("Rejecting " << name << " " << object);
            return false;
        }
        sni_watcher_emit_status_notifier_item_registered(watcher, service);
        sni_watcher_complete_register_status_notifier_item(watcher, invocation);
        LOG("SNI: Registered Item " << name << " " << object);
        clientsToQuery[name] = {name, std::move(object)};
        return true;
    }

    static void RegisterHost(sniWatcher*, GDBusMethodInvocation*, const char*)
    {
        LOG("TODO: Implement RegisterHost!");
    }

    // Signals
    static void ItemRegistered(sniWatcher*, const char*, void*)
    {
        // Don't care, since watcher and host will always be from gBar (at least for now)
    }
    static void ItemUnregistered(sniWatcher*, const char*, void*)
    {
        // Don't care, since watcher and host will always be from gBar (at least for now)
    }

    void Init()
    {
        if (RuntimeConfig::Get().hasSNI == false || Config::Get().enableSNI == false)
        {
            return;
        }

        auto busAcquired = [](GDBusConnection* connection, const char*, void*)
        {
            GError* err = nullptr;
            g_dbus_interface_skeleton_export((GDBusInterfaceSkeleton*)watcherSkeleton, connection, "/StatusNotifierWatcher", &err);
            if (err)
            {
                LOG("SNI: Failed to connect to dbus! Disabling SNI. Error: " << err->message);
                RuntimeConfig::Get().hasSNI = false;
                g_error_free(err);
                return;
            }
            dbusConnection = connection;

            // Connect methods and signals
            g_signal_connect(watcherSkeleton, "handle-register-status-notifier-item", G_CALLBACK(RegisterItem), nullptr);
            g_signal_connect(watcherSkeleton, "handle-register-status-notifier-host", G_CALLBACK(RegisterHost), nullptr);

            g_signal_connect(watcherSkeleton, "status-notifier-item-registered", G_CALLBACK(ItemRegistered), nullptr);
            g_signal_connect(watcherSkeleton, "status-notifier-item-unregistered", G_CALLBACK(ItemUnregistered), nullptr);

            // Host is always available
            sni_watcher_set_is_status_notifier_host_registered(watcherSkeleton, true);
            sni_watcher_emit_status_notifier_host_registered(watcherSkeleton);
        };
        auto emptyCallback = [](GDBusConnection*, const char*, void*) {};
        auto lostName = [](GDBusConnection*, const char*, void*)
        {
            LOG("SNI: Lost Name! Disabling SNI!");
            RuntimeConfig::Get().hasSNI = false;
        };
        auto flags = G_BUS_NAME_OWNER_FLAGS_REPLACE;
        g_bus_own_name(G_BUS_TYPE_SESSION, "org.kde.StatusNotifierWatcher", (GBusNameOwnerFlags)flags, +busAcquired, +emptyCallback, +lostName,
                       nullptr, nullptr);
        watcherSkeleton = sni_watcher_skeleton_new();

        std::string hostName = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
        g_bus_own_name(G_BUS_TYPE_SESSION, hostName.c_str(), (GBusNameOwnerFlags)flags, +emptyCallback, +emptyCallback, +emptyCallback, nullptr,
                       nullptr);
    }

    void Shutdown() {}
}
#endif
