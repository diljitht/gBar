#include "System.h"
#include "Common.h"
#include "AMDGPU.h"
#include "PipeWire.h"
#include "Workspaces.h"
#include "Config.h"
#include "SNI.h"
#include "Wayland.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <charconv>
#include <cmath>
#include <memory>

#include <gio/gio.h>

#include <dlfcn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace System
{
    struct CPUTimestamp
    {
        size_t total = 0;
        size_t idle = 0;
    };

    double GetCPUUsage()
    {
        static CPUTimestamp prevCPUTime;
        static bool haveSample = false;
        std::ifstream procstat("/proc/stat");

        std::string line;
        while (std::getline(procstat, line))
        {
            // The "cpu " line (with a space) is the aggregate; skip "cpu0", "cpu1", ...
            if (line.rfind("cpu ", 0) != 0)
                continue;

            // Format: cpu user nice system idle iowait irq softirq steal guest guest_nice
            std::istringstream lineStr(line.substr(4));
            size_t user, nice, system, idle, iowait, irq, softirq, steal;
            if (!(lineStr >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal))
                continue;

            // Guest time is already included in user/nice. I/O wait is not CPU work.
            CPUTimestamp current{user + nice + system + idle + iowait + irq + softirq + steal, idle + iowait};
            CPUTimestamp previous = prevCPUTime;
            prevCPUTime = current;
            bool firstSample = !haveSample;
            haveSample = true;
            if (firstSample || current.total <= previous.total || current.idle < previous.idle)
                return 0;
            size_t diffTotal = current.total - previous.total;
            size_t diffIdle = current.idle - previous.idle;
            return diffIdle <= diffTotal ? 1.0 - (double)diffIdle / diffTotal : 0;
        }

        haveSample = false;
        return 0;
    }

    double GetCPUTemp()
    {
        std::ifstream tempFile(Config::Get().cpuThermalZone);
        if (!tempFile.is_open())
        {
            return 0.f;
        }
        std::string tempStr;
        std::getline(tempFile, tempStr);
        uint32_t intTemp = atoi(tempStr.c_str());
        double temp = (double)intTemp / 1000;
        return temp;
    }

    bool IsBatteryCharging()
    {
        std::ifstream batteryStatus(Config::Get().batteryFolder + "/status");
        if (batteryStatus.is_open())
        {
            std::string status;
            std::getline(batteryStatus, status);
            if (status == "Charging" || status == "Full")
            {
                return true;
            }
            else
            {
                return false;
            }
        }
        return false;
    }

    double GetBatteryPercentage()
    {
        std::ifstream fullChargeFile(Config::Get().batteryFolder + "/charge_full");
        std::ifstream currentChargeFile(Config::Get().batteryFolder + "/charge_now");
        if (fullChargeFile.is_open() && currentChargeFile.is_open())
        {
            std::string fullChargeStr;
            std::string currentChargeStr;
            std::getline(fullChargeFile, fullChargeStr);
            std::getline(currentChargeFile, currentChargeStr);
            uint32_t intFullCharge = atoi(fullChargeStr.c_str());
            uint32_t intCurrentCharge = atoi(currentChargeStr.c_str());
            return ((double)intCurrentCharge / (double)intFullCharge);
        }

        // Try capacity
        std::ifstream capacityFile(Config::Get().batteryFolder + "/capacity");
        if (capacityFile.is_open())
        {
            std::string capacityStr;
            std::getline(capacityFile, capacityStr);
            uint32_t intCapacity = atoi(capacityStr.c_str());
            return (double)intCapacity / 100.0;
        }

        // TODO: This is the wrong place to do this, since we don't know whether it is actually disabled.
        // A RuntimeConfig would be better, but it works for now.
        LOG("Couldn't open battery charge files! Disabling battery widget.");
        return -1;
    }

    RAMInfo GetRAMInfo()
    {
        RAMInfo out{};
        std::ifstream procstat("/proc/meminfo");
        ASSERT(procstat.is_open(), "Cannot open /proc/meminfo");

        std::string curLine;
        while (std::getline(procstat, curLine))
        {
            if (curLine.find("MemTotal: ") != std::string::npos)
            {
                // Found total
                std::string_view withoutMemTotal = std::string_view(curLine).substr(10);
                size_t begNum = withoutMemTotal.find_first_not_of(' ');
                std::string_view totalKiBStr = withoutMemTotal.substr(begNum, withoutMemTotal.find_last_of(' ') - begNum);
                uint32_t totalKiB = std::stoi(std::string(totalKiBStr));
                out.totalGiB = (double)totalKiB / (1024 * 1024);
            }
            else if (curLine.find("MemAvailable: ") != std::string::npos)
            {
                // Found available
                std::string_view withoutMemAvail = std::string_view(curLine).substr(14);
                size_t begNum = withoutMemAvail.find_first_not_of(' ');
                std::string_view availKiBStr = withoutMemAvail.substr(begNum, withoutMemAvail.find_last_of(' ') - begNum);
                uint32_t availKiB = std::stoi(std::string(availKiBStr));
                out.freeGiB = (double)availKiB / (1024 * 1024);
            }
        }
        return out;
    }

#ifdef WITH_AMD
    GPUInfo GetGPUInfo()
    {
#ifdef WITH_AMD
        if (RuntimeConfig::Get().hasAMD)
        {
            uint32_t util = AMDGPU::GetUtilization();
            GPUInfo out;
            out.utilisation = util;
            out.coreTemp = AMDGPU::GetTemperature();
            return out;
        }
#endif
        return {};
    }

    VRAMInfo GetVRAMInfo()
    {
#ifdef WITH_AMD
        if (RuntimeConfig::Get().hasAMD)
        {
            AMDGPU::VRAM vram = AMDGPU::GetVRAM();
            VRAMInfo out;
            out.totalGiB = (double)vram.totalB / (1024 * 1024 * 1024);
            out.usedGiB = (double)vram.usedB / (1024 * 1024 * 1024);
            return out;
        }
#endif
        return {};
    }
#endif

    DiskInfo GetDiskInfo()
    {
        struct statvfs stat{};
        std::string partition = Config::Get().diskPartition;
        int err = statvfs(partition.c_str(), &stat);
        DiskInfo out{};
        out.partition = partition;
        if (err != 0)
            return out;
        out.totalGiB = (double)stat.f_blocks * stat.f_frsize / (1024 * 1024 * 1024);
        if (stat.f_bfree <= stat.f_blocks)
            out.usedGiB = (double)(stat.f_blocks - stat.f_bfree) * stat.f_frsize / (1024 * 1024 * 1024);
        return out;
    }

#ifdef WITH_BLUEZ
    void InitBluetooth()
    {
        // Try connecting to d-bus and org.bluez
        GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
        if (!connection)
        {
            LOG("Can't connect to d-bus! Disabling Bluetooth!");
            // dbus not found, disable bluetooth
            RuntimeConfig::Get().hasBlueZ = false;
            return;
        }

        GError* err = nullptr;
        GVariant* objects = g_dbus_connection_call_sync(connection, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                                                        nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
        if (!objects)
        {
            LOG("Can't connect to BlueZ d-bus! Disabling Bluetooth!");
            if (err)
            {
                LOG(err->message);
                g_error_free(err);
            }
            // Not found, disable bluetooth
            RuntimeConfig::Get().hasBlueZ = false;
        }
        if (objects)
            g_variant_unref(objects);
        g_object_unref(connection);
    }
    BluetoothInfo GetBluetoothInfo()
    {
        BluetoothInfo out{};
        if (!RuntimeConfig::Get().hasBlueZ)
        {
            LOG("Error: GetBluetoothInfo called, but bluetooth isn't available");
            return out;
        }
        // Init D-Bus
        GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
        if (!connection)
            return out;

        GError* err = nullptr;
        GVariant* objects = g_dbus_connection_call_sync(connection, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                                                        nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
        g_object_unref(connection);
        if (!objects)
        {
            if (err)
            {
                LOG(err->message);
                g_error_free(err);
            }
            return out;
        }

        // First array
        GVariantIter* topArray;
        g_variant_get(objects, "(a{oa{sa{sv}}})", &topArray);

        // Iterate the objects
        GVariantIter* objectDescs;
        while (g_variant_iter_next(topArray, "{oa{sa{sv}}}", NULL, &objectDescs))
        {
            // Iterate the descs
            char* type = nullptr;
            GVariantIter* propIter;
            while (g_variant_iter_next(objectDescs, "{sa{sv}}", &type, &propIter))
            {
                if (strstr(type, "org.bluez.Adapter1"))
                {
                    std::string adapterName;
                    bool powered = false;

                    // This is a controller/adapter -> The "host"
                    char* str = nullptr;
                    GVariant* var = nullptr;
                    while (g_variant_iter_next(propIter, "{sv}", &str, &var))
                    {
                        if (strstr(str, "Name"))
                        {
                            const char* name = g_variant_get_string(var, nullptr);
                            // Copy it for us
                            adapterName = name;
                        }
                        else if (strstr(str, "Powered"))
                        {
                            powered = g_variant_get_boolean(var);
                        }
                        g_free(str);
                        g_variant_unref(var);
                    }
                    if (powered)
                    {
                        out.defaultController = std::move(adapterName);
                    }
                }
                else if (strstr(type, "org.bluez.Device1"))
                {
                    std::string deviceMac;
                    std::string deviceName;
                    std::string deviceType;
                    bool connected = false;
                    bool paired = false;

                    // This is a device -> One "client"
                    char* str = nullptr;
                    GVariant* var = nullptr;
                    while (g_variant_iter_next(propIter, "{sv}", &str, &var))
                    {
                        if (strcmp(str, "Address") == 0)
                        {
                            const char* mac = g_variant_get_string(var, nullptr);
                            // Copy it for us
                            deviceMac = mac;
                        }
                        else if (strstr(str, "Name"))
                        {
                            const char* name = g_variant_get_string(var, nullptr);
                            // Copy it for us
                            deviceName = name;
                        }
                        else if (strstr(str, "Icon"))
                        {
                            const char* icon = g_variant_get_string(var, nullptr);
                            // Copy it for us
                            deviceType = icon;
                        }
                        else if (strstr(str, "Connected"))
                        {
                            connected = g_variant_get_boolean(var);
                        }
                        else if (strstr(str, "Paired"))
                        {
                            paired = g_variant_get_boolean(var);
                        }
                        g_free(str);
                        g_variant_unref(var);
                    }
                    out.devices.push_back(BluetoothDevice{connected, paired, std::move(deviceMac), std::move(deviceName), std::move(deviceType)});
                }
                g_variant_iter_free(propIter);
                g_free(type);
            }
            g_variant_iter_free(objectDescs);
        }

        g_variant_iter_free(topArray);
        g_variant_unref(objects);

        return out;
    }

    static Process btctlProcess{-1};
    void StartBTScan()
    {
        StopBTScan();
        btctlProcess = OpenProcess("/bin/sh -c \"bluetoothctl scan on\"");
    }
    void StopBTScan()
    {
        if (btctlProcess.pid != -1)
        {
            // Ctrl-C stops bluetoothctl
            kill(btctlProcess.pid, SIGINT);
            btctlProcess = {-1};
        }
    }

    void ConnectBTDevice(BluetoothDevice& device, std::function<void(bool, BluetoothDevice&)> onFinish)
    {
        auto thread = [&, mac = device.mac, onFinish]()
        {
            // 1. Pair
            if (!device.paired)
            {
                int success = system(("bluetoothctl pair " + mac).c_str());
                if (success != 0)
                {
                    onFinish(false, device);
                    return;
                }
            }
            // 2. Connect
            if (!device.connected)
            {
                int success = system(("bluetoothctl connect " + mac).c_str());
                if (success != 0)
                {
                    onFinish(false, device);
                    return;
                }
            }
            onFinish(true, device);
        };
        std::thread worker(thread);
        worker.detach();
    }
    void DisconnectBTDevice(BluetoothDevice& device, std::function<void(bool, BluetoothDevice&)> onFinish)
    {
        auto thread = [&, mac = device.mac, onFinish]()
        {
            // 1. Disconnect
            if (device.connected)
            {
                int success = system(("bluetoothctl disconnect " + mac).c_str());
                if (success != 0)
                {
                    onFinish(false, device);
                    return;
                }
            }
            onFinish(true, device);
        };
        std::thread worker(thread);
        worker.detach();
    }

    void OpenBTWidget()
    {
        OpenProcess("/bin/sh -c \"gBar bluetooth\"");
    }

    std::string BTTypeToIcon(const BluetoothDevice& dev)
    {
        if (dev.type == "input-keyboard")
        {
            return Config::Get().devKeyboardIcon;
        }
        else if (dev.type == "input-mouse")
        {
            return Config::Get().devMouseIcon;
        }
        else if (dev.type == "audio-headset")
        {
            return Config::Get().devHeadsetIcon;
        }
        else if (dev.type == "input-gaming")
        {
            return Config::Get().devControllerIcon;
        }
        return Config::Get().devUnknownIcon;
    }
#endif

    AudioInfo GetAudioInfo()
    {
        return PipeWireAudio::GetInfo();
    }
    void SetVolumeSink(double volume)
    {
        PipeWireAudio::SetVolumeSink(volume);
    }
    void SetVolumeSource(double volume)
    {
        PipeWireAudio::SetVolumeSource(volume);
    }

    void SetMutedSink(bool muted)
    {
        PipeWireAudio::SetMutedSink(muted);
    }
    void SetMutedSource(bool muted)
    {
        PipeWireAudio::SetMutedSource(muted);
    }

#ifdef WITH_WORKSPACES
    void PollWorkspaces(const std::string& monitor, uint32_t numWorkspaces)
    {
        Workspaces::PollStatus(monitor, numWorkspaces);
    }
    WorkspaceStatus GetWorkspaceStatus(uint32_t workspace)
    {
        return Workspaces::GetStatus(workspace);
    }
    uint32_t GetMaxUsedWorkspace()
    {
        return Workspaces::GetMaxUsedWorkspace();
    }
    void GotoWorkspace(uint32_t workspace)
    {
        return Workspaces::Goto(workspace);
    }
    void GotoNextWorkspace(char direction)
    {
        return Workspaces::GotoNext(direction);
    }
    std::string GetWorkspaceSymbol(int index)
    {
        if (index < 0 || index > (int)Config::Get().numWorkspaces)
        {
            LOG("Workspace Symbol Index Out Of Bounds: " + std::to_string(index));
            return "";
        }

        // workspaceSymbols is from [1-n], wsidx is from [0-n[
        auto it = Config::Get().workspaceSymbols.find(index + 1);
        if (it == Config::Get().workspaceSymbols.end())
        {
            return Config::Get().defaultWorkspaceSymbol + " ";
        }

        return it->second + " ";
    }
    size_t GetNumSpecialWorkspaces()
    {
        return Workspaces::GetNumSpecialWorkspaces();
    }
    WorkspaceStatus GetSpecialWorkspaceStatus(uint32_t index)
    {
        return Workspaces::GetSpecialWorkspaceStatus(index);
    }
    std::string GetSpecialWorkspaceName(uint32_t index)
    {
        return Workspaces::GetSpecialWorkspaceName(index);
    }
    void ToggleSpecialWorkspace(uint32_t index)
    {
        return Workspaces::ToggleSpecialWorkspace(index);
    }
#endif

    void CheckNetwork()
    {
        std::ifstream bytes("/sys/class/net/" + Config::Get().networkAdapter + "/statistics/tx_bytes");
        if (!bytes.is_open())
        {
            LOG("Cannot open network device! Disabling Network widget.");
            RuntimeConfig::Get().hasNet = false;
        }
    }

    double GetNetworkBpsCommon(double dt, uint64_t& prevBytes, const std::string& deviceFile)
    {
        if (!RuntimeConfig::Get().hasNet)
        {
            prevBytes = UINT64_MAX;
            return 0.f;
        }
        std::ifstream bytes(deviceFile);
        std::string bytesStr;
        if (!(bytes >> bytesStr))
        {
            prevBytes = UINT64_MAX;
            return 0.f;
        }
        uint64_t curBytes = 0;
        auto parsed = std::from_chars(bytesStr.data(), bytesStr.data() + bytesStr.size(), curBytes);
        if (parsed.ec != std::errc{} || parsed.ptr != bytesStr.data() + bytesStr.size())
        {
            prevBytes = UINT64_MAX;
            return 0.f;
        }

        if (prevBytes == UINT64_MAX || curBytes < prevBytes || !std::isfinite(dt) || dt <= 0)
        {
            prevBytes = curBytes;
            return 0;
        }
        else
        {
            uint64_t diffBytes = curBytes - prevBytes;
            prevBytes = curBytes;
            // Is double precision a problem here?
            return diffBytes / dt;
        }
    }

    double GetNetworkBpsUpload(double dt)
    {
        // Better safe than sorry. Isn't 32bit max only a few GB?
        static uint64_t prevUploadBytes = UINT64_MAX;
        // Apparently /sys/class/net/.../statistics/[t/r]x_bytes is valid for all net devices under Linux
        // https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-class-net-statistics
        return GetNetworkBpsCommon(dt, prevUploadBytes, "/sys/class/net/" + Config::Get().networkAdapter + "/statistics/tx_bytes");
    }

    double GetNetworkBpsDownload(double dt)
    {
        // Better safe than sorry. Isn't 32bit max only a few GB?
        static uint64_t prevDownloadBytes = UINT64_MAX;
        // Apparently /sys/class/net/.../statistics/[t/r]x_bytes is valid for all net devices under Linux
        // https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-class-net-statistics
        return GetNetworkBpsCommon(dt, prevDownloadBytes, "/sys/class/net/" + Config::Get().networkAdapter + "/statistics/rx_bytes");
    }

    void GetOutdatedPackagesAsync(std::function<void(uint32_t)>&& returnVal)
    {
        static bool currentlyRunning = false;
        static std::function<void(uint32_t)> handlerFunction;
        static std::mutex configMutex;

        {
            std::scoped_lock<std::mutex> lock(configMutex);
            if (!RuntimeConfig::Get().hasPackagesScript)
            {
                return; // Don't bother
            }
            handlerFunction = std::move(returnVal);
            if (currentlyRunning)
            {
                // Thread is running, only update handler
                return;
            }

            currentlyRunning = true;
        }

        std::thread(
            []()
            {
                struct CallbackData
                {
                    uint32_t count = 0;
                    bool valid = false;
                };
                auto data = std::make_unique<CallbackData>();
                try
                {
                    auto closePipe = [](FILE* pipe) { pclose(pipe); };
                    std::unique_ptr<FILE, decltype(closePipe)> pipe(popen(Config::Get().checkPackagesCommand.c_str(), "r"), closePipe);
                    if (pipe)
                    {
                        std::string output;
                        char buf[2056];
                        while (fgets(buf, sizeof(buf), pipe.get()) != nullptr)
                            output.append(buf);
                        bool readOK = feof(pipe.get()) && !ferror(pipe.get());
                        int status = pclose(pipe.release());
                        if (readOK && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                        {
                            // Accept the first whitespace-delimited count, not a numeric prefix.
                            std::istringstream stream(output);
                            std::string token;
                            if (stream >> token)
                            {
                                auto parsed = std::from_chars(token.data(), token.data() + token.size(), data->count);
                                data->valid = parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
                            }
                        }
                    }
                }
                catch (const std::exception& error)
                {
                    LOG("GetOutdatedPackages: " << error.what());
                }

                // Keep the check pending until dispatch, so a newer caller replaces the handler.
                g_idle_add_full(
                    G_PRIORITY_DEFAULT_IDLE,
                    +[](gpointer userData) -> gboolean
                    {
                        auto* data = static_cast<CallbackData*>(userData);
                        std::function<void(uint32_t)> handler;
                        {
                            std::scoped_lock<std::mutex> lock(configMutex);
                            handler = std::move(handlerFunction);
                            currentlyRunning = false;
                            if (!data->valid)
                            {
                                LOG("GetOutdatedPackages: Command failed or returned an invalid count. Disabling package widget!");
                                RuntimeConfig::Get().hasPackagesScript = false;
                            }
                        }
                        if (data->valid && handler)
                            handler(data->count);
                        return G_SOURCE_REMOVE;
                    },
                    data.release(),
                    +[](gpointer userData) { delete static_cast<CallbackData*>(userData); });
            })
            .detach();
    }

    std::string GetTime(const std::string& format)
    {
        time_t stdTime = time(NULL);
        tm* localTime = localtime(&stdTime);
        std::stringstream str;
        str.imbue(std::locale(Config::Get().dateTimeLocale.c_str()));
        str << std::put_time(localTime,
                             format == "" ? Config::Get().dateTimeStyle.c_str() : format.c_str());
        return str.str();
    }

    std::string GetActiveWindowTitle()
    {
        Wayland::PollEvents();
        const Wayland::Window* activeWindow = Wayland::GetActiveWindow();
        if (!activeWindow)
            return "No Active Window"; // TODO Customize!!

        return activeWindow->title;
    }

    void Shutdown()
    {
        system("shutdown 0");
    }

    void Reboot()
    {
        system("reboot");
    }

    void ExitWM()
    {
        system(Config::Get().exitCommand.c_str());
    }

    void Lock()
    {
        system(Config::Get().lockCommand.c_str());
    }

    void Suspend()
    {
        system(Config::Get().suspendCommand.c_str());
    }

    void Init(const std::string& overrideConfigLocation)
    {
        Logging::Init();

        Config::Load(overrideConfigLocation);

        Wayland::Init();

#ifdef WITH_AMD
        AMDGPU::Init();
#endif

#ifdef WITH_WORKSPACES
        Workspaces::Init();
#endif

#ifdef WITH_BLUEZ
        InitBluetooth();
#endif

        PipeWireAudio::Init();

#ifdef WITH_SNI
        SNI::Init();
#endif

        CheckNetwork();
    }
    void FreeResources()
    {
        PipeWireAudio::Shutdown();

#ifdef WITH_WORKSPACES
        Workspaces::Shutdown();
#endif

#ifdef WITH_BLUEZ
        StopBTScan();
#endif
#ifdef WITH_SNI
        SNI::Shutdown();
#endif

        Wayland::Shutdown();

        Logging::Shutdown();
    }
}
