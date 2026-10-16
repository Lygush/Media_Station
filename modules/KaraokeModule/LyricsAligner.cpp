// modules/KaraokeModule/LyricsAligner.cpp
#include "LyricsAligner.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace {

struct RefWord {
    QString text;       // как в тексте песни
    QString norm;       // для сравнения (может быть пустым — «—», «&»)
    int     line = 0;
    bool    matched = false;
    qint64  t0 = 0, t1 = 0;
};

// Секции вида [Chorus] / (Припев) / [Куплет 1] — не поются
bool isSectionTag(const QString &line)
{
    const QString t = line.trimmed();
    return t.size() >= 3
        && ((t.startsWith('[') && t.endsWith(']')) || (t.startsWith('(') && t.endsWith(')') && t.size() < 24));
}

} // namespace

QString LyricsAligner::normalize(const QString &word)
{
    QString out;
    out.reserve(word.size());
    for (const QChar ch : word) {
        if (ch.isLetterOrNumber()) {
            QChar c = ch.toLower();
            if (c == QChar(0x0451)) c = QChar(0x0435);         // ё → е
            out += c;
        }
    }
    return out;
}

double LyricsAligner::similarity(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    if (a == b) return 1.0;
    const int n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, sub });
        }
        std::swap(prev, cur);
    }
    return 1.0 - double(prev[m]) / std::max(n, m);
}

QString LyricsAligner::promptVocabulary(const QString &refText, int minLen, int maxChars)
{
    QStringList uniq;
    QSet<QString> seen;
    const QStringList lines = refText.split('\n');
    for (const QString &line : lines) {
        if (isSectionTag(line)) continue;
        for (const QString &w : line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
            const QString n = normalize(w);
            if (n.size() < minLen || seen.contains(n)) continue;
            seen.insert(n);
            uniq << w.trimmed().remove(QRegularExpression("^[\\p{P}\\p{S}]+|[\\p{P}\\p{S}]+$"));
        }
    }
    QString out;
    for (const QString &w : uniq) {
        if (w.isEmpty()) continue;
        if (out.size() + w.size() + 2 > maxChars) break;
        if (!out.isEmpty()) out += QStringLiteral(", ");
        out += w;
    }
    return out;
}

LyricsAligner::Result LyricsAligner::align(const QString &refText,
                                           const QVector<WhisperWord> &rec)
{
    Result res;

    // ── Эталонные слова по строкам ────────────────────────────────────────────
    std::vector<RefWord> ref;
    int lineNo = 0;
    for (const QString &line : refText.split('\n')) {
        const QString t = line.trimmed();
        if (t.isEmpty() || isSectionTag(t)) continue;
        const QStringList parts = t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        for (const QString &w : parts) {
            RefWord r;
            r.text = w;
            r.norm = normalize(w);
            r.line = lineNo;
            ref.push_back(r);
        }
        ++lineNo;
    }
    const int N = int(ref.size());
    const int M = rec.size();
    res.refWords = N;
    if (N == 0) return res;

    std::vector<QString> recNorm(M);
    for (int j = 0; j < M; ++j) recNorm[j] = normalize(rec[j].text);

    // ── Выравнивание (глобальное, монотонное) ────────────────────────────────
    // dp[i][j] — минимальная стоимость для первых i эталонных и j распознанных.
    // Пропуск слова с любой стороны = 1; замена = 1 − схожесть, если слова
    // похожи (≥ 0.5), иначе замена запрещена (дешевле пропустить оба).
    if (M > 0) {
        const size_t W = size_t(M) + 1;
        std::vector<float>  dp((size_t(N) + 1) * W);
        std::vector<char>   bt((size_t(N) + 1) * W);     // 0 диаг., 1 пропуск ref, 2 пропуск rec
        auto at = [&](int i, int j) -> size_t { return size_t(i) * W + size_t(j); };

        for (int i = 0; i <= N; ++i) { dp[at(i, 0)] = float(i); bt[at(i, 0)] = 1; }
        for (int j = 0; j <= M; ++j) { dp[at(0, j)] = float(j); bt[at(0, j)] = 2; }

        for (int i = 1; i <= N; ++i) {
            for (int j = 1; j <= M; ++j) {
                float best = dp[at(i - 1, j)] + 1.f; char how = 1;            // слово не расслышано
                const float skipRec = dp[at(i, j - 1)] + 1.f;                  // лишнее слово Whisper
                if (skipRec < best) { best = skipRec; how = 2; }

                const QString &a = ref[size_t(i - 1)].norm;
                const QString &b = recNorm[size_t(j - 1)];
                if (!a.isEmpty() && !b.isEmpty()) {
                    const double s = similarity(a, b);
                    if (s >= 0.5) {
                        const float diag = dp[at(i - 1, j - 1)] + float(1.0 - s);
                        if (diag <= best) { best = diag; how = 0; }
                    }
                }
                dp[at(i, j)] = best;
                bt[at(i, j)] = how;
            }
        }

        // Обратный ход: фиксируем пары
        int i = N, j = M;
        while (i > 0 && j > 0) {
            const char how = bt[at(i, j)];
            if (how == 0) {
                RefWord &r = ref[size_t(i - 1)];
                r.matched = true;
                r.t0 = rec[j - 1].t0_ms;
                r.t1 = std::max(rec[j - 1].t1_ms, rec[j - 1].t0_ms);
                ++res.matchedWords;
                --i; --j;
            } else if (how == 1) {
                --i;
            } else {
                --j;
            }
        }
    }
    res.matchedRatio = double(res.matchedWords) / N;

    // ── Время для не расслышанных слов: интерполяция между опорными ──────────
    constexpr qint64 kDefaultWordMs = 380;     // типичная длительность спетого слова
    constexpr qint64 kMaxGapPerWord = 800;

    int firstAnchor = -1, lastAnchor = -1;
    for (int k = 0; k < N; ++k)
        if (ref[size_t(k)].matched) { if (firstAnchor < 0) firstAnchor = k; lastAnchor = k; }

    if (firstAnchor < 0) {                      // совпадений нет — время не определить
        res.lines.clear();
        return res;
    }

    // слова перед первой опорой — назад от неё
    for (int k = firstAnchor - 1; k >= 0; --k) {
        RefWord &r = ref[size_t(k)];
        const RefWord &next = ref[size_t(k + 1)];
        r.t1 = std::max<qint64>(0, next.t0 - 40);
        r.t0 = std::max<qint64>(0, r.t1 - kDefaultWordMs);
    }
    // слова после последней опоры — вперёд от неё
    for (int k = lastAnchor + 1; k < N; ++k) {
        RefWord &r = ref[size_t(k)];
        const RefWord &prev = ref[size_t(k - 1)];
        r.t0 = prev.t1 + 40;
        r.t1 = r.t0 + kDefaultWordMs;
    }
    // промежутки между опорными
    int k = firstAnchor;
    while (k < lastAnchor) {
        int a = k;
        int b = a + 1;
        while (b <= lastAnchor && !ref[size_t(b)].matched) ++b;
        if (b - a > 1) {
            const qint64 gapStart = ref[size_t(a)].t1;
            const qint64 gapEnd   = std::max(gapStart, ref[size_t(b)].t0);
            const int    cnt      = b - a - 1;

            double totalW = 0;
            for (int q = a + 1; q < b; ++q) totalW += ref[size_t(q)].text.size() + 1;

            qint64 span = gapEnd - gapStart;
            qint64 begin = gapStart;
            if (span > cnt * kMaxGapPerWord) {            // длинная пауза: слова — по центру
                const qint64 used = cnt * kMaxGapPerWord;
                begin = gapStart + (span - used) / 2;
                span  = used;
            }
            double acc = 0;
            for (int q = a + 1; q < b; ++q) {
                RefWord &r = ref[size_t(q)];
                const double w = r.text.size() + 1;
                r.t0 = begin + qint64(span * acc / totalW);
                r.t1 = begin + qint64(span * (acc + w) / totalW);
                acc += w;
            }
        }
        k = b;
    }

    // монотонность: время не должно идти назад
    qint64 floorMs = 0;
    for (RefWord &r : ref) {
        if (r.t0 < floorMs) r.t0 = floorMs;
        if (r.t1 < r.t0)    r.t1 = r.t0;
        floorMs = r.t0;
    }

    // ── Группировка по строкам эталона ────────────────────────────────────────
    res.lines.resize(lineNo);
    for (const RefWord &r : ref) {
        WhisperWord w;
        w.text  = r.text;
        w.t0_ms = r.t0;
        w.t1_ms = r.t1;
        w.prob  = r.matched ? 1.0f : 0.5f;
        res.lines[r.line].append(w);
    }
    return res;
}
