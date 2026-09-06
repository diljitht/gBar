#include "BluetoothDevices.h"
#include "System.h"
#include <memory>
#include <unordered_map>
#include <string>
#include <algorithm>

#ifdef WITH_BLUEZ
namespace BluetoothDevices
{
    namespace DynCtx
    {
        enum class DeviceState
        {
            Invalid = BIT(0),
            Failed = BIT(1),            // Request failed
            RequestConnect = BIT(2),    // If device is requested to connect
            RequestDisconnect = BIT(3), // If device is requested to disconnect
            Connected = BIT(4),         // BlueZ says to us, that it is connected
            Disconnected = BIT(5),      // BlueZ says to us, that it is disconnected
        };
        DEFINE_ENUM_FLAGS(DeviceState);

        struct BTDeviceWithState
        {
            System::BluetoothDevice device{};
            DeviceState state{};
            std::shared_ptr<System::BluetoothDevice> request;
        };

        std::vector<BTDeviceWithState> devices;
        Box* deviceListBox;
        Window* win;
        bool scanning = false;

        void OnClick(Button& button, BTDeviceWithState& device)
        {
            DeviceState& state = device.state;

            // Clear failed bit
            state &= ~DeviceState::Failed;
            button.RemoveClass("failed");

            // System retains a reference on its worker; the callback owns this stable copy.
            auto requestDevice = std::make_shared<System::BluetoothDevice>(device.device);
            auto onFinish = [requestDevice](bool success, System::BluetoothDevice&)
            {
                struct Result
                {
                    std::shared_ptr<System::BluetoothDevice> request;
                    bool success;
                };
                g_idle_add_full(
                    G_PRIORITY_DEFAULT_IDLE,
                    +[](void* ptr) -> gboolean
                    {
                        const auto& result = *static_cast<Result*>(ptr);
                        auto it = std::find_if(devices.begin(), devices.end(),
                                               [&](const auto& current) { return current.request == result.request; });
                        if (it != devices.end())
                        {
                            it->request.reset();
                            if (!result.success)
                            {
                                it->state &= ~(DeviceState::RequestConnect | DeviceState::RequestDisconnect);
                                it->state |= DeviceState::Failed;
                            }
                        }
                        return G_SOURCE_REMOVE;
                    },
                    new Result{requestDevice, success},
                    +[](void* ptr) { delete static_cast<Result*>(ptr); });
            };

            // Only try to connect, if we know we're already disconnected and we haven't requested before(Same for disconnect)
            if (FLAG_CHECK(state, DeviceState::Disconnected) && !FLAG_CHECK(state, DeviceState::RequestConnect))
            {
                button.AddClass("active");
                button.RemoveClass("inactive");
                state |= DeviceState::RequestConnect;

                device.request = requestDevice;
                System::ConnectBTDevice(*requestDevice, onFinish);
            }
            else if (FLAG_CHECK(state, DeviceState::Connected) && !FLAG_CHECK(state, DeviceState::RequestDisconnect))
            {
                button.AddClass("inactive");
                button.RemoveClass("active");
                state |= DeviceState::RequestDisconnect;

                device.request = requestDevice;
                System::DisconnectBTDevice(*requestDevice, onFinish);
            }
        }

        void UpdateDeviceUIElem(Button& button, BTDeviceWithState& device)
        {
            if (device.device.name.size())
            {
                button.SetText(System::BTTypeToIcon(device.device) + device.device.name);
            }
            else
            {
                button.SetText(device.device.mac);
            }
            bool requestConnect = FLAG_CHECK(device.state, DeviceState::RequestConnect);
            bool requestDisconnect = FLAG_CHECK(device.state, DeviceState::RequestDisconnect);
            if (requestConnect || (!requestDisconnect && FLAG_CHECK(device.state, DeviceState::Connected)))
            {
                button.AddClass("active");
                button.RemoveClass("inactive");
            }
            else if (requestDisconnect || (!requestConnect && FLAG_CHECK(device.state, DeviceState::Disconnected)))
            {
                button.AddClass("inactive");
                button.RemoveClass("active");
            }
            if (FLAG_CHECK(device.state, DeviceState::Failed))
            {
                button.AddClass("failed");
            }
            else
            {
                button.RemoveClass("failed");
            }

            button.OnClick(
                [mac = device.device.mac](Button& button)
                {
                    auto it = std::find_if(devices.begin(), devices.end(),
                                           [&](const auto& current) { return current.device.mac == mac; });
                    if (it != devices.end())
                        OnClick(button, *it);
                });
        }

        void InvalidateDeviceUI()
        {
            // Shrink
            while (deviceListBox->GetChilds().size() > devices.size())
            {
                deviceListBox->RemoveChild(deviceListBox->GetChilds().size() - 1);
            }

            size_t idx = 0;
            for (auto& device : devices)
            {
                if (idx >= deviceListBox->GetChilds().size())
                {
                    // Create new
                    auto button = Widget::Create<Button>();
                    button->SetClass("bt-button");
                    deviceListBox->AddChild(std::move(button));
                }

                // Initialise
                UpdateDeviceUIElem((Button&)*deviceListBox->GetChilds()[idx], device);

                idx++;
            }
        }

        TimerResult OnUpdate(Widget&)
        {
            // Invalidate each current device
            for (auto& device : devices)
            {
                device.state |= DeviceState::Invalid;
            }

            for (auto& device : System::GetBluetoothInfo().devices)
            {
                auto stateDevIt = std::find_if(devices.begin(), devices.end(),
                                               [&](auto& x)
                                               {
                                                   return device.mac == x.device.mac;
                                               });
                BTDeviceWithState* stateDev = nullptr;
                if (stateDevIt != devices.end())
                {
                    stateDev = &*stateDevIt;
                }
                else
                {
                    devices.push_back({});
                    stateDev = &devices.back();
                }
                // This device exists
                stateDev->state &= ~DeviceState::Invalid;
                if (device.connected)
                {
                    // Clear any requests, it is now connected
                    stateDev->state &= ~DeviceState::RequestConnect;

                    stateDev->state &= ~DeviceState::Disconnected;
                    stateDev->state |= DeviceState::Connected;
                }
                else
                {
                    stateDev->state &= ~DeviceState::RequestDisconnect;

                    stateDev->state &= ~DeviceState::Connected;
                    stateDev->state |= DeviceState::Disconnected;
                }
                stateDev->device = device;
            }
            // Erase all invalid
            for (auto it = devices.begin(); it != devices.end();)
            {
                if (FLAG_CHECK(it->state, DeviceState::Invalid))
                {
                    LOG("Removing " << it->device.name);
                    it = devices.erase(it);
                }
                else
                {
                    it++;
                }
            }

            // Sort devices by, whether they're connected or not
            std::sort(devices.begin(), devices.end(),
                      [](auto& a, auto& b)
                      {
                          auto& deviceA = a.device;
                          auto& deviceB = b.device;
                          if (deviceA.connected && !deviceB.connected)
                          {
                              return true;
                          }
                          else if (deviceB.connected && !deviceA.connected)
                          {
                              return false;
                          }
                          return deviceA.name < deviceB.name;
                      });

            InvalidateDeviceUI();

            return TimerResult::Ok;
        }

        void Close(Button&)
        {
            win->Close();
        }

        void Scan(Button& button)
        {
            scanning = !scanning;
            if (scanning)
            {
                button.AddClass("active");
                button.RemoveClass("inactive");
                System::StartBTScan();
            }
            else
            {
                button.AddClass("inactive");
                button.RemoveClass("active");
                System::StopBTScan();
            }
        }
    }

    void WidgetHeader(Widget& parentWidget)
    {
        auto headerBox = Widget::Create<Box>();
        headerBox->SetClass("bt-header-box");
        {
            auto headerText = Widget::Create<Text>();
            headerText->SetText(Config::Get().btOnIcon + " Bluetooth");
            headerBox->AddChild(std::move(headerText));

            auto headerRefresh = Widget::Create<Button>();
            headerRefresh->SetText("");
            headerRefresh->SetClass("bt-scan");
            headerRefresh->OnClick(DynCtx::Scan);
            headerBox->AddChild(std::move(headerRefresh));

            auto headerClose = Widget::Create<Button>();
            headerClose->SetText("");
            headerClose->SetClass("bt-close");
            headerClose->OnClick(DynCtx::Close);
            headerBox->AddChild(std::move(headerClose));
        }
        parentWidget.AddChild(std::move(headerBox));
    }

    void WidgetBody(Widget& parentWidget)
    {
        auto bodyBox = Widget::Create<Box>();
        DynCtx::deviceListBox = bodyBox.get();
        bodyBox->SetOrientation(Orientation::Vertical);
        bodyBox->SetClass("bt-body-box");
        bodyBox->AddTimer<Widget>(DynCtx::OnUpdate, 100);
        parentWidget.AddChild(std::move(bodyBox));
    }

    void Create(Window& window, UNUSED const std::string& monitor)
    {
        DynCtx::win = &window;
        auto mainWidget = Widget::Create<Box>();
        mainWidget->SetSpacing({8, false});
        mainWidget->SetOrientation(Orientation::Vertical);
        mainWidget->SetVerticalTransform({32, true, Alignment::Fill});
        mainWidget->SetClass("bt-bg");

        WidgetHeader(*mainWidget);
        WidgetBody(*mainWidget);

        window.SetExclusive(false);
        Anchor anchor;
        Anchor marginAnchor;
        switch (Config::Get().location)
        {
        case 'T':
            anchor = Anchor::Right | Anchor::Top;
            marginAnchor = Anchor::Top;
            break;
        case 'B':
            anchor = Anchor::Bottom | Anchor::Right;
            marginAnchor = Anchor::Bottom;
            break;
        case 'L':
            anchor = Anchor::Left | Anchor::Bottom;
            marginAnchor = Anchor::Left;
            // TODO: Config
            window.SetMargin(Anchor::Bottom, 150);
            break;
        case 'R':
            anchor = Anchor::Right | Anchor::Bottom;
            marginAnchor = Anchor::Right;
            // TODO: Config
            window.SetMargin(Anchor::Bottom, 150);
            break;
        default:
            LOG("Invalid location char \"" << Config::Get().location << "\"!");
            anchor = Anchor::Right | Anchor::Top;
            marginAnchor = Anchor::Top;
        }
        window.SetMargin(marginAnchor, 8);
        window.SetAnchor(anchor);
        window.SetMainWidget(std::move(mainWidget));
    }
}
#endif
