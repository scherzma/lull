// Mutes the system's sound outputs during a lull and restores exactly what we changed.
#include "app.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <vector>

namespace
{
    // Device ids we muted, '|'-separated. Mirrored into the settings file so that a crash
    // mid-lull can be undone on the next start.
    std::wstring g_muted;

    std::vector<std::wstring> Split(const std::wstring& s)
    {
        std::vector<std::wstring> out;
        size_t start = 0;
        while (start < s.size())
        {
            size_t bar = s.find(L'|', start);
            if (bar == std::wstring::npos) bar = s.size();
            if (bar > start) out.push_back(s.substr(start, bar - start));
            start = bar + 1;
        }
        return out;
    }

    ComPtr<IAudioEndpointVolume> Volume(IMMDevice* dev)
    {
        ComPtr<IAudioEndpointVolume> vol;
        if (dev) dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(vol.GetAddressOf()));
        return vol;
    }
}

void audio::MuteAll()
{
    if (!g_muted.empty()) return;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; i++)
    {
        ComPtr<IMMDevice> dev;
        if (FAILED(devices->Item(i, &dev))) continue;
        auto vol = Volume(dev.Get());
        BOOL muted = TRUE;
        if (!vol || FAILED(vol->GetMute(&muted)) || muted) continue; // leave the user's own mutes alone
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        if (SUCCEEDED(vol->SetMute(TRUE, nullptr)))
        {
            if (!g_muted.empty()) g_muted += L'|';
            g_muted += id;
        }
        CoTaskMemFree(id);
    }
    g_settings.mutedDevices = g_muted;
    SaveSettings();
}

void audio::RestoreAll()
{
    std::wstring ids = g_muted.empty() ? g_settings.mutedDevices : g_muted;
    if (ids.empty()) return;
    ComPtr<IMMDeviceEnumerator> en;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
    {
        for (const auto& id : Split(ids))
        {
            ComPtr<IMMDevice> dev;
            if (FAILED(en->GetDevice(id.c_str(), &dev))) continue;
            if (auto vol = Volume(dev.Get())) vol->SetMute(FALSE, nullptr);
        }
    }
    g_muted.clear();
    g_settings.mutedDevices.clear();
    SaveSettings();
}

// For the watchdog thread: restore without touching shared state beyond a copy of the ids.
void audio::RestoreFromThread()
{
    std::wstring ids = g_muted;
    if (ids.empty()) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
        for (const auto& id : Split(ids))
        {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(en->GetDevice(id.c_str(), &dev)))
                if (auto vol = Volume(dev.Get())) vol->SetMute(FALSE, nullptr);
        }
    CoUninitialize();
}
