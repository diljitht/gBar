#include "Window.h"
#include "Common.h"
#include "System.h"
#include "Bar.h"
#include "AudioFlyin.h"
#include "BluetoothDevices.h"
#include "Plugin.h"
#include "Config.h"
#include "CSS.h"

#include <gio/gio.h>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

static volatile sig_atomic_t shutdownSignal = 0;
static int lockFd = -1;
static constexpr const char* reloadLockFdEnv = "GBAR_RELOAD_LOCK_FD";

struct ReloadContext
{
    char** argv;
    guint debounceSource = 0;
    std::vector<GFileMonitor*> monitors;
    std::unordered_set<std::string> watchedPaths;
};

static gboolean ReloadProcess(void* data)
{
    ReloadContext& context = *(ReloadContext*)data;
    context.debounceSource = 0;
    LOG("Config or style changed; reloading gBar.");
    std::cout.flush();

    int descriptorFlags = -1;
    if (lockFd >= 0)
    {
        descriptorFlags = fcntl(lockFd, F_GETFD);
        if (descriptorFlags < 0 || fcntl(lockFd, F_SETFD, descriptorFlags & ~FD_CLOEXEC) != 0
            || setenv(reloadLockFdEnv, std::to_string(lockFd).c_str(), 1) != 0)
        {
            LOG("Failed to preserve widget lock while reloading: " << std::strerror(errno));
            if (descriptorFlags >= 0)
                fcntl(lockFd, F_SETFD, descriptorFlags);
            unsetenv(reloadLockFdEnv);
            return G_SOURCE_REMOVE;
        }
    }
    execvp(context.argv[0], context.argv);
    if (descriptorFlags >= 0)
        fcntl(lockFd, F_SETFD, descriptorFlags);
    unsetenv(reloadLockFdEnv);
    LOG("Failed to reload gBar: " << std::strerror(errno));
    return G_SOURCE_REMOVE;
}

static void OnWatchedFileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent event, void* data)
{
    switch (event)
    {
    case G_FILE_MONITOR_EVENT_CHANGED:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_MOVED:
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        break;
    default: return;
    }

    ReloadContext& context = *(ReloadContext*)data;
    if (context.debounceSource)
        g_source_remove(context.debounceSource);
    // Editors commonly write through a temporary file and rename it. Wait until
    // the sequence settles before reading the config or compiling SCSS again.
    context.debounceSource = g_timeout_add(250, ReloadProcess, &context);
}

static void WatchFile(ReloadContext& context, const std::string& path)
{
    if (path.empty())
        return;

    std::error_code pathError;
    std::filesystem::path absolutePath = std::filesystem::absolute(path, pathError).lexically_normal();
    std::string watchedPath = pathError ? path : absolutePath.string();
    if (!context.watchedPaths.insert(watchedPath).second)
        return;

    GFile* file = g_file_new_for_path(watchedPath.c_str());
    GError* error = nullptr;
    GFileMonitor* monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
    g_object_unref(file);
    if (!monitor)
    {
        LOG("Cannot watch " << watchedPath << ": " << (error ? error->message : "unknown error"));
        if (error)
            g_error_free(error);
    }
    else
    {
        g_signal_connect(monitor, "changed", G_CALLBACK(OnWatchedFileChanged), &context);
        context.monitors.push_back(monitor);
        LOG("Watching for changes: " << watchedPath);
    }

    // A file monitor on a symlink does not see atomic replacement of its target.
    std::filesystem::path resolvedPath = std::filesystem::canonical(watchedPath, pathError);
    if (!pathError && resolvedPath != absolutePath)
        WatchFile(context, resolvedPath.string());
}

void OpenAudioFlyin(Window& window, const std::string& monitor, AudioFlyin::Type type)
{
    AudioFlyin::Create(window, monitor, type);
}

static void RequestShutdown(int sig)
{
    shutdownSignal = sig;
}

void PrintHelp()
{
    LOG("==============================================\n"
        "|                gBar                        |\n"
        "==============================================\n"
        "gBar, a fast status bar + widgets\n"
        "\n"
        "Basic usage: \n"
        "\tgBar [OPTIONS...] WIDGET [MONITOR]\n"
        "\n"
        "Sample usage:\n"
        "\tgBar bar DP-1 \tOpens the status bar on monitor \"DP-1\"\n"
        "\tgBar bar 0    \tOpens the status bar on monitor 0 (Legacy)\n"
        "\tgBar audio    \tOpens the audio flyin on the current monitor\n"
        "\n"
        "All options:\n"
        "\t--help/-h      \tPrints this help page and exits afterwards\n"
        "\t--config/-c DIR\tOverrides the config search path to DIR and appends DIR to the CSS search path.\n"
        "\t               \t   DIR cannot contain path shorthands like e.g. \"~\""
        "\n"
        "All available widgets:\n"
        "\tbar            \tThe main status bar\n"
        "\taudio          \tAn audio volume slider flyin\n"
        "\tmic            \tA microphone volume slider flyin\n"
        "\tbluetooth      \tA bluetooth connection widget\n"
        "\t[plugin]       \tTries to open and run the plugin lib[plugin].so\n");
}

void CreateWidget(const std::string& widget, Window& window)
{
    if (widget == "bar")
    {
        Bar::Create(window, window.GetName());
    }
    else if (widget == "audio")
    {
        OpenAudioFlyin(window, window.GetName(), AudioFlyin::Type::Speaker);
    }
    else if (widget == "mic")
    {
        OpenAudioFlyin(window, window.GetName(), AudioFlyin::Type::Microphone);
    }
#ifdef WITH_BLUEZ
    else if (widget == "bluetooth")
    {
        if (RuntimeConfig::Get().hasBlueZ)
        {
            BluetoothDevices::Create(window, window.GetName());
        }
        else
        {
            LOG("Blutooth disabled, cannot open bluetooth widget!");
            exit(1);
        }
    }
#endif
    else
    {
        Plugin::LoadWidgetFromPlugin(widget, window, window.GetName());
    }
}

int main(int argc, char** argv)
{
    std::string widget;
    int32_t monitor = -1;
    std::string monitorName;
    std::string overrideConfigLocation = "";

    // Arg parsing
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg.size() < 1 || arg[0] != '-')
        {
            // This must be the widget selection.
            widget = arg;
            if (i + 1 < argc)
            {
                std::string mon = argv[i + 1];

                // Check if a monitor was supplied
                if (mon.empty() || mon[0] == '-')
                    continue;

                if (std::isdigit(mon[0]))
                {
                    // Monitor using ID
                    monitor = std::stoi(mon);
                    i += 1;
                }
                else
                {
                    // Monitor using connector name
                    monitorName = std::move(mon);
                    i += 1;
                    continue;
                }
            }
        }
        else if (arg == "-h" || arg == "--help")
        {
            PrintHelp();
            return 0;
        }
        else if (arg == "-c" || arg == "--config")
        {
            ASSERT(i + 1 < argc, "Not enough arguments provided for -c/--config!");
            overrideConfigLocation = argv[i + 1];
            i += 1;
        }
        else
        {
            LOG("Warning: Unknown CLI option \"" << arg << "\"")
        }
    }
    if (widget == "")
    {
        LOG("Error: Widget to open not specified!\n");
        PrintHelp();
        return 0;
    }
    if (argc <= 1)
    {
        LOG("Error: Too little arguments\n");
        PrintHelp();
        return 0;
    }

    struct sigaction action = {};
    action.sa_handler = RequestShutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0)
    {
        LOG("Cannot install shutdown handlers: " << std::strerror(errno));
        return 1;
    }

    // Acquire once, not in OnWidget: monitor changes can recreate the widget.
    const char* lockName = nullptr;
    if (widget == "audio" || widget == "mic")
        lockName = "gBar__audio.lock";
#ifdef WITH_BLUEZ
    else if (widget == "bluetooth")
        lockName = "gBar__bluetooth.lock";
#endif
    if (lockName)
    {
        const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtimeDir || runtimeDir[0] != '/')
        {
            LOG("A valid XDG_RUNTIME_DIR is required for widget locking.");
            return 1;
        }
        int dirFd = open(runtimeDir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat info = {};
        if (dirFd < 0 || fstat(dirFd, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0077) != 0)
        {
            if (dirFd >= 0)
                close(dirFd);
            LOG("XDG_RUNTIME_DIR must be a private directory owned by the current user.");
            return 1;
        }

        // A reload inherits the already-locked open file description. Verify
        // that it still names this widget's lock before reusing it.
        bool reusedLock = false;
        const char* inheritedValue = getenv(reloadLockFdEnv);
        if (inheritedValue)
        {
            char* end = nullptr;
            errno = 0;
            long inheritedFd = std::strtol(inheritedValue, &end, 10);
            struct stat pathInfo = {};
            if (errno == 0 && end != inheritedValue && *end == '\0' && inheritedFd >= 0 && inheritedFd <= INT_MAX
                && fstat((int)inheritedFd, &info) == 0
                && fstatat(dirFd, lockName, &pathInfo, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISREG(info.st_mode) && info.st_uid == geteuid() && info.st_nlink == 1
                && info.st_dev == pathInfo.st_dev && info.st_ino == pathInfo.st_ino
                && flock((int)inheritedFd, LOCK_EX | LOCK_NB) == 0)
            {
                lockFd = (int)inheritedFd;
                int flags = fcntl(lockFd, F_GETFD);
                reusedLock = flags >= 0 && fcntl(lockFd, F_SETFD, flags | FD_CLOEXEC) == 0;
            }
            unsetenv(reloadLockFdEnv);
            if (!reusedLock)
            {
                close(dirFd);
                LOG("Cannot validate inherited widget lock.");
                return 1;
            }
        }

        if (!reusedLock)
            lockFd = openat(dirFd, lockName, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        int openError = errno;
        close(dirFd);
        if (lockFd < 0)
        {
            LOG("Cannot open widget lock: " << std::strerror(openError));
            return 1;
        }
        if (fstat(lockFd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1)
        {
            close(lockFd);
            LOG("Widget lock must be a regular file owned by the current user with one link.");
            return 1;
        }
        if (flock(lockFd, LOCK_EX | LOCK_NB) != 0)
        {
            int lockError = errno;
            close(lockFd);
            if (lockError == EWOULDBLOCK || lockError == EAGAIN)
            {
                LOG("Widget already open (" << lockName << " is locked)! Exiting...");
                return 0;
            }
            LOG("Cannot acquire widget lock: " << std::strerror(lockError));
            return 1;
        }
        // OpenProcess forks a wrapper that does not exec, so CLOEXEC is not enough.
        int forkError = pthread_atfork(nullptr, nullptr, +[]()
        {
            if (lockFd >= 0)
                close(lockFd);
            lockFd = -1;
        });
        if (forkError != 0)
        {
            close(lockFd);
            LOG("Cannot install widget lock fork handler: " << std::strerror(forkError));
            return 1;
        }
    }

    System::Init(overrideConfigLocation);

    Window window;
    if (monitor != -1)
    {
        window = Window(monitor);
    }
    else
    {
        window = Window(monitorName);
    }

    window.Init(overrideConfigLocation);
    ReloadContext reloadContext{argv, 0, {}, {}};
    WatchFile(reloadContext, Config::GetLoadedPath());
    WatchFile(reloadContext, CSS::GetLoadedPath());
    window.OnWidget = [&]()
    {
        CreateWidget(widget, window);
    };
    // Only the flag is touched in signal context; GTK and cleanup run here.
    std::pair<Window*, bool> shutdownData{&window, false};
    guint shutdownSource = g_timeout_add(50, [](gpointer data) -> gboolean
    {
        auto& state = *static_cast<std::pair<Window*, bool>*>(data);
        if (shutdownSignal && !state.second)
        {
            state.second = true;
            state.first->Close();
        }
        return G_SOURCE_CONTINUE;
    }, &shutdownData);
    window.Run();

    g_source_remove(shutdownSource);
    if (reloadContext.debounceSource)
        g_source_remove(reloadContext.debounceSource);
    for (GFileMonitor* monitor : reloadContext.monitors)
    {
        g_file_monitor_cancel(monitor);
        g_object_unref(monitor);
    }
    System::FreeResources();
    // Never unlink flock files: another process may already have opened the inode.
    // Closing our descriptor (also automatic on abnormal exit) releases only our lock.
    if (lockFd >= 0)
        close(lockFd);
    return 0;
}
