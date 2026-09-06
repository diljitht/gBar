#include "Workspaces.h"
#include "Wayland.h"
#include <ext-workspace-unstable-v1.h>
#include <unordered_map>
#include <poll.h>

#ifdef WITH_WORKSPACES
namespace Workspaces
{
    namespace Wayland
    {
        using WaylandMonitor = ::Wayland::Monitor;
        using WaylandWorkspaceGroup = ::Wayland::WorkspaceGroup;
        using WaylandWorkspace = ::Wayland::Workspace;

        static std::string lastPolledMonitor;
        void PollStatus(const std::string& monitor, uint32_t)
        {
            ::Wayland::PollEvents();
            lastPolledMonitor = monitor;
        }
        System::WorkspaceStatus GetStatus(uint32_t workspaceId)
        {
            const ::Wayland::Monitor* monitor = ::Wayland::FindMonitorByName(lastPolledMonitor);
            if (!monitor)
            {
                LOG("Polled monitor doesn't exist!");
                return System::WorkspaceStatus::Dead;
            }

            auto& workspaces = ::Wayland::GetWorkspaces();
            auto workspaceIt = std::find_if(workspaces.begin(), workspaces.end(),
                                            [&](const std::pair<zext_workspace_handle_v1*, WaylandWorkspace>& ws)
                                            {
                                                return ws.second.id == workspaceId;
                                            });
            if (workspaceIt == workspaces.end())
            {
                return System::WorkspaceStatus::Dead;
            }

            const WaylandWorkspaceGroup& group = ::Wayland::GetWorkspaceGroups().at(monitor->workspaceGroup);
            if (group.lastActiveWorkspace)
            {
                const WaylandWorkspace& activeWorkspace = workspaces.at(group.lastActiveWorkspace);
                if (activeWorkspace.id == workspaceId && activeWorkspace.active)
                {
                    return System::WorkspaceStatus::Active;
                }
                else if (activeWorkspace.id == workspaceId)
                {
                    // Last active workspace (Means we can still see it, since no other ws is active and thus is only visible)
                    return System::WorkspaceStatus::Current;
                }
            }

            const WaylandWorkspaceGroup& currentWorkspaceGroup = ::Wayland::GetWorkspaceGroups().at(workspaceIt->second.parent);
            if (currentWorkspaceGroup.lastActiveWorkspace == workspaceIt->first)
            {
                return System::WorkspaceStatus::Visible;
            }
            else
            {
                return System::WorkspaceStatus::Inactive;
            }

            return System::WorkspaceStatus::Dead;
        }
        uint32_t GetMaxUsedWorkspace()
        {
            uint32_t maxUsedWorkspace = 0;
            for (auto& workspace : ::Wayland::GetWorkspaces())
            {
                if (workspace.second.id > maxUsedWorkspace)
                {
                    maxUsedWorkspace = workspace.second.id;
                }
            }
            return maxUsedWorkspace;
        }
    }

#ifdef WITH_HYPRLAND
    namespace Hyprland
    {
        void Init()
        {
            if (!getenv("HYPRLAND_INSTANCE_SIGNATURE"))
            {
                LOG("Workspaces not running, disabling workspaces");
                // Not available
                RuntimeConfig::Get().hasWorkspaces = false;
            }
        }

        std::string GetSocketPath()
        {
            const char* instanceSignature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
            const char* xdgRuntimeDir = getenv("XDG_RUNTIME_DIR");
            if (!instanceSignature || !xdgRuntimeDir)
            {
                return "";
            }

            // First try $XDG_RUNTIME_DIR/hypr/.../. This is the new dir.
            std::string socketPath = std::string(xdgRuntimeDir) + "/hypr/" + instanceSignature + "/.socket.sock";
            if (std::filesystem::exists(socketPath))
            {
                return socketPath;
            }

            // Next try /tmp/hypr/.../. This is removed as of https://github.com/hyprwm/Hyprland/pull/5788
            socketPath = "/tmp/hypr/" + std::string(instanceSignature) + "/.socket.sock";
            if (std::filesystem::exists(socketPath))
            {
                return socketPath;
            }
            return "";
        }

        std::string DispatchIPC(const std::string& arg)
        {
            std::string socketPath = GetSocketPath();
            if (socketPath == "")
            {
                LOG("Error: Couldn't find the Hyprland socket!");
                return "";
            }

            sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            if (socketPath.size() >= sizeof(addr.sun_path))
            {
                LOG("Error: Hyprland socket path is too long.");
                return "";
            }
            memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);

            int hyprSocket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (hyprSocket < 0)
            {
                LOG("Error: Couldn't create Hyprland socket.");
                return "";
            }
            struct SocketGuard
            {
                int fd;
                ~SocketGuard() { close(fd); }
            } socketGuard{hyprSocket};

            // Bound the entire exchange, including a peer that sends data slowly.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            auto waitFor = [&](short events)
            {
                while (true)
                {
                    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                    if (remaining <= 0)
                    {
                        errno = ETIMEDOUT;
                        return false;
                    }
                    pollfd fd{hyprSocket, events, 0};
                    int ready = poll(&fd, 1, (int)remaining);
                    if (ready > 0)
                        return true; // Let the socket operation report errors or EOF.
                    if (ready == 0)
                        errno = ETIMEDOUT;
                    if (ready == 0 || errno != EINTR)
                        return false;
                }
            };

            int ret;
            while (true)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    errno = ETIMEDOUT;
                    ret = -1;
                    break;
                }
                ret = connect(hyprSocket, (sockaddr*)&addr, SUN_LEN(&addr));
                if (ret == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                    break;
                // AF_UNIX reports a full listen backlog as EAGAIN, not EINPROGRESS.
                // No connection is pending yet; retry connect rather than polling SO_ERROR.
                if (errno != EINTR)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (ret < 0 && (errno == EINPROGRESS || errno == EALREADY) && waitFor(POLLOUT))
            {
                int error = 0;
                socklen_t size = sizeof(error);
                ret = getsockopt(hyprSocket, SOL_SOCKET, SO_ERROR, &error, &size);
                if (ret == 0 && error != 0)
                {
                    errno = error;
                    ret = -1;
                }
            }
            if (ret < 0)
            {
                LOG("Error: Couldn't connect to Hyprland socket.");
                return "";
            }

            size_t offset = 0;
            while (offset < arg.size())
            {
                ssize_t written = Utils::RetrySocketOp(
                    [&]() -> ssize_t
                    {
                        if (!waitFor(POLLOUT))
                            return -1;
                        return send(hyprSocket, arg.data() + offset, arg.size() - offset, MSG_NOSIGNAL);
                    },
                    5, "write");
                if (written <= 0)
                {
                    LOG("Error: Couldn't write to Hyprland socket.");
                    return "";
                }
                offset += (size_t)written;
            }
            char buf[2056];
            std::string res;

            while (true)
            {
                ssize_t bytesRead = Utils::RetrySocketOp(
                    [&]() -> ssize_t
                    {
                        if (!waitFor(POLLIN))
                            return -1;
                        return read(hyprSocket, buf, sizeof(buf));
                    },
                    5, "read");
                if (bytesRead == 0)
                {
                    break;
                }
                if (bytesRead < 0)
                {
                    LOG("Error: Couldn't read from Hyprland socket.");
                    return "";
                }
                res += std::string(buf, bytesRead);
            }
            return res;
        }

        static std::vector<System::WorkspaceStatus> workspaceStati;
        static uint32_t maxUsedWorkspace = 0;

        struct SpecialWorkspace
        {
            int32_t id;
            std::string name; // Without the "special:" prefix
        };
        // Hyprland special (scratchpad) workspaces
        static std::vector<SpecialWorkspace> specialWorkspaces;
        // The id of the special workspace currently shown on the polled monitor (0 if none)
        static int32_t activeSpecialWorkspaceId = 0;

        void PollStatus(const std::string& monitor, uint32_t numWorkspaces)
        {
            if (RuntimeConfig::Get().hasWorkspaces == false)
            {
                LOG("Error: Polled workspace status, but Workspaces isn't open!");
                return;
            }
            workspaceStati.clear();
            workspaceStati.resize(numWorkspaces, System::WorkspaceStatus::Dead);
            maxUsedWorkspace = 0;
            specialWorkspaces.clear();
            activeSpecialWorkspaceId = 0;

            size_t parseIdx = 0;
            // First parse workspaces
            std::string workspaces = DispatchIPC("/workspaces");
            while ((parseIdx = workspaces.find("workspace ID ", parseIdx)) != std::string::npos)
            {
                // Advance two spaces
                size_t begWSNum = workspaces.find(' ', parseIdx) + 1;
                begWSNum = workspaces.find(' ', begWSNum) + 1;
                size_t endWSNum = workspaces.find(' ', begWSNum);

                std::string ws = workspaces.substr(begWSNum, endWSNum - begWSNum);
                int32_t wsId = std::atoi(ws.c_str());
                if (wsId >= 1 && wsId <= (int32_t)numWorkspaces)
                {
                    // WS is at least inactive
                    workspaceStati[wsId - 1] = System::WorkspaceStatus::Inactive;
                }
                // Special (scratchpad) workspaces have negative ids, e.g. -98 (special:gbartest)
                else if (wsId <= -1)
                {
                    size_t begName = endWSNum + 1;
                    size_t endName = workspaces.find(')', begName);
                    std::string wsName = endName == std::string::npos
                        ? ""
                        : workspaces.substr(begName + 1, endName - begName - 1);
                    if (wsName.compare(0, 8, "special:") == 0)
                    {
                        wsName = wsName.substr(8);
                    }
                    specialWorkspaces.push_back({wsId, wsName});
                }
                // Update maxUsedWorkspace
                if (wsId > 0 && (uint32_t)wsId > maxUsedWorkspace)
                    maxUsedWorkspace = wsId;
                parseIdx = endWSNum;
            }

            // Parse active workspaces for monitor
            std::string monitors = DispatchIPC("/monitors");
            parseIdx = 0;
            while ((parseIdx = monitors.find("Monitor ", parseIdx)) != std::string::npos)
            {
                // Query monitor name
                // Format: Monitor <name> (ID <id>)
                size_t begMonNum = monitors.find(' ', parseIdx) + 1;
                size_t endMonNum = monitors.find(' ', begMonNum);
                std::string mon = monitors.substr(begMonNum, endMonNum - begMonNum);
                size_t nextMon = monitors.find("Monitor ", begMonNum);

                // Parse active workspace
                parseIdx = monitors.find("active workspace: ", parseIdx);
                ASSERT(parseIdx != std::string::npos, "Invalid IPC response!");
                size_t begWSNum = monitors.find('(', parseIdx) + 1;
                size_t endWSNum = monitors.find(')', begWSNum);
                std::string ws = monitors.substr(begWSNum, endWSNum - begWSNum);
                int32_t wsId = std::atoi(ws.c_str());

                // Check if focused
                parseIdx = monitors.find("focused: ", parseIdx);
                ASSERT(parseIdx != std::string::npos, "Invalid IPC response!");
                size_t begFocused = monitors.find(' ', parseIdx) + 1;
                size_t endFocused = monitors.find('\n', begFocused);
                bool focused = std::string_view(monitors).substr(begFocused, endFocused - begFocused) == "yes";

                if (wsId >= 1 && wsId <= (int32_t)numWorkspaces)
                {
                    if (mon == monitor)
                    {
                        if (focused)
                        {
                            workspaceStati[wsId - 1] = System::WorkspaceStatus::Active;
                        }
                        else
                        {
                            workspaceStati[wsId - 1] = System::WorkspaceStatus::Current;
                        }
                    }
                    else
                    {
                        workspaceStati[wsId - 1] = System::WorkspaceStatus::Visible;
                    }
                }
                // Update maxUsedWorkspace
                if (wsId > 0 && (uint32_t)wsId > maxUsedWorkspace)
                    maxUsedWorkspace = wsId;

                // Parse which special workspace is shown on this monitor (Hyprland scratchpad)
                // Format: special workspace: <id> (<name>)
                size_t specialParseIdx = monitors.find("special workspace: ", begMonNum);
                if (specialParseIdx != std::string::npos && (nextMon == std::string::npos || specialParseIdx < nextMon))
                {
                    size_t begSpecialId = specialParseIdx + std::string("special workspace: ").size();
                    size_t endSpecialId = monitors.find(' ', begSpecialId);
                    int32_t specialId = std::atoi(monitors.substr(begSpecialId, endSpecialId - begSpecialId).c_str());
                    if (mon == monitor)
                    {
                        activeSpecialWorkspaceId = specialId;
                    }
                }
            }
        }

        System::WorkspaceStatus GetStatus(uint32_t workspaceId)
        {
            if (RuntimeConfig::Get().hasWorkspaces == false)
            {
                LOG("Error: Queried for workspace status, but Workspaces isn't open!");
                return System::WorkspaceStatus::Dead;
            }
            ASSERT(workspaceId > 0 && workspaceId <= workspaceStati.size(), "Invalid workspaceId, you need to poll the workspace first!");
            return workspaceStati[workspaceId - 1];
        }

        uint32_t GetMaxUsedWorkspace()
        {
            return maxUsedWorkspace;
        }

        size_t GetNumSpecialWorkspaces()
        {
            return specialWorkspaces.size();
        }

        System::WorkspaceStatus GetSpecialWorkspaceStatus(uint32_t index)
        {
            if (RuntimeConfig::Get().hasWorkspaces == false)
            {
                return System::WorkspaceStatus::Dead;
            }
            if (index >= specialWorkspaces.size())
            {
                return System::WorkspaceStatus::Dead;
            }
            return specialWorkspaces[index].id == activeSpecialWorkspaceId
                ? System::WorkspaceStatus::SpecialActive
                : System::WorkspaceStatus::SpecialInactive;
        }

        std::string GetSpecialWorkspaceName(uint32_t index)
        {
            if (index >= specialWorkspaces.size())
            {
                return "";
            }
            return specialWorkspaces[index].name;
        }
    }
#endif

    void Init()
    {
#ifdef WITH_HYPRLAND
        if (Config::Get().useHyprlandIPC)
        {
            Hyprland::Init();
            return;
        }
#endif
    }

    void PollStatus(const std::string& monitor, uint32_t numWorkspaces)
    {
#ifdef WITH_HYPRLAND
        if (Config::Get().useHyprlandIPC)
        {
            Hyprland::PollStatus(monitor, numWorkspaces);
            return;
        }
#endif
        Wayland::PollStatus(monitor, numWorkspaces);
    }

    System::WorkspaceStatus GetStatus(uint32_t workspaceId)
    {
#ifdef WITH_HYPRLAND
        if (Config::Get().useHyprlandIPC)
        {
            return Hyprland::GetStatus(workspaceId);
        }
#endif
        return Wayland::GetStatus(workspaceId);
    }

    uint32_t GetMaxUsedWorkspace()
    {
#ifdef WITH_HYPRLAND
        if (Config::Get().useHyprlandIPC)
        {
            return Hyprland::GetMaxUsedWorkspace();
        }
#endif
        return Wayland::GetMaxUsedWorkspace();
    }

    size_t GetNumSpecialWorkspaces()
    {
#ifdef WITH_HYPRLAND
        if (Config::Get().useHyprlandIPC)
        {
            return Hyprland::GetNumSpecialWorkspaces();
        }
#endif
        return 0;
    }

    System::WorkspaceStatus GetSpecialWorkspaceStatus(uint32_t index)
    {
#ifdef WITH_HYPRLAND
        if (Config::Get().useHyprlandIPC)
        {
            return Hyprland::GetSpecialWorkspaceStatus(index);
        }
#endif
        return System::WorkspaceStatus::Dead;
    }

    std::string GetSpecialWorkspaceName(uint32_t index)
    {
#ifdef WITH_HYPRLAND
        if (Config::Get().useHyprlandIPC)
        {
            return Hyprland::GetSpecialWorkspaceName(index);
        }
#endif
        return "";
    }

    void Shutdown() {}
}
#endif
