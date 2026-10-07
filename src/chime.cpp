// A soft two-note bell, synthesized once and played from memory.
#include "app.h"
#include <mmsystem.h>
#include <vector>
#include <cmath>

void PlayChime()
{
    static std::vector<BYTE> wav;
    if (wav.empty())
    {
        const int rate = 44100;
        const double dur = 3.2;
        const int n = (int)(rate * dur);
        std::vector<int16_t> pcm(n);
        struct Note { double f, start, gain; };
        const Note notes[] = { { 659.25, 0.00, 0.55 }, { 987.77, 0.22, 0.45 } }; // E5, B5
        for (int i = 0; i < n; i++)
        {
            double t = (double)i / rate, s = 0;
            for (const auto& nt : notes)
            {
                double u = t - nt.start;
                if (u < 0) continue;
                double env = (1 - exp(-u * 160)) * exp(-u * 1.6);
                // Fundamental plus a quiet, slightly inharmonic overtone for a bell-like tone.
                s += nt.gain * env * (sin(2 * 3.14159265 * nt.f * u) + 0.18 * sin(2 * 3.14159265 * nt.f * 2.76 * u) * exp(-u * 3));
            }
            double fadeOut = std::min(1.0, (dur - t) / 0.3);
            pcm[i] = (int16_t)(s * fadeOut * 0.16 * 32767);
        }
        DWORD dataBytes = (DWORD)(pcm.size() * 2);
        wav.resize(44 + dataBytes);
        BYTE* p = wav.data();
        auto put32 = [&](size_t off, DWORD v) { memcpy(p + off, &v, 4); };
        auto put16 = [&](size_t off, WORD v) { memcpy(p + off, &v, 2); };
        memcpy(p, "RIFF", 4); put32(4, 36 + dataBytes); memcpy(p + 8, "WAVE", 4);
        memcpy(p + 12, "fmt ", 4); put32(16, 16); put16(20, 1); put16(22, 1);
        put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
        memcpy(p + 36, "data", 4); put32(40, dataBytes);
        memcpy(p + 44, pcm.data(), dataBytes);
    }
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
