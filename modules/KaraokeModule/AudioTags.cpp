// modules/KaraokeModule/AudioTags.cpp
#include "AudioTags.h"

#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

namespace AudioTags {

namespace {

constexpr qint64 kMaxHeadBytes = 4 * 1024 * 1024;   // теги с обложкой бывают по несколько МБ

quint32 be32(const QByteArray &d, int p)
{
    return (quint32(quint8(d[p])) << 24) | (quint32(quint8(d[p + 1])) << 16)
         | (quint32(quint8(d[p + 2])) << 8) | quint32(quint8(d[p + 3]));
}
quint32 le32(const QByteArray &d, int p)
{
    return quint32(quint8(d[p])) | (quint32(quint8(d[p + 1])) << 8)
         | (quint32(quint8(d[p + 2])) << 16) | (quint32(quint8(d[p + 3])) << 24);
}
quint32 syncsafe(const QByteArray &d, int p)
{
    return (quint32(quint8(d[p]) & 0x7F) << 21) | (quint32(quint8(d[p + 1]) & 0x7F) << 14)
         | (quint32(quint8(d[p + 2]) & 0x7F) << 7) | quint32(quint8(d[p + 3]) & 0x7F);
}

// Строка без завершающих нулей и пробелов
QString clean(QString s)
{
    while (!s.isEmpty() && (s.back() == QChar(0) || s.back().isSpace())) s.chop(1);
    return s.trimmed();
}

// Текстовый фрейм ID3v2: первый байт — кодировка
QString decodeId3Text(const QByteArray &frame)
{
    if (frame.isEmpty()) return {};
    const char enc = frame.at(0);
    QByteArray body = frame.mid(1);
    switch (enc) {
    case 0:  // ISO-8859-1 (а на деле часто cp1251)
        return clean(decodeSingleByte(body.left(body.indexOf('\0') >= 0 ? body.indexOf('\0') : body.size())));
    case 1: { // UTF-16 с BOM
        QStringDecoder dec(QStringDecoder::Utf16, QStringDecoder::Flag::Default);
        return clean(dec.decode(body));
    }
    case 2: { // UTF-16BE без BOM
        QStringDecoder dec(QStringDecoder::Utf16BE);
        return clean(dec.decode(body));
    }
    case 3:  // UTF-8
        return clean(QString::fromUtf8(body));
    default:
        return {};
    }
}

} // namespace

QString decodeSingleByte(const QByteArray &bytes)
{
    // cp1251, если среди байтов ≥0x80 преобладают буквы кириллицы (0xC0–0xFF,
    // Ё/ё). В Latin-1 такие байты — À…ÿ, в названиях песен они редки.
    // Одна «лишняя» буква среди латиницы («Beyoncé») — это Latin-1: в cp1251
    // она прочиталась бы как «й». Кириллица — это хотя бы два таких байта или
    // бóльшая часть букв строки.
    int high = 0, cyr = 0, letters = 0;
    for (char c : bytes) {
        const quint8 b = quint8(c);
        if (b >= 0x80) { ++high; if (b >= 0xC0 || b == 0xA8 || b == 0xB8) ++cyr; }
        if (b >= 0x80 || QChar::isLetter(b)) ++letters;
    }
    const bool cp1251 = high > 0 && cyr * 10 >= high * 8 && (cyr >= 2 || cyr * 2 > letters);
    if (!cp1251) return QString::fromLatin1(bytes);
    QString out;
    out.reserve(bytes.size());
    for (char c : bytes) {
        const quint8 b = quint8(c);
        if (b < 0x80)       out += QChar(b);
        else if (b >= 0xC0) out += QChar(0x0410 + (b - 0xC0));   // А…я
        else if (b == 0xA8) out += QChar(0x0401);                // Ё
        else if (b == 0xB8) out += QChar(0x0451);                // ё
        else                out += QChar(0xFFFD);
    }
    return out;
}

Tags parseId3v2(const QByteArray &d)
{
    Tags t;
    if (d.size() < 10 || !d.startsWith("ID3")) return t;
    const int ver   = quint8(d[3]);           // 2, 3 или 4
    const int flags = quint8(d[5]);
    const int size  = int(syncsafe(d, 6));
    const int end   = qMin(d.size(), 10 + size);
    int pos = 10;

    if (ver < 2 || ver > 4) return t;
    if (flags & 0x80) return t;               // unsynchronisation — редкость, не разбираем
    if ((flags & 0x40) && ver >= 3 && pos + 4 <= end)   // расширенный заголовок
        pos += (ver == 4 ? int(syncsafe(d, pos)) : int(be32(d, pos)) + 4);

    const int idLen  = ver == 2 ? 3 : 4;
    const int hdrLen = ver == 2 ? 6 : 10;
    while (pos + hdrLen <= end) {
        const QByteArray id = d.mid(pos, idLen);
        if (id.at(0) == '\0') break;           // дальше — заполнение
        int fsize;
        if (ver == 2)      fsize = (quint8(d[pos + 3]) << 16) | (quint8(d[pos + 4]) << 8) | quint8(d[pos + 5]);
        else if (ver == 3) fsize = int(be32(d, pos + 4));
        else               fsize = int(syncsafe(d, pos + 4));
        pos += hdrLen;
        if (fsize <= 0 || pos + fsize > end) break;
        const QByteArray body = d.mid(pos, fsize);
        if      (id == "TPE1" || id == "TP1") t.artist = decodeId3Text(body);
        else if (id == "TIT2" || id == "TT2") t.title  = decodeId3Text(body);
        else if (id == "TALB" || id == "TAL") t.album  = decodeId3Text(body);
        pos += fsize;
    }
    return t;
}

Tags parseId3v1(const QByteArray &b)
{
    Tags t;
    if (b.size() != 128 || !b.startsWith("TAG")) return t;
    auto field = [&](int off, int len) {
        QByteArray f = b.mid(off, len);
        const int z = f.indexOf('\0');
        if (z >= 0) f.truncate(z);
        return clean(decodeSingleByte(f));
    };
    t.title  = field(3, 30);
    t.artist = field(33, 30);
    t.album  = field(63, 30);
    return t;
}

Tags parseFlac(const QByteArray &d)
{
    Tags t;
    if (d.size() < 8 || !d.startsWith("fLaC")) return t;
    int pos = 4;
    for (bool last = false; !last && pos + 4 <= d.size(); ) {
        const quint8 hdr = quint8(d[pos]);
        last = hdr & 0x80;
        const int type = hdr & 0x7F;
        const int len  = (quint8(d[pos + 1]) << 16) | (quint8(d[pos + 2]) << 8) | quint8(d[pos + 3]);
        pos += 4;
        if (pos + len > d.size()) break;
        if (type == 0 && len >= 18) {                // STREAMINFO
            const quint64 x = (quint64(be32(d, pos + 10)) << 32) | be32(d, pos + 14);
            const quint32 rate    = quint32(x >> 44) & 0xFFFFF;
            const quint64 samples = x & 0xFFFFFFFFFULL;
            if (rate > 0) t.durationSec = int(samples / rate);
        } else if (type == 4) {                      // VORBIS_COMMENT
            int p = pos;
            const int blockEnd = pos + len;
            if (p + 4 > blockEnd) break;
            p += 4 + int(le32(d, p));                // vendor
            if (p + 4 > blockEnd) break;
            const quint32 n = le32(d, p); p += 4;
            for (quint32 i = 0; i < n && p + 4 <= blockEnd; ++i) {
                const int l = int(le32(d, p)); p += 4;
                if (l < 0 || p + l > blockEnd) break;
                const QString kv = QString::fromUtf8(d.mid(p, l));
                p += l;
                const int eq = kv.indexOf(QLatin1Char('='));
                if (eq <= 0) continue;
                const QString key = kv.left(eq).toUpper();
                const QString val = clean(kv.mid(eq + 1));
                if (key == QLatin1String("ARTIST") && t.artist.isEmpty()) t.artist = val;
                else if (key == QLatin1String("TITLE") && t.title.isEmpty()) t.title = val;
                else if (key == QLatin1String("ALBUM") && t.album.isEmpty()) t.album = val;
            }
        }
        pos += len;
    }
    return t;
}

Tags read(const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray head = f.read(10);
    if (head.startsWith("fLaC")) {
        f.seek(0);
        return parseFlac(f.read(kMaxHeadBytes));
    }
    Tags t;
    if (head.startsWith("ID3") && head.size() == 10) {
        const qint64 tagSize = 10 + syncsafe(head, 6);
        f.seek(0);
        t = parseId3v2(f.read(qMin(tagSize, kMaxHeadBytes)));
    }
    if (t.artist.isEmpty() || t.title.isEmpty()) {
        // ID3v1 в последних 128 байтах — дополняет недостающее
        if (f.size() >= 128 && f.seek(f.size() - 128)) {
            const Tags v1 = parseId3v1(f.read(128));
            if (t.artist.isEmpty()) t.artist = v1.artist;
            if (t.title.isEmpty())  t.title  = v1.title;
            if (t.album.isEmpty())  t.album  = v1.album;
        }
    }
    return t;
}

} // namespace AudioTags
