// Settings.qml (singleton)
pragma Singleton
import QtQuick
import AppSettings 1.0
import "../translation/i18n.js" as I18n

QtObject {
    // ── Проксируемые свойства ─────────────────────────────────────────────
    property color  accentColor:     SettingsManager.accentColor
    readonly property string defaultBackground: "qrc:/assets/background.png"
    property string backgroundImage: SettingsManager.backgroundImage !== ""
                                     ? SettingsManager.backgroundImage
                                     : defaultBackground
    property bool   fullscreen:      SettingsManager.fullscreen
    property string language:        SettingsManager.language
    property string installPath:     SettingsManager.installPath
    property double uiScale:         SettingsManager.uiScale
    property int    fontOffset:      SettingsManager.fontOffset
    property bool   enableFogShader: SettingsManager.enableFogShader

    // ── Константы типографики ─────────────────────────────────────────────
    readonly property int baseFontSize:  16
    readonly property int labelFontSize: 16

    // ── Синхронизация → C++ ───────────────────────────────────────────────
    onAccentColorChanged:     SettingsManager.accentColor     = accentColor
    // Фон по умолчанию хранится как пустая строка. Раньше он записывался в
    // настройки как выбор пользователя (цикл привязки при старте).
    onBackgroundImageChanged: {
        var stored = (backgroundImage === defaultBackground) ? "" : backgroundImage
        if (SettingsManager.backgroundImage !== stored)
            SettingsManager.backgroundImage = stored
    }
    onFullscreenChanged:      SettingsManager.fullscreen      = fullscreen
    onLanguageChanged:        SettingsManager.language        = language
    onInstallPathChanged:     SettingsManager.installPath     = installPath
    onUiScaleChanged:         SettingsManager.uiScale         = uiScale
    onFontOffsetChanged:      SettingsManager.fontOffset      = fontOffset
    onEnableFogShaderChanged: SettingsManager.enableFogShader = enableFogShader

    // ── Методы ────────────────────────────────────────────────────────────

    /// Возвращает цвет акцента с заданной прозрачностью (0.0–1.0).
    function accent(alpha) {
        return Qt.rgba(accentColor.r, accentColor.g, accentColor.b, alpha)
    }

    /// Возвращает локализованную строку по ключу.
    function tr(key, args) {
        var map = (language === "en") ? I18n.en : I18n.ru
        var s   = (map[key] !== undefined) ? map[key] : key
        if (args) {
            for (var k in args)
                s = s.replace("{" + k + "}", args[k])
        }
        return s
    }

    /// Масштабирование размеров UI (с учётом uiScale).
    function us(base) { return Math.round(base * SettingsManager.uiScale) }

    /// Масштабирование размера шрифта (базовый кегль + пользовательская коррекция).
    function fs(base) { return base + SettingsManager.fontOffset }

    /// Безопасный перевод: если строка пуста или отсутствует, возвращает сам ключ.
    function safeTr(key) {
        var s = tr(key);
        return (s !== undefined && s !== "") ? s : key;
    }
}
