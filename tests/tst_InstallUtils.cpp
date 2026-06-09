// tst_InstallUtils.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Unit-тесты для InstallUtils::parseDependencyAnalyzerOutput.
//
// Как подключить к проекту:
//   1. Создать папку tests/tst_InstallUtils/
//   2. Положить этот файл туда
//   3. Добавить в MediaStation.pro (или создать отдельный .pro):
//
//      QT += testlib
//      CONFIG += c++20 console testcase
//      TARGET = tst_InstallUtils
//      INCLUDEPATH += ../../modules/TileModule
//      SOURCES += tst_InstallUtils.cpp \
//                 ../../modules/TileModule/InstallUtils.cpp
//
// Запуск: ./tst_InstallUtils -v2
// ─────────────────────────────────────────────────────────────────────────────

#include <QtTest/QtTest>
#include "InstallUtils.h"

class TstInstallUtils : public QObject
{
    Q_OBJECT

private slots:
    void allDepsPresent_noMissing();
    void missingDll_appearsInResult();
    void multipleMissingDlls();
    void tabSeparator_parsedCorrectly();
    void xinputDetected();
    void dinputDetected();
    void bothInputTypes_directinputFirst();
    void inputTypesInOtherSections();
    void summarySection_ignored();
    void emptyOutput_emptyResult();
    void realWorldOutput_universeSandbox();
};

// ─────────────────────────────────────────────────────────────────────────────
// Хелпер: строит минимальный вывод PEDependencyAnalyzer
// ─────────────────────────────────────────────────────────────────────────────

static QString makeOutput(const QString &otherSection)
{
    return QString(
        "System Dependencies:\n"
        "--------------------\n"
        "KERNEL32.dll    C:\\Windows\\system32\\KERNEL32.dll\n"
        "\n"
        "Other Dependencies:\n"
        "-------------------\n"
        "%1"
        "Summary:\n"
        "--------\n"
        "Total: 1\n"
    ).arg(otherSection);
}

// ─────────────────────────────────────────────────────────────────────────────
// Тесты
// ─────────────────────────────────────────────────────────────────────────────

void TstInstallUtils::allDepsPresent_noMissing()
{
    // Все dll в Other Dependencies имеют путь → missing пустой.
    const QString out = makeOutput(
        "UnityPlayer.dll    E:\\MyGame\\UnityPlayer.dll\n"
    );
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QVERIFY2(r.missing.isEmpty(),
             qPrintable("Expected no missing, got: " + r.missing.join(", ")));
}

void TstInstallUtils::missingDll_appearsInResult()
{
    // Dll без пути → missing.
    const QString out = makeOutput("SomeMissing.dll\n");
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QCOMPARE(r.missing.size(), 1);
    QCOMPARE(r.missing.first(), QStringLiteral("SomeMissing.dll"));
}

void TstInstallUtils::multipleMissingDlls()
{
    const QString out = makeOutput(
        "Missing1.dll\n"
        "Found.dll    C:\\Windows\\Found.dll\n"
        "Missing2.dll\n"
    );
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QCOMPARE(r.missing.size(), 2);
    QVERIFY(r.missing.contains("Missing1.dll"));
    QVERIFY(r.missing.contains("Missing2.dll"));
    // Found.dll с путём не должна попасть в missing
    QVERIFY(!r.missing.contains("Found.dll"));
}

void TstInstallUtils::tabSeparator_parsedCorrectly()
{
    // Одиночный таб как разделитель — раньше ломало \s{2,}.
    const QString out = makeOutput("UnityPlayer.dll\tE:\\Game\\UnityPlayer.dll\n");
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QVERIFY2(r.missing.isEmpty(),
             "Tab-separated dll with path should not be treated as missing");
}

void TstInstallUtils::xinputDetected()
{
    // xinput в System Dependencies → inputTypes содержит "xinput".
    const QString out =
        "System Dependencies:\n"
        "--------------------\n"
        "xinput1_4.dll    C:\\Windows\\system32\\xinput1_4.dll\n"
        "Other Dependencies:\n"
        "-------------------\n";
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QVERIFY(r.inputTypes.contains("xinput"));
    QVERIFY(!r.inputTypes.contains("directinput"));
}

void TstInstallUtils::dinputDetected()
{
    const QString out =
        "System Dependencies:\n"
        "--------------------\n"
        "dinput8.dll    C:\\Windows\\system32\\dinput8.dll\n"
        "Other Dependencies:\n"
        "-------------------\n";
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QVERIFY(r.inputTypes.contains("directinput"));
    QVERIFY(!r.inputTypes.contains("xinput"));
}

void TstInstallUtils::bothInputTypes_directinputFirst()
{
    // Когда оба присутствуют — directinput должен идти первым.
    const QString out =
        "System Dependencies:\n"
        "--------------------\n"
        "xinput1_4.dll    C:\\Windows\\system32\\xinput1_4.dll\n"
        "dinput8.dll      C:\\Windows\\system32\\dinput8.dll\n"
        "Other Dependencies:\n"
        "-------------------\n";
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QCOMPARE(r.inputTypes.size(), 2);
    QCOMPARE(r.inputTypes.at(0), QStringLiteral("directinput"));
    QCOMPARE(r.inputTypes.at(1), QStringLiteral("xinput"));
}

void TstInstallUtils::inputTypesInOtherSections()
{
    // xinput в Runtime — тоже должен детектироваться.
    const QString out =
        "Runtime Dependencies:\n"
        "---------------------\n"
        "xinput9_1_0.dll    C:\\Windows\\system32\\xinput9_1_0.dll\n"
        "Other Dependencies:\n"
        "-------------------\n";
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QVERIFY(r.inputTypes.contains("xinput"));
}

void TstInstallUtils::summarySection_ignored()
{
    // Строки после "Summary:" не должны влиять на результат.
    const QString out =
        "Other Dependencies:\n"
        "-------------------\n"
        "Other Dependencies:    some weird line in summary that looks like a section\n"
        "Summary:\n"
        "--------\n"
        "FakeMissing.dll\n"   // это в Summary — игнорируем
        "Total: 1\n";
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QVERIFY(!r.missing.contains("FakeMissing.dll"));
}

void TstInstallUtils::emptyOutput_emptyResult()
{
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(QString());
    QVERIFY(r.missing.isEmpty());
    QVERIFY(r.inputTypes.isEmpty());
}

void TstInstallUtils::realWorldOutput_universeSandbox()
{
    // Реальный вывод из задачи — Universe Sandbox x64.exe.
    // UnityPlayer.dll имеет путь → missing должен быть пуст.
    const QString out = R"(System Dependencies:
--------------------
ADVAPI32.dll                             C:\Windows\system32\ADVAPI32.dll
KERNEL32.dll                             C:\Windows\system32\KERNEL32.dll
USER32.dll                               C:\Windows\system32\USER32.dll
d3d11.dll                                C:\Windows\system32\d3d11.dll
Runtime Dependencies:
---------------------
msvcp_win.dll                            C:\Windows\system32\msvcp_win.dll
msvcrt.dll                               C:\Windows\system32\msvcrt.dll
Virtual Dependencies (API Sets):
--------------------------------
api-ms-win-eventing-controller-l1-1-0.dll C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\api-ms-win-eventing-controller-l1-1-0.dll
Other Dependencies:
-------------------
UnityPlayer.dll                          E:\Universe Sandbox\UnityPlayer.dll
Summary:
--------
System Dependencies: 3
Other Dependencies: 1
Total: 4
)";
    const auto r = InstallUtils::parseDependencyAnalyzerOutput(out);
    QVERIFY2(r.missing.isEmpty(),
             qPrintable("Universe Sandbox: expected no missing, got: " + r.missing.join(", ")));
    QVERIFY(r.inputTypes.isEmpty());  // нет xinput/dinput
}

QTEST_MAIN(TstInstallUtils)
#include "tst_InstallUtils.moc"
