// Нормализация названий игр для поиска описания (аудит §18 п.2–3)
#include <QtTest>
#include "TitleNormalizer.h"

class TstTitleNormalizer : public QObject
{
    Q_OBJECT
private slots:
    void normalize_data()
    {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("out");
        // Подзаголовок не отрезается
        QTest::newRow("jedi")    << "Star Wars Jedi: Fallen Order" << "Star Wars Jedi: Fallen Order";
        QTest::newRow("batman")  << "Batman: Arkham City GOTY"     << "Batman: Arkham City";
        // Издания и версии
        QTest::newRow("edition") << "Skyrim Special Edition"        << "Skyrim";
        QTest::newRow("version") << "Hollow Knight (v1.5)"          << "Hollow Knight";
        // Римские цифры: только заглавные, последним словом, от двух букв
        QTest::newRow("bread")   << "I Am Bread"                    << "I Am Bread";
        QTest::newRow("megaman") << "Mega Man X"                    << "Mega Man X";
        QTest::newRow("ff7")     << "Final Fantasy VII"             << "Final Fantasy 7";
        QTest::newRow("ff7r")    << "Final Fantasy VII Remake"      << "Final Fantasy VII Remake";
        QTest::newRow("civ")     << "Sid Meier's Civilization VI"   << "Sid Meier's Civilization 6";
        QTest::newRow("lower")   << "Divinity ii"                   << "Divinity ii";
        QTest::newRow("word")    << "Civilization"                  << "Civilization";
        QTest::newRow("nier")    << "NieR:Automata"                 << "NieR:Automata";
    }
    void normalize()
    {
        QFETCH(QString, in);
        QFETCH(QString, out);
        QCOMPARE(TitleNormalizer::normalize(in), out);
    }

    void variants()
    {
        QCOMPARE(TitleNormalizer::searchVariants("The Witcher 3: Wild Hunt"),
                 (QStringList{"The Witcher 3: Wild Hunt", "The Witcher 3"}));
        QCOMPARE(TitleNormalizer::searchVariants("Hollow Knight"),
                 (QStringList{"Hollow Knight"}));
        QCOMPARE(TitleNormalizer::searchVariants("NieR:Automata"),
                 (QStringList{"NieR:Automata"}));
    }
};

QTEST_GUILESS_MAIN(TstTitleNormalizer)
#include "tst_titlenormalizer.moc"
