// modules/KaraokeModule/DemucsDsp.h
// ─────────────────────────────────────────────────────────────────────────────
// DSP для HTDemucs (без зависимостей от Qt — можно тестировать отдельно).
//
// Повторяет HTDemucs._spec / _ispec из демакса (facebookresearch/demucs):
//
//   _spec:  le  = ceil(length / hop)
//           pad = hop / 2 * 3                      (1536)
//           x   = reflect_pad(x, pad, pad + le*hop - length)
//           z   = stft(x, nfft=4096, hop=1024, hann(periodic), center=True,
//                      pad_mode=reflect, normalized=True)
//           z   = z[:-1 (последний бин, Nyquist), 2 : 2 + le]   → 2048 × le
//           CaC: каналы [re_L, im_L, re_R, im_R]
//
//   _ispec: z = pad(z, Nyquist = 0), pad(z, 2 кадра с каждой стороны)
//           x = istft(z, normalized=True, center=True)
//           x = x[pad : pad + length]
//
// ВАЖНО: DC-бин (0) ОСТАЁТСЯ (бины 0..2047), отбрасывается только Nyquist.
// Старая реализация делала наоборот (бины 1..2048) и не делала reflect-padding /
// center — модель получала неверный спектр, отсюда пустой вокал.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "kiss/kiss_fftr.h"

namespace DemucsDsp {

constexpr int kNfft  = 4096;
constexpr int kHop   = 1024;
constexpr int kFreqs = kNfft / 2;                // 2048 (без Nyquist)
constexpr int kPad   = kHop / 2 * 3;             // 1536

inline int numFrames(int length) { return (length + kHop - 1) / kHop; }

class Stft
{
public:
    Stft()
    {
        m_window.resize(kNfft);
        const double twoPi = 6.283185307179586476925286766559;
        for (int i = 0; i < kNfft; ++i)   // torch.hann_window(n) — periodic
            m_window[i] = static_cast<float>(0.5 * (1.0 - std::cos(twoPi * i / kNfft)));
        m_fwd = kiss_fftr_alloc(kNfft, 0, nullptr, nullptr);
        m_inv = kiss_fftr_alloc(kNfft, 1, nullptr, nullptr);
    }
    ~Stft()
    {
        if (m_fwd) kiss_fft_free(m_fwd);
        if (m_inv) kiss_fft_free(m_inv);
    }
    Stft(const Stft &)            = delete;
    Stft &operator=(const Stft &) = delete;

    // left/right: `length` сэмплов. out → 4 × kFreqs × frames (frames = ceil(length/hop)),
    // индекс: ((ch * kFreqs) + f) * frames + t, ch: 0=re_L 1=im_L 2=re_R 3=im_R.
    void computeCac(const float *left, const float *right, int length,
                    std::vector<float> &out) const
    {
        const int frames = numFrames(length);
        const size_t plane = static_cast<size_t>(kFreqs) * frames;
        out.assign(4 * plane, 0.f);

        std::vector<float>        buf(kNfft);
        std::vector<kiss_fft_cpx> spec(kNfft / 2 + 1);
        const float scale = 1.0f / std::sqrt(static_cast<float>(kNfft));   // normalized=True

        for (int ch = 0; ch < 2; ++ch) {
            const float *x = (ch == 0) ? left : right;
            float *re = out.data() + (2 * ch)     * plane;
            float *im = out.data() + (2 * ch + 1) * plane;

            for (int t = 0; t < frames; ++t) {
                // Кадр z[t] (после среза 2:2+le) начинается в исходном сигнале с t*hop - pad
                const int start = t * kHop - kPad;
                for (int i = 0; i < kNfft; ++i)
                    buf[i] = reflectSample(x, length, start + i) * m_window[i];

                kiss_fftr(m_fwd, buf.data(), spec.data());

                for (int f = 0; f < kFreqs; ++f) {
                    re[static_cast<size_t>(f) * frames + t] = spec[f].r * scale;
                    im[static_cast<size_t>(f) * frames + t] = spec[f].i * scale;
                }
            }
        }
    }

    // cac: 4 × kFreqs × frames (как из computeCac / выход модели).
    // outL/outR: `length` сэмплов.
    void inverseCac(const float *cac, int length, float *outL, float *outR) const
    {
        const int frames = numFrames(length);
        const size_t plane = static_cast<size_t>(kFreqs) * frames;

        const int zFrames = frames + 4;                       // +2 нулевых кадра с каждой стороны
        const int olaLen  = kHop * (zFrames - 1) + kNfft;
        const float scale = 1.0f / std::sqrt(static_cast<float>(kNfft));

        std::vector<float>        ola(olaLen), env(olaLen), frame(kNfft);
        std::vector<kiss_fft_cpx> spec(kNfft / 2 + 1);

        for (int ch = 0; ch < 2; ++ch) {
            const float *re = cac + (2 * ch)     * plane;
            const float *im = cac + (2 * ch + 1) * plane;
            float *out = (ch == 0) ? outL : outR;

            std::fill(ola.begin(), ola.end(), 0.f);
            std::fill(env.begin(), env.end(), 0.f);

            for (int tz = 0; tz < zFrames; ++tz) {
                const int t = tz - 2;
                const bool valid = (t >= 0 && t < frames);

                if (valid) {
                    for (int f = 0; f < kFreqs; ++f) {
                        spec[f].r = re[static_cast<size_t>(f) * frames + t];
                        spec[f].i = im[static_cast<size_t>(f) * frames + t];
                    }
                    spec[kFreqs].r = 0.f;      // Nyquist = 0
                    spec[kFreqs].i = 0.f;
                    kiss_fftri(m_inv, spec.data(), frame.data());
                }

                const int pos = tz * kHop;
                for (int i = 0; i < kNfft; ++i) {
                    const float w = m_window[i];
                    if (valid)
                        ola[pos + i] += frame[i] * scale * w;
                    env[pos + i] += w * w;      // огибающая считается и для нулевых кадров
                }
            }

            // center=True: срезаем kNfft/2 слева; затем _ispec срезает kPad
            const int base = kNfft / 2 + kPad;
            for (int i = 0; i < length; ++i) {
                const float e = env[base + i];
                out[i] = (e > 1e-11f) ? ola[base + i] / e : 0.f;
            }
        }
    }

private:
    // torch reflect: x[-k] = x[k], x[n-1+k] = x[n-1-k]
    static float reflectSample(const float *x, int n, int idx)
    {
        if (idx < 0)  idx = -idx;
        if (idx >= n) idx = 2 * (n - 1) - idx;
        if (idx < 0)  idx = 0;
        if (idx >= n) idx = n - 1;
        return x[idx];
    }

    std::vector<float> m_window;
    kiss_fftr_cfg      m_fwd = nullptr;
    kiss_fftr_cfg      m_inv = nullptr;
};

} // namespace DemucsDsp
