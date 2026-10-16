// modules/KaraokeModule/VoiceActivity.h
// ─────────────────────────────────────────────────────────────────────────────
// Простой энергетический VAD для вокальной дорожки (после Demucs).
//
// Зачем: Whisper на 5-минутной дорожке с длинными инструментальными паузами
// теряет синхронизацию — выдумывает «Thank you» на тишине и размазывает
// повторяющиеся слова по равномерной сетке. Поэтому дорожка режется на участки
// с вокалом (до ~28 с — одно окно Whisper), каждый распознаётся отдельно
// с no_context, а тайм-коды сдвигаются на начало участка.
//
// Без зависимостей от Qt — тестируется отдельно.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace VoiceActivity {

struct Region {
    int start = 0;   // сэмплы
    int end   = 0;
};

struct Result {
    std::vector<Region> regions;
    std::vector<float>  frameRms;       // RMS на кадр 20 мс
    float               threshold = 0.f;
    int                 sampleRate = 16000;

    static constexpr int kFrameMs = 20;

    // Есть ли в интервале [fromMs, toMs] хотя бы один кадр громче порога
    bool isActive(long long fromMs, long long toMs) const
    {
        if (frameRms.empty()) return false;
        long long a = std::max<long long>(0, fromMs / kFrameMs);
        long long b = std::min<long long>(static_cast<long long>(frameRms.size()) - 1,
                                          toMs / kFrameMs);
        for (long long i = a; i <= b; ++i)
            if (frameRms[static_cast<size_t>(i)] > threshold) return true;
        return false;
    }
};

inline Result analyze(const float *pcm, int n, int sampleRate,
                      float maxRegionSec = 28.f,   // окно Whisper — 30 с
                      float mergeGapSec  = 1.5f,   // паузы короче — одна фраза
                      float padSec       = 0.3f,
                      float minActiveSec = 0.3f)
{
    Result r;
    r.sampleRate = sampleRate;
    const int frame = sampleRate * Result::kFrameMs / 1000;     // 320 @ 16 кГц
    if (n < frame) return r;
    const int nF = n / frame;

    r.frameRms.resize(nF);
    for (int i = 0; i < nF; ++i) {
        double s = 0.0;
        const float *p = pcm + static_cast<size_t>(i) * frame;
        for (int j = 0; j < frame; ++j) s += double(p[j]) * p[j];
        r.frameRms[i] = static_cast<float>(std::sqrt(s / frame));
    }

    // Порог: 10% (−20 дБ) от «громкого» уровня (90-й перцентиль), но не ниже шумового пола
    std::vector<float> sorted = r.frameRms;
    std::sort(sorted.begin(), sorted.end());
    const float p90 = sorted[static_cast<size_t>(nF * 0.90)];
    r.threshold = std::max(0.004f, 0.10f * p90);

    // ── Серии активных кадров ────────────────────────────────────────────────
    struct Run { int a, b; int active; };       // кадры [a, b)
    std::vector<Run> runs;
    {
        int i = 0;
        while (i < nF) {
            if (r.frameRms[i] <= r.threshold) { ++i; continue; }
            // серия живёт, пока паузы внутри не длиннее 3 кадров (60 мс)
            int act = 0, quiet = 0, lastActive = i, j = i;
            for (; j < nF; ++j) {
                if (r.frameRms[j] > r.threshold) { quiet = 0; lastActive = j; ++act; }
                else if (++quiet > 3) break;
            }
            if (act >= 4) runs.push_back({ i, lastActive + 1, act });   // ≥ 80 мс
            i = std::max(j, lastActive + 1);
        }
    }

    // ── Слияние близких серий в окна ≤ maxRegionSec ──────────────────────────
    const int fps       = 1000 / Result::kFrameMs;                 // 50
    const int maxFrames = static_cast<int>(maxRegionSec * fps);
    const int gapFrames = static_cast<int>(mergeGapSec  * fps);
    const int minAct    = static_cast<int>(minActiveSec * fps);

    std::vector<Run> merged;
    for (const Run &run : runs) {
        if (!merged.empty()
            && run.a - merged.back().b <= gapFrames
            && run.b - merged.back().a <= maxFrames) {
            merged.back().b       = run.b;
            merged.back().active += run.active;
        } else {
            merged.push_back(run);
        }
    }

    // ── Нарезка слишком длинных (непрерывный вокал/шум) по самому тихому месту ─
    std::vector<Run> pieces;
    for (Run run : merged) {
        while (run.b - run.a > maxFrames) {
            const int lo = run.a + maxFrames / 2;
            const int hi = run.a + maxFrames;
            int cut = lo;
            for (int i = lo; i < hi; ++i)
                if (r.frameRms[i] < r.frameRms[cut]) cut = i;
            pieces.push_back({ run.a, cut, run.active / 2 + 1 });
            run.a = cut;
        }
        pieces.push_back(run);
    }

    // ── Паддинг и перевод в сэмплы ────────────────────────────────────────────
    const int padSamples = static_cast<int>(padSec * sampleRate);
    int prevEnd = 0;
    for (const Run &p : pieces) {
        if (p.active < minAct) continue;
        Region rg;
        rg.start = std::max(prevEnd, p.a * frame - padSamples);
        rg.end   = std::min(n,       p.b * frame + padSamples);
        if (rg.end - rg.start < frame) continue;
        r.regions.push_back(rg);
        prevEnd = rg.end;
    }
    return r;
}

} // namespace VoiceActivity
