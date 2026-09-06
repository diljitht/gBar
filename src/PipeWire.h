#pragma once
#include "System.h"
#include "Common.h"
#include "Config.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>

namespace PipeWireAudio
{
    struct NodeState
    {
        std::vector<float> channelVolumes;
        bool mute = false;
        double avgVolume = 0.0;
        bool valid = false;
    };

    static struct pw_main_loop* mainLoop = nullptr;
    static struct pw_context* context = nullptr;
    static struct pw_core* core = nullptr;
    static struct pw_registry* registry = nullptr;
    static struct pw_proxy* metadataProxy = nullptr;
    static struct pw_proxy* sinkProxy = nullptr;
    static struct pw_proxy* sourceProxy = nullptr;

    static struct spa_hook registryListener;
    static struct spa_hook metadataListener;
    static struct spa_hook sinkNodeListener;
    static struct spa_hook sourceNodeListener;
    static struct pw_metadata_events metadataEvents;

    static std::thread loopThread;

    static std::unordered_map<std::string, uint32_t> audioNodes;
    static std::string defaultSinkName;
    static std::string defaultSourceName;
    static uint32_t sinkId = 0;
    static uint32_t sourceId = 0;

    static std::mutex stateMutex;
    static NodeState sinkState;
    static NodeState sourceState;
    static System::AudioInfo info = { 0., false, 0., false };

    inline double RemapToUi(double linear)
    {
        double minVolume = Config::Get().audioMinVolume / 100.;
        double maxVolume = Config::Get().audioMaxVolume / 100.;
        double range = maxVolume - minVolume;
        if (range <= 0)
            return linear;
        return (std::clamp(linear, minVolume, maxVolume) - minVolume) / range;
    }

    inline double RemapToLinear(double value)
    {
        double minVolume = Config::Get().audioMinVolume / 100.;
        double maxVolume = Config::Get().audioMaxVolume / 100.;
        double range = maxVolume - minVolume;
        if (range <= 0)
            return std::clamp(value, 0., 1.);
        return std::clamp(value, 0., 1.) * range + minVolume;
    }

    inline std::string ParseDefaultName(const char* json)
    {
        if (!json)
            return "";
        std::string value = json;
        size_t keyPos = value.find("\"name\"");
        if (keyPos == std::string::npos)
            return "";
        size_t valueStart = value.find('"', keyPos + 6);
        if (valueStart == std::string::npos)
            return "";
        size_t valueEnd = value.find('"', valueStart + 1);
        if (valueEnd == std::string::npos)
            return "";
        return value.substr(valueStart + 1, valueEnd - valueStart - 1);
    }

    inline void ParseProps(const struct spa_pod* param, NodeState& state)
    {
        state.valid = true;
        if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
            return;

        if (const struct spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes))
        {
            uint32_t count = 0;
            void* values = spa_pod_get_array(&prop->value, &count);
            if (values && count > 0)
            {
                float* floats = (float*)values;
                state.channelVolumes.assign(floats, floats + count);

                double sum = 0.;
                for (float volume : state.channelVolumes)
                    sum += volume;
                state.avgVolume = std::cbrt(sum / (double)state.channelVolumes.size());
            }
        }

        if (const struct spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
            spa_pod_get_bool(&prop->value, &state.mute);
    }

    inline void PublishInfo()
    {
        if (sinkState.valid)
        {
            info.sinkVolume = RemapToUi(sinkState.avgVolume);
            info.sinkMuted = sinkState.mute;
        }
        if (sourceState.valid)
        {
            info.sourceVolume = RemapToUi(sourceState.avgVolume);
            info.sourceMuted = sourceState.mute;
        }
    }

    inline void NodeParamCallback(void* data, int, uint32_t id, uint32_t, uint32_t,
                                  const struct spa_pod* param)
    {
        if (id != SPA_PARAM_Props || !param)
            return;
        std::lock_guard<std::mutex> lock(stateMutex);
        ParseProps(param, *(NodeState*)data);
        PublishInfo();
    }

    inline void BindDefaultNode(const std::string& name, struct pw_proxy** proxy,
                                struct spa_hook* listener, NodeState& state, uint32_t& id)
    {
        if (!registry || *proxy || name.empty())
            return;
        auto it = audioNodes.find(name);
        if (it == audioNodes.end())
            return;
        {
            // Guard the id so the widget thread can't read it mid-update
            std::lock_guard<std::mutex> lock(stateMutex);
            id = it->second;
        }

        static const struct pw_node_events nodeEvents = { PW_VERSION_NODE_EVENTS, nullptr,
                                                          NodeParamCallback };
        *proxy = (struct pw_proxy*)pw_registry_bind(registry, it->second, PW_TYPE_INTERFACE_Node,
                                                    PW_VERSION_NODE, 0);
        if (!*proxy)
            return;
        uint32_t propsId = SPA_PARAM_Props;
        pw_node_subscribe_params((struct pw_node*)*proxy, &propsId, 1);
        pw_node_add_listener((struct pw_node*)*proxy, listener, &nodeEvents, &state);
    }

    inline void TryBindDefaults()
    {
        BindDefaultNode(defaultSinkName, &sinkProxy, &sinkNodeListener, sinkState, sinkId);
        BindDefaultNode(defaultSourceName, &sourceProxy, &sourceNodeListener, sourceState, sourceId);
    }

    inline int MetadataPropertyCallback(void*, uint32_t, const char* key, const char*,
                                        const char* value)
    {
        if (!key)
            return 0;
        std::string name = ParseDefaultName(value);
        if (strncmp(key, "default.audio.sink", strlen("default.audio.sink")) == 0)
            defaultSinkName = name;
        else if (strncmp(key, "default.audio.source", strlen("default.audio.source")) == 0)
            defaultSourceName = name;
        TryBindDefaults();
        return 0;
    }

    inline void RegistryGlobalCallback(void*, uint32_t id, uint32_t, const char* type,
                                       uint32_t, const struct spa_dict* props)
    {
        const char* name = nullptr;
        const char* mediaClass = nullptr;
        if (props)
        {
            const struct spa_dict_item* it;
            spa_dict_for_each(it, props)
            {
                if (strcmp(it->key, PW_KEY_NODE_NAME) == 0)
                    name = it->value;
                else if (strcmp(it->key, PW_KEY_METADATA_NAME) == 0)
                    name = it->value;
                if (strcmp(it->key, "media.class") == 0)
                    mediaClass = it->value;
            }
        }

        if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && name
            && strcmp(name, "default") == 0)
        {
            metadataProxy = (struct pw_proxy*)pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Metadata,
                                                               PW_VERSION_METADATA, 0);
            pw_metadata_add_listener((struct pw_metadata*)metadataProxy, &metadataListener,
                                     &metadataEvents, nullptr);
            return;
        }
        if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0 && name && mediaClass
            && strncmp(mediaClass, "Audio/", strlen("Audio/")) == 0)
        {
            audioNodes[name] = id;
            TryBindDefaults();
        }
    }

    inline int ConnectCallback(struct spa_loop*, bool, uint32_t, const void*, size_t, void*)
    {
        context = pw_context_new(pw_main_loop_get_loop(mainLoop), nullptr, 0);
        if (!context)
            return -ENOMEM;
        core = pw_context_connect(context, nullptr, 0);
        if (!core)
            return -errno;
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

        static const struct pw_registry_events registryEvents = {
            PW_VERSION_REGISTRY_EVENTS,
            RegistryGlobalCallback,
            nullptr,
        };
        pw_registry_add_listener(registry, &registryListener, &registryEvents, nullptr);

        metadataEvents.version = PW_VERSION_METADATA_EVENTS;
        metadataEvents.property = MetadataPropertyCallback;
        return 0;
    }

    inline void Init()
    {
        pw_init(nullptr, nullptr);
        mainLoop = pw_main_loop_new(nullptr);
        loopThread = std::thread([]() { pw_main_loop_run(mainLoop); });
        pw_loop_invoke(pw_main_loop_get_loop(mainLoop), ConnectCallback, 0, nullptr, 0, true,
                       nullptr);
        LOG("PipeWire: Audio backend initialised");
    }

    inline System::AudioInfo GetInfo()
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return info;
    }

    inline void WpctlSet(const std::string& action, const std::string& id,
                         const std::string& arg)
    {
        if (id.empty())
            return;
        std::thread([action, id, arg]() {
            std::vector<char> cmd;
            for (size_t i = 0; i < action.size(); ++i)
                cmd.push_back(action[i]);
            cmd.push_back(0);
            std::vector<char> paramId;
            for (size_t i = 0; i < id.size(); ++i)
                paramId.push_back(id[i]);
            paramId.push_back(0);
            std::vector<char> paramArg;
            for (size_t i = 0; i < arg.size(); ++i)
                paramArg.push_back(arg[i]);
            paramArg.push_back(0);

            char* argv[5] = { cmd.data(), paramId.data(), paramArg.data(), nullptr };
            argv[0] = (char*)"wpctl";
            argv[1] = cmd.data();
            argv[2] = paramId.data();
            argv[3] = paramArg.data();
            argv[4] = nullptr;

            pid_t pid = fork();
            if (pid == 0)
            {
                execvp("wpctl", argv);
                _exit(127);
            }
            if (pid > 0)
                waitpid(pid, nullptr, 0);
        }).detach();
    }

    inline void ApplyVolume(uint32_t id, const std::string& fallback, double targetVolume)
    {
        std::string target = id ? std::to_string(id) : fallback;
        if (target.empty())
            return;
        double value = std::clamp(targetVolume, 0., 2.);
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%.4f", value);
        WpctlSet("set-volume", target, buffer);
    }

    inline void ApplyMute(uint32_t id, const std::string& fallback, bool mutate)
    {
        std::string target = id ? std::to_string(id) : fallback;
        WpctlSet("set-mute", target, mutate ? "1" : "0");
    }

    inline void SetVolumeSink(double value)
    {
        LOG("Audio: Set volume of sink: " << value);
        std::lock_guard<std::mutex> lock(stateMutex);
        ApplyVolume(sinkId, "@DEFAULT_AUDIO_SINK@", RemapToLinear(value));
    }

    inline void SetVolumeSource(double value)
    {
        LOG("Audio: Set volume of source: " << value);
        std::lock_guard<std::mutex> lock(stateMutex);
        ApplyVolume(sourceId, "@DEFAULT_AUDIO_SOURCE@", RemapToLinear(value));
    }

    inline void SetMutedSink(bool muted)
    {
        LOG("Audio: " << (muted ? "Mute" : "Unmute") << " sink");
        std::lock_guard<std::mutex> lock(stateMutex);
        ApplyMute(sinkId, "@DEFAULT_AUDIO_SINK@", muted);
    }

    inline void SetMutedSource(bool muted)
    {
        LOG("Audio: " << (muted ? "Mute" : "Unmute") << " source");
        std::lock_guard<std::mutex> lock(stateMutex);
        ApplyMute(sourceId, "@DEFAULT_AUDIO_SOURCE@", muted);
    }

    inline void Shutdown()
    {
        if (!mainLoop)
            return;
        pw_main_loop_quit(mainLoop);
        if (loopThread.joinable())
            loopThread.join();
        if (registry)
            pw_proxy_destroy((struct pw_proxy*)registry);
        if (metadataProxy)
            pw_proxy_destroy(metadataProxy);
        if (sinkProxy)
            pw_proxy_destroy(sinkProxy);
        if (sourceProxy)
            pw_proxy_destroy(sourceProxy);
        if (core)
            pw_core_disconnect(core);
        if (context)
            pw_context_destroy(context);
        pw_main_loop_destroy(mainLoop);
        mainLoop = nullptr;
        pw_deinit();
    }
}