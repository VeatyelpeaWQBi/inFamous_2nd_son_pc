// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from infamous2ndson_pc/gpu/shim/bbport_windows_touchpad.h.
// Native foreground-only Windows precision touchpad HID input (Digitizers/Touchpad 0x0D/0x05).
// Report fields are located through Windows' HID parser, not a vendor byte layout.
#pragma once
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#include <vector>
#include <optional>
#include <cstdio>
#include <cstring>
#include "input/touchpad_frames.h"
namespace Input {
class WindowsTouchpad {
    HMODULE hid{};
    decltype(&HidP_GetCaps) caps_fn{};
    decltype(&HidP_GetValueCaps) values_fn{};
    decltype(&HidP_GetButtonCaps) buttons_fn{};
    decltype(&HidP_GetUsageValue) value_fn{};
    decltype(&HidP_GetUsages) usages_fn{};
    struct Device {
        HANDLE handle{};
        std::vector<std::byte> prepared;
        std::vector<HIDP_VALUE_CAPS> values;
        std::vector<HIDP_BUTTON_CAPS> buttons;
        std::vector<USHORT> fingers;
        TouchpadFrames frames;
        PHIDP_PREPARSED_DATA Data() {return reinterpret_cast<PHIDP_PREPARSED_DATA>(prepared.data());}
    };
    std::vector<Device> devices;
    std::vector<std::byte> payload;
    HWND window{};bool registered{};
    bool Parser() {
        if(hid)return caps_fn&&values_fn&&buttons_fn&&value_fn&&usages_fn;
        hid=LoadLibraryExW(L"hid.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!hid)return false;
        caps_fn=reinterpret_cast<decltype(caps_fn)>(GetProcAddress(hid,"HidP_GetCaps"));
        values_fn=reinterpret_cast<decltype(values_fn)>(GetProcAddress(hid,"HidP_GetValueCaps"));
        buttons_fn=reinterpret_cast<decltype(buttons_fn)>(GetProcAddress(hid,"HidP_GetButtonCaps"));
        value_fn=reinterpret_cast<decltype(value_fn)>(GetProcAddress(hid,"HidP_GetUsageValue"));
        usages_fn=reinterpret_cast<decltype(usages_fn)>(GetProcAddress(hid,"HidP_GetUsages"));
        return caps_fn&&values_fn&&buttons_fn&&value_fn&&usages_fn;
    }
    static bool Has(const HIDP_VALUE_CAPS& cap,USHORT page,USHORT usage) {
        return cap.UsagePage==page&&(cap.IsRange?usage>=cap.Range.UsageMin&&usage<=cap.Range.UsageMax:cap.NotRange.Usage==usage);
    }
    Device* Find(HANDLE handle) {
        for(auto& device:devices)if(device.handle==handle)return &device;
        if(devices.size()>=16||!Parser())return nullptr;
        RID_DEVICE_INFO identity{};identity.cbSize=sizeof(identity);UINT bytes=sizeof(identity);
        if(GetRawInputDeviceInfoW(handle,RIDI_DEVICEINFO,&identity,&bytes)==UINT(-1)||identity.dwType!=RIM_TYPEHID||
           identity.hid.usUsagePage!=0x0D||identity.hid.usUsage!=0x05)return nullptr;
        bytes=0;
        if(GetRawInputDeviceInfoW(handle,RIDI_PREPARSEDDATA,nullptr,&bytes)==UINT(-1)||!bytes||bytes>65536)return nullptr;
        Device device;device.handle=handle;device.prepared.resize(bytes);
        if(GetRawInputDeviceInfoW(handle,RIDI_PREPARSEDDATA,device.prepared.data(),&bytes)==UINT(-1))return nullptr;
        HIDP_CAPS caps{};
        if(caps_fn(device.Data(),&caps)!=HIDP_STATUS_SUCCESS||caps.UsagePage!=0x0D||caps.Usage!=0x05||
           caps.NumberInputValueCaps>256||caps.NumberInputButtonCaps>256)return nullptr;
        USHORT count=caps.NumberInputValueCaps;device.values.resize(count);
        if(count&&values_fn(HidP_Input,device.values.data(),&count,device.Data())!=HIDP_STATUS_SUCCESS)return nullptr;
        device.values.resize(count);count=caps.NumberInputButtonCaps;device.buttons.resize(count);
        if(count&&buttons_fn(HidP_Input,device.buttons.data(),&count,device.Data())!=HIDP_STATUS_SUCCESS)return nullptr;
        device.buttons.resize(count);
        for(const auto& cap:device.values)if(Has(cap,0x0D,0x51)&&
            std::find(device.fingers.begin(),device.fingers.end(),cap.LinkCollection)==device.fingers.end())
            device.fingers.push_back(cap.LinkCollection);
        if(device.fingers.empty()||device.fingers.size()>5)return nullptr;
        std::fprintf(stderr,"Windows touchpad: HID contacts detected (%zu report slots); touchpad emulation enables native touch input\n",device.fingers.size());
        devices.push_back(std::move(device));return &devices.back();
    }
    std::optional<ULONG> Value(Device& device,USHORT page,USHORT usage,USHORT link,char* report,ULONG bytes) {
        ULONG value{};
        if(value_fn(HidP_Input,page,link,usage,&value,device.Data(),report,bytes)!=HIDP_STATUS_SUCCESS)return {};
        return value;
    }
    const HIDP_VALUE_CAPS* ValueCaps(Device& device,USHORT page,USHORT usage,USHORT link) {
        for(const auto& cap:device.values)if(cap.LinkCollection==link&&Has(cap,page,usage))return &cap;
        return nullptr;
    }
    std::optional<bool> Button(Device& device,USHORT page,USHORT usage,USHORT link,char* report,ULONG bytes) {
        bool supported=false;
        for(const auto& cap:device.buttons)if(cap.UsagePage==page&&cap.LinkCollection==link&&
            (cap.IsRange?usage>=cap.Range.UsageMin&&usage<=cap.Range.UsageMax:cap.NotRange.Usage==usage))supported=true;
        if(!supported)return {};
        std::array<USAGE,128> pressed{};ULONG count=pressed.size();
        if(usages_fn(HidP_Input,page,link,pressed.data(),&count,device.Data(),report,bytes)!=HIDP_STATUS_SUCCESS)return {};
        return std::find(pressed.begin(),pressed.begin()+count,usage)!=pressed.begin()+count;
    }
public:
    ~WindowsTouchpad() {
        if(registered) {const RAWINPUTDEVICE device{0x0D,0x05,RIDEV_REMOVE,nullptr};RegisterRawInputDevices(&device,1,sizeof(device));}
        if(hid)FreeLibrary(hid);
    }
    // Read-only capability check; neither opens a game nor captures another app's input.
    size_t Discover() {
        UINT count{};
        if(GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))==UINT(-1)||count>1024)return 0;
        std::vector<RAWINPUTDEVICELIST> list(count);
        if(GetRawInputDeviceList(list.data(),&count,sizeof(RAWINPUTDEVICELIST))==UINT(-1))return 0;
        for(const auto& item:list)if(item.dwType==RIM_TYPEHID)Find(item.hDevice);
        return devices.size();
    }
    bool Attach(HWND target) {
        window=target;if(!Parser())return false;
        const RAWINPUTDEVICE registration{0x0D,0x05,RIDEV_DEVNOTIFY,window};
        registered=RegisterRawInputDevices(&registration,1,sizeof(registration));
        if(!registered)std::fprintf(stderr,"Windows touchpad: raw registration failed error=%lu; mouse dual-button fallback available\n",GetLastError());
        Discover();return registered;
    }
    void Reset() {for(auto& device:devices)device.frames.Reset();}
    void Message(const MSG& message,MouseInput& input,bool allowed,uint64_t now) {
        if(message.hwnd!=window)return;
        if(message.message==WM_INPUT_DEVICE_CHANGE) {
            if(message.wParam==GIDC_REMOVAL) {
                std::erase_if(devices,[&](const Device& d){return d.handle==reinterpret_cast<HANDLE>(message.lParam);});
                input.NativeTouch({},false,now);
            }
            return;
        }
        if(message.message!=WM_INPUT)return;
        if(!allowed||!input.TouchActive()){Reset();return;}
        UINT bytes{};const auto handle=reinterpret_cast<HRAWINPUT>(message.lParam);
        if(GetRawInputData(handle,RID_INPUT,nullptr,&bytes,sizeof(RAWINPUTHEADER))==UINT(-1)||bytes<sizeof(RAWINPUTHEADER)||bytes>65536)return;
        payload.resize(bytes);
        if(GetRawInputData(handle,RID_INPUT,payload.data(),&bytes,sizeof(RAWINPUTHEADER))==UINT(-1))return;
        const auto* raw=reinterpret_cast<const RAWINPUT*>(payload.data());
        if(raw->header.dwType!=RIM_TYPEHID||bytes<offsetof(RAWINPUT,data.hid.bRawData))return;
        auto* device=Find(raw->header.hDevice);if(!device)return;
        const auto& hid_data=raw->data.hid;
        if(!hid_data.dwSizeHid||hid_data.dwSizeHid>4096||hid_data.dwCount>128||
           uint64_t(hid_data.dwSizeHid)*hid_data.dwCount>bytes - offsetof(RAWINPUT,data.hid.bRawData))return;
        for(UINT index=0;index<hid_data.dwCount;++index) {
            char* report=reinterpret_cast<char*>(payload.data()+offsetof(RAWINPUT,data.hid.bRawData)+index*hid_data.dwSizeHid);
            const auto count=Value(*device,0x0D,0x54,0,report,hid_data.dwSizeHid);
            const auto scan=Value(*device,0x0D,0x56,0,report,hid_data.dwSizeHid);
            if(!count||!scan)continue;
            std::array<TouchpadFrames::Finger,5> fingers{};size_t used{};
            for(const USHORT link:device->fingers) {
                const auto id=Value(*device,0x0D,0x51,link,report,hid_data.dwSizeHid);
                const auto x=Value(*device,0x01,0x30,link,report,hid_data.dwSizeHid);
                const auto y=Value(*device,0x01,0x31,link,report,hid_data.dwSizeHid);
                const auto down=Button(*device,0x0D,0x42,link,report,hid_data.dwSizeHid);
                const auto confidence=Button(*device,0x0D,0x47,link,report,hid_data.dwSizeHid);
                const auto* xc=ValueCaps(*device,0x01,0x30,link);const auto* yc=ValueCaps(*device,0x01,0x31,link);
                if(!id||!x||!y||!down||!xc||!yc)continue;
                fingers[used++]={*id,TouchpadFrames::Coordinate(*x,xc->LogicalMin,xc->LogicalMax,1919),
                    TouchpadFrames::Coordinate(*y,yc->LogicalMin,yc->LogicalMax,942),*down&&confidence.value_or(true)};
            }
            const bool pressed=Button(*device,0x09,0x01,0,report,hid_data.dwSizeHid).value_or(false);
            device->frames.Report(*scan,*count,std::span{fingers.data(),used},pressed,[&](auto contacts,bool click) {
                input.NativeTouch(contacts,click,now);
            });
        }
    }
};

} // namespace Input
