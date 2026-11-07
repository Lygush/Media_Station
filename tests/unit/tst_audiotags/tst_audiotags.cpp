// AudioTags на файлах, записанных сторонними средствами: теги — библиотекой
// mutagen (Python), FLAC — libsndfile. ID3v1 и «cp1251 в Latin-1» — как пишут
// старые программы для русских песен.
#include <QtTest>
#include "AudioTags.h"

class TstAudioTags : public QObject {
    Q_OBJECT
    static QString f(const char *name) { return QString(DATA_DIR) + "/" + name; }
private slots:
    void id3v23Utf16() {
        const auto t = AudioTags::read(f("v23_utf16.mp3"));
        QCOMPARE(t.artist, QString("Кино"));
        QCOMPARE(t.title,  QString("Группа крови"));
        QCOMPARE(t.album,  QString("Группа крови"));
    }
    void id3v24Utf8() {
        const auto t = AudioTags::read(f("v24_utf8.mp3"));
        QCOMPARE(t.artist, QString("Daft Punk"));
        QCOMPARE(t.title,  QString("One More Time"));
    }
    void id3v23Cp1251InLatin1() {
        const auto t = AudioTags::read(f("v23_cp1251.mp3"));
        QCOMPARE(t.artist, QString("Сплин"));
        QCOMPARE(t.title,  QString("Выхода нет"));
    }
    void id3v1Cp1251() {
        const auto t = AudioTags::read(f("v1_cp1251.mp3"));
        QCOMPARE(t.artist, QString("Кино"));
        QCOMPARE(t.title,  QString("Звезда по имени Солнце"));
    }
    void flacVorbisAndDuration() {
        const auto t = AudioTags::read(f("song.flac"));
        QCOMPARE(t.artist, QString("Ария"));
        QCOMPARE(t.title,  QString("Беспечный ангел"));
        QCOMPARE(t.album,  QString("Химера"));
        QCOMPARE(t.durationSec, 3);
    }
    void noTagsAndMissingFile() {
        QVERIFY(AudioTags::read(f("notags.mp3")).isEmpty());
        QVERIFY(AudioTags::read(f("nope.mp3")).isEmpty());
    }
    void latin1StaysLatin1() {
        QCOMPARE(AudioTags::decodeSingleByte("Beyonc\xe9"), QString("Beyoncé"));
        QCOMPARE(AudioTags::decodeSingleByte("Mot\xf6rhead"), QString("Motörhead"));
        QCOMPARE(AudioTags::decodeSingleByte("\xdf"), QString("Я"));                  // одна буква целиком
        QCOMPARE(AudioTags::decodeSingleByte("DJ \xd1\xec\xe0\xf8"), QString("DJ Смаш"));
    }
    // Повреждённые данные не роняют разбор
    void truncatedData() {
        QFile in(f("v23_utf16.mp3")); QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray d = in.readAll();
        for (int n = 0; n < 200; n += 7) AudioTags::parseId3v2(d.left(n));
        QFile fl(f("song.flac")); QVERIFY(fl.open(QIODevice::ReadOnly));
        const QByteArray fd = fl.readAll();
        for (int n = 0; n < fd.size(); n += 13) AudioTags::parseFlac(fd.left(n));
    }
};

QTEST_MAIN(TstAudioTags)
#include "tst_audiotags.moc"
