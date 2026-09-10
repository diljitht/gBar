#pragma once
#include <gtk/gtk.h>
#include <string>

namespace CSS
{
    void Load(const std::string& overrideConfigLocation);
    GtkCssProvider* GetProvider();
    const std::string& GetLoadedPath();
}
