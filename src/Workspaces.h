#pragma once
#include "Common.h"
#include "System.h"
#include "Config.h"

#include <cstdint>
#include <string>
#include <cstdlib>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef WITH_WORKSPACES
namespace Workspaces
{
    void Init();

    void PollStatus(const std::string& monitor, uint32_t numWorkspaces);

    System::WorkspaceStatus GetStatus(uint32_t workspaceId);

    uint32_t GetMaxUsedWorkspace();

    // Hyprland special (scratchpad) workspaces. 0/Dead when unsupported.
    size_t GetNumSpecialWorkspaces();
    System::WorkspaceStatus GetSpecialWorkspaceStatus(uint32_t index);
    std::string GetSpecialWorkspaceName(uint32_t index);

    void Shutdown();

    // TODO: Use ext_workspaces for this, if applicable
    inline void Goto(uint32_t workspace)
    {
        if (RuntimeConfig::Get().hasWorkspaces == false)
        {
            LOG("Error: Called Go to workspace, but Workspaces isn't open!");
            return;
        }

        // Hyprland >= 0.55 interprets the dispatch payload as a Lua call, so the old
        // "hyprctl dispatch workspace <n>" form no longer switches workspaces.
        // Use the Lua dispatcher, falling back to the legacy form for older versions.
        std::string newCmd = "hyprctl dispatch 'hl.dsp.focus({ workspace = " + std::to_string(workspace) + " })'";
        std::string legacyCmd = "hyprctl dispatch workspace " + std::to_string(workspace);
        LOG("Switching workspace: " << newCmd);
        if (system(newCmd.c_str()) != 0)
        {
            LOG("New dispatch syntax failed, falling back to: " << legacyCmd);
            system(legacyCmd.c_str());
        }
    }

    // direction: + or -
    inline void GotoNext(char direction)
    {
        char scrollOp = 'e';
        if (Config::Get().workspaceScrollOnMonitor)
        {
            scrollOp = 'm';
        }
        std::string workspaceRef = std::string("e") + direction + "1";
        std::string newCmd = std::string("hyprctl dispatch 'hl.dsp.focus({ workspace = \"") + workspaceRef + "\"";
        if (scrollOp == 'm')
        {
            newCmd += ", on_current_monitor = true";
        }
        newCmd += " })'";
        std::string legacyCmd = std::string("hyprctl dispatch workspace ") + scrollOp + direction + "1";
        LOG("Switching workspace: " << newCmd);
        if (system(newCmd.c_str()) != 0)
        {
            LOG("New dispatch syntax failed, falling back to: " << legacyCmd);
            system(legacyCmd.c_str());
        }
    }

    inline void ToggleSpecialWorkspace(uint32_t index)
    {
        if (RuntimeConfig::Get().hasWorkspaces == false)
        {
            LOG("Error: Called toggle special workspace, but Workspaces isn't open!");
            return;
        }
        std::string name = GetSpecialWorkspaceName(index);
        if (name == "")
        {
            LOG("Error: Toggle special workspace: Invalid index " << index);
            return;
        }

        // Hyprland >= 0.55 interprets the dispatch payload as a Lua call, so use
        // the toggle_special dispatcher, falling back to the legacy form.
        std::string newCmd = "hyprctl dispatch 'hl.dsp.workspace.toggle_special(\"" + name + "\")'";
        std::string legacyCmd = "hyprctl dispatch togglespecialworkspace " + name;
        LOG("Toggling special workspace: " << newCmd);
        if (system(newCmd.c_str()) != 0)
        {
            LOG("New dispatch syntax failed, falling back to: " << legacyCmd);
            system(legacyCmd.c_str());
        }
    }
}
#endif
