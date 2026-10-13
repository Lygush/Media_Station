// i18n.js
// ─────────────────────────────────────────────────────────────────────────────
// Все переводы приложения.
//
// Использование из Settings.qml:
//   import "i18n.js" as I18n
//   function tr(key) { ... I18n.ru[key] / I18n.en[key] ... }
//
// Параметрические строки содержат плейсхолдеры вида {name}.
// Замена делается на стороне вызывающего кода:
//   Settings.tr("del_body_folder_nonempty")
//       .replace("{title}", folderTitle)
//       .replace("{count}", itemCount)
// ─────────────────────────────────────────────────────────────────────────────

.pragma library

// ── Русский ──────────────────────────────────────────────────────────────────
var ru = {
    // Общие действия
    "add":            "Добавить",
    "cancel":         "Отмена",
    "save":           "Сохранить",
    "select":         "Выбрать",
    "edit":           "Редактировать",
    "delete":         "Удалить",
    "exit":           "Выход",
    "shutdown":       "Выключение",
    "install":        "Установить",
    "cancel_install": "Отменить установку",

    // Плитки и папки
    "new_folder":     "Новая папка",
    "new_shortcut":   "Новый ярлык",
    "folder":         "Папка",
    "shortcut":       "Ярлык",
    "edit_tile":      "Редактирование плитки",

    // Поля редактора плитки
    "folder_name":          "Название папки",
    "folder_name_ph":       "Например: Мои игры",
    "shortcut_name_ph":     "Название ярлыка",
    "name":                 "Название",
    "shortcut_type":        "Тип ярлыка",
    "internet":             "Интернет",
    "application":          "Приложение",
    "playstation1":         "PlayStation 1",
    "url":                  "URL страницы",
    "exe_path":             "Путь к исполняемому файлу",
    "url_ph":               "https://...",
    "exe_ph":               "C:/Program Files/...",
    "icon_path":            "Иконка (путь к файлу)",
    "icon_ph":              "C:/icons/icon.png  или  qrc:/...",
    "preview":              "Превью:",
    "install_game_copy_files": "Установить игру (скопировать файлы)",

    // Локальная установка — выбор папки и exe
    "game_folder":        "Папка с игрой",
    "game_folder_ph":     "Выберите корневую папку игры…",
    "select_game_folder": "Выбор папки с игрой",
    "exe_file":           "Исполняемый файл",
    "exe_select_ph":      "Выберите .exe из списка…",
    "no_exe_found":       "Исполняемые файлы не найдены",
    "exe_scanning":       "Поиск исполняемых файлов…",
    "checking_deps":      "Проверка зависимостей…",

    // Браузер файлов
    "file_picker":    "Выбор файла",
    "empty_folder":   "Папка пуста или недоступна",
    "no_file":        "— файл не выбран —",
    "drives":         "Диски",

    // Настройки
    "settings":       "Настройки",
    "appearance":     "Внешний вид",
    "fullscreen":     "Полноэкранный режим",
    "language":       "Язык",
    "install_path":   "Папка для установки игр",
    "general":        "Общие",
    "games":          "Игры",
    "log_upload":     "Логи",
    "ui_scale":       "Масштаб интерфейса",
    "font_size":      "Размер шрифта",
    "enable_fog_shader": "Шейдер тумана",
    "audio_driver":   "Аудио драйвер",
    "audio_timer_period": "Период таймера аудио",
    "dsound_latency": "Задержка DSound",
    "log_status":     "Статус",
    "log_server":     "Сервер логов",
    "search_servers": "Поиск серверов",
    "send_logs":      "Отправить логи",
    "log_progress":   "Отправка…",
    "log_send_success": "Логи успешно отправлены",
    "log_send_error": "Ошибка отправки логов",
    "ok":             "ОК",

    // Экраны навигации
    "Games":          "Игры",
    "TV":             "ТВ",

    // Диалог установки по сети
    "install_request":    "Запрос на установку",
    "server_wants_install": "Сервер хочет установить:\n«{title}»",
    "select_destination": "Выберите экран назначения:",
    "no_screens":         "Нет доступных экранов",
    "destination":        "Назначение:",

    // Диалог дубликата (Main.qml)
    "already_installed_msg": "«{title}» уже установлена.\nУстановить ещё раз?",

    // Диалог удаления
    "delete_title":           "Удаление",
    "del_body_shortcut_installed":
        "Вы хотите удалить ярлык «{title}».\n\nТакже можно удалить установленные файлы игры.",
    "del_body_shortcut_external":
        "Удалить ярлык «{title}»?\n\nФайлы, на которые ссылается ярлык, удалены не будут.",
    "del_body_folder_empty":
        "Удалить папку «{title}»?",
    "del_body_folder_nonempty":
        "Папка «{title}» содержит {count} эл. Все вложенные ярлыки и подпапки будут удалены вместе с ней.",
    "del_shortcut_only":       "Только ярлык",
    "del_shortcut_with_files": "Ярлык + файлы",
    "del_all":                 "Удалить всё",

    // Фазы установки и запуска (Tile.qml)
    "phase_packing":    "Упаковка…",
    "phase_receiving":  "Получение…",
    "phase_extracting": "Распаковка…",
    "phase_installing": "Установка…",
    "phase_launching":  "Запуск…",

    // Библиотека игр
    "library":                   "Библиотека",
    "lib_scanning":              "Сканирование…",
    "lib_found":                 "Найдено: {count}",
    "lib_empty":                 "Игры не найдены.",
    "lib_no_path":               "Укажите папку установки игр в настройках.",
    "lib_col_name":              "Название",
    "lib_col_status":            "Статус",
    "lib_col_size":              "Размер",
    "lib_status_installed":      "Установлена",
    "lib_status_external":       "Внешний ярлык",
    "lib_status_unavailable":    "Недоступна",
    "lib_status_archive":        "Архив",
    "lib_status_archiving":      "Архивирование…",
    "lib_status_extracting":     "Распаковка…",
    "lib_status_no_tile":        "Нет ярлыка",
    "lib_legacy_hint":           "нет метаданных",
    "lib_menu_create_tile":      "Создать ярлык",
    "lib_menu_manage_tiles":     "Управление ярлыками",
    "lib_menu_regenerate_metadata": "Обновить описание и медиа",
    "atv_status":          "Состояние",
    "atv_state_running":   "Работает",
    "atv_state_booting":   "Загружается…",
    "atv_state_stopped":   "Остановлен",
    "atv_emulator":        "Эмулятор",
    "atv_start":           "Запустить",
    "atv_stop":            "Остановить",
    "atv_user_data":       "Данные пользователя (активный профиль)",
    "atv_reset_data":      "Сбросить данные",
    "atv_reset_confirm":   "Удалить установленные приложения и настройки Android TV активного профиля? Действие необратимо.",
    "lib_menu_archive":          "Архивировать",
    "lib_menu_extract":          "Разархивировать",
    "lib_menu_open_folder":      "Открыть папку",
    "lib_menu_reassign":         "Переназначить путь",
    "lib_menu_delete":           "Удалить игру",
    "lib_create_tile_title":     "Создать ярлык",
    "lib_manage_tiles_title":    "Управление ярлыками",
    "lib_tile_for":              "Игра: {name}",
    "lib_create_tile_btn":       "Создать ярлык",
    "lib_archive_confirm":       "Архивировать «{name}»?\nПапка game/ будет удалена, архив сохранён внутри папки игры.",
    "lib_archive_confirm_tiles": "Архивировать «{name}»?\n{count} ярлык(а/ов) будут удалены.\nПапка game/ будет удалена.",
    "lib_archive_btn":           "Архивировать",
    "lib_delete_installed":      "Удалить «{name}»?\nБудут удалены файлы игры и ярлыки.",
    "lib_delete_archive":        "Удалить «{name}»?\nАрхив и папка игры будут удалены безвозвратно.",
    "lib_delete_external":       "Удалить «{name}» из библиотеки?\nСами файлы игры удалены не будут.",
    "lib_delete_btn":            "Удалить",

    // Android TV (AndroidTVScreen.qml)
    "android_tv":    "Android TV",
    "atv_booting":   "Загрузка Android TV…",
    "atv_idle":      "Android TV не запущен",
    "atv_running":   "Android TV запущен",
    "atv_launch":    "Запустить",
    "atv_switch_to": "Открыть",
    "atv_stop":      "Остановить",

    // Оверлей (OverlayScreen.qml)
    "atv_overlay_title":      "Android TV",
    "atv_overlay_back_to_tv": "Продолжить",
    "atv_overlay_exit":       "Выйти",
    "game_overlay_title":     "В игре",
    "game_overlay_resume":    "Продолжить",
    "game_overlay_exit":      "Выйти из игры",
    "overlay_hint":           "◀ ▶ навигация  •  OK выбор  •  Меню закрыть",

    // ── Обновление (UpdateAvailableDialog) ───────────────────────────────────
    "update_available":        "Доступно обновление",
    "version_label":           "Версия {version}",
    "no_changelog":            "Нет описания изменений.",
    "downloading_update":      "Загрузка обновления…",
    "later_btn":               "Позже",
    "install_now_btn":         "Установить сейчас",
    "preparing_update":        "Подготовка обновления…",
    "restart_warning":         "MediaStation будет перезапущен после обновления.",

    // Фаза 2: страница игры (GameDetailScreen.qml)
    "game_details":          "Подробнее",
    "launch_game":           "Запустить",
    "gamedetail_about":      "ОБ ИГРЕ",
    "gamedetail_publisher":  "ИЗДАТЕЛЬ",
    "gamedetail_sysreq":     "СИСТЕМНЫЕ ТРЕБОВАНИЯ",
    "gamedetail_sysreq_min": "Минимальные",
    "gamedetail_sysreq_rec": "Рекомендуемые",
    "gamedetail_no_info":    "Информация об игре ещё не загружена.\nОткройте контекстное меню → «Получить информацию».",
    "gamedetail_fetching":   "Загрузка…",
    "gamedetail_fetch_info": "Получить информацию",
    "gamedetail_more_info":  "Подробнее",
    "gamedetail_stage_db":         "Поиск в базе данных",
    "gamedetail_stage_fetching":   "Получение информации",
    "gamedetail_stage_generating": "Генерация",
    "gamedetail_stage_media":      "Загрузка медиа",
    "gamedetail_cancel_trailer":   "Отменить загрузку",
    "gamedetail_select_app":       "Выберите приложение",
    "gamedetail_next":             "Далее",
    "gamedetail_skip":             "Пропустить",
    "gamedetail_generating_json":  "Генерация JSON…",

    // Профили Android TV (AndroidTvSettings.qml)
    "system_image":     "Образ системы",
    "cores":            "Ядра",
    "ram":              "ОЗУ",
    "resolution":       "Разрешение",
    "profiles":         "Профили",
    "delete_profile":   "Удалить профиль",
    "clear_data":       "Очистить данные",
    "used_space":       "Занято",
    "qcow2_not_found":  "не найден",
    "select_system_image": "Выберите образ системы",
    "no_images_found":  "Образы не найдены",
    "select_manually":  "Выбрать вручную",
    "browse":           "Обзор",
    "active_profile":   "Активный профиль",
    "active":           "Активен",
    "inactive":         "Неактивен",
    "max_profiles_reached": "Достигнуто максимальное количество профилей",
    "add_profile":      "Добавить профиль",
    "profile_name":     "Название профиля",
    "new_profile":      "Новый профиль",
    "confirm_clear_data": "Вы уверены, что хотите очистить данные? Все пользовательские данные Android будут удалены."
}

// ── English ───────────────────────────────────────────────────────────────────
var en = {
    // Общие действия
    "add":            "Add",
    "cancel":         "Cancel",
    "save":           "Save",
    "select":         "Select",
    "edit":           "Edit",
    "delete":         "Delete",
    "exit":           "Exit",
    "shutdown":       "Shutdown",
    "install":        "Install",
    "cancel_install": "Cancel installation",

    // Плитки и папки
    "new_folder":     "New folder",
    "new_shortcut":   "New shortcut",
    "folder":         "Folder",
    "shortcut":       "Shortcut",
    "edit_tile":      "Edit tile",

    // Поля редактора плитки
    "folder_name":          "Folder name",
    "folder_name_ph":       "For example: My Games",
    "shortcut_name_ph":     "Shortcut name",
    "name":                 "Name",
    "shortcut_type":        "Shortcut type",
    "internet":             "Internet",
    "application":          "Application",
    "playstation1":         "PlayStation 1",
    "url":                  "Page URL",
    "exe_path":             "Path to executable",
    "url_ph":               "https://...",
    "exe_ph":               "C:/Program Files/...",
    "icon_path":            "Icon (file path)",
    "icon_ph":              "C:/icons/icon.png  or  qrc:/...",
    "preview":              "Preview:",
    "install_game_copy_files": "Install game (copy files)",

    // Local installation — folder and exe selection
    "game_folder":        "Game folder",
    "game_folder_ph":     "Select the game's root folder…",
    "select_game_folder": "Select game folder",
    "exe_file":           "Executable file",
    "exe_select_ph":      "Select .exe from the list…",
    "no_exe_found":       "No executable files found",
    "exe_scanning":       "Scanning for executables…",
    "checking_deps":      "Checking dependencies…",

    // Браузер файлов
    "file_picker":    "Select file",
    "empty_folder":   "Folder is empty or inaccessible",
    "no_file":        "— no file selected —",
    "drives":         "Drives",

    // Настройки
    "settings":       "Settings",
    "appearance":     "Appearance",
    "fullscreen":     "Fullscreen mode",
    "language":       "Language",
    "install_path":   "Games installation folder",
    "general":        "General",
    "games":          "Games",
    "log_upload":     "Logs",
    "ui_scale":       "UI scale",
    "font_size":      "Font size",
    "enable_fog_shader": "Fog shader",
    "audio_driver":   "Audio driver",
    "audio_timer_period": "Audio timer period",
    "dsound_latency": "DSound latency",
    "log_status":     "Status",
    "log_server":     "Log server",
    "search_servers": "Search servers",
    "send_logs":      "Send logs",
    "log_progress":   "Sending…",
    "log_send_success": "Logs sent successfully",
    "log_send_error": "Log send error",
    "ok":             "OK",

    // Экраны навигации
    "Games":          "Games",
    "TV":             "TV",

    // Диалог установки по сети
    "install_request":    "Install request",
    "server_wants_install": "Server wants to install:\n«{title}»",
    "select_destination": "Select destination screen:",
    "no_screens":         "No screens available",
    "destination":        "Destination:",

    // Диалог дубликата (Main.qml)
    "already_installed_msg": "«{title}» is already installed.\nInstall again?",

    // Диалог удаления
    "delete_title":           "Delete",
    "del_body_shortcut_installed":
        "You are about to delete the shortcut «{title}».\n\nYou can also delete the installed game files.",
    "del_body_shortcut_external":
        "Delete shortcut «{title}»?\n\nThe files this shortcut points to will not be deleted.",
    "del_body_folder_empty":
        "Delete folder «{title}»?",
    "del_body_folder_nonempty":
        "Folder «{title}» contains {count} item(s). All shortcuts and subfolders inside will be deleted too.",
    "del_shortcut_only":       "Shortcut only",
    "del_shortcut_with_files": "Shortcut + files",
    "del_all":                 "Delete all",

    // Фазы установки и запуска (Tile.qml)
    "phase_packing":    "Packing…",
    "phase_receiving":  "Receiving…",
    "phase_extracting": "Extracting…",
    "phase_installing": "Installing…",
    "phase_launching":  "Launching…",

    // Game Library
    "library":                   "Library",
    "lib_scanning":              "Scanning…",
    "lib_found":                 "Found: {count}",
    "lib_empty":                 "No games found.",
    "lib_no_path":               "Set the games folder in Settings.",
    "lib_col_name":              "Name",
    "lib_col_status":            "Status",
    "lib_col_size":              "Size",
    "lib_status_installed":      "Installed",
    "lib_status_external":       "External",
    "lib_status_unavailable":    "Unavailable",
    "lib_status_archive":        "Archive",
    "lib_status_archiving":      "Archiving…",
    "lib_status_extracting":     "Extracting…",
    "lib_status_no_tile":        "No shortcut",
    "lib_legacy_hint":           "no metadata",
    "lib_menu_create_tile":      "Create shortcut",
    "lib_menu_manage_tiles":     "Manage shortcuts",
    "lib_menu_regenerate_metadata": "Refresh info & media",
    "atv_status":          "Status",
    "atv_state_running":   "Running",
    "atv_state_booting":   "Booting…",
    "atv_state_stopped":   "Stopped",
    "atv_emulator":        "Emulator",
    "atv_start":           "Start",
    "atv_stop":            "Stop",
    "atv_user_data":       "User data (active profile)",
    "atv_reset_data":      "Reset data",
    "atv_reset_confirm":   "Delete installed apps and Android TV settings of the active profile? This cannot be undone.",
    "lib_menu_archive":          "Archive",
    "lib_menu_extract":          "Extract",
    "lib_menu_open_folder":      "Open folder",
    "lib_menu_reassign":         "Reassign path",
    "lib_menu_delete":           "Delete game",
    "lib_create_tile_title":     "Create shortcut",
    "lib_manage_tiles_title":    "Manage shortcuts",
    "lib_tile_for":              "Game: {name}",
    "lib_create_tile_btn":       "Create shortcut",
    "lib_archive_confirm":       "Archive «{name}»?\nThe game/ folder will be removed, archive saved inside the game folder.",
    "lib_archive_confirm_tiles": "Archive «{name}»?\n{count} shortcut(s) will be removed.\nThe game/ folder will be deleted.",
    "lib_archive_btn":           "Archive",
    "lib_delete_installed":      "Delete «{name}»?\nGame files and shortcuts will be removed.",
    "lib_delete_archive":        "Delete «{name}»?\nThe archive and game folder will be permanently deleted.",
    "lib_delete_external":       "Remove «{name}» from library?\nThe actual game files will not be deleted.",
    "lib_delete_btn":            "Delete",

    // Android TV (AndroidTVScreen.qml)
    "android_tv":    "Android TV",
    "atv_booting":   "Loading Android TV…",
    "atv_idle":      "Android TV is not running",
    "atv_running":   "Android TV is running",
    "atv_launch":    "Launch",
    "atv_switch_to": "Open",
    "atv_stop":      "Stop",

    // Overlay (OverlayScreen.qml)
    "atv_overlay_title":      "Android TV",
    "atv_overlay_back_to_tv": "Resume",
    "atv_overlay_exit":       "Exit",
    "game_overlay_title":     "In Game",
    "game_overlay_resume":    "Resume",
    "game_overlay_exit":      "Exit Game",
    "overlay_hint":           "◀ ▶ navigate  •  OK select  •  Menu close",

    // ── Update (UpdateAvailableDialog) ───────────────────────────────────────
    "update_available":        "Update available",
    "version_label":           "Version {version}",
    "no_changelog":            "No changelog available.",
    "downloading_update":      "Downloading update…",
    "later_btn":               "Later",
    "install_now_btn":         "Install now",
    "preparing_update":        "Preparing update…",
    "restart_warning":         "MediaStation will restart after the update.",

    // Phase 2: game detail page (GameDetailScreen.qml)
    "game_details":          "Details",
    "launch_game":           "Play",
    "gamedetail_about":      "ABOUT",
    "gamedetail_publisher":  "PUBLISHER",
    "gamedetail_sysreq":     "SYSTEM REQUIREMENTS",
    "gamedetail_sysreq_min": "Minimum",
    "gamedetail_sysreq_rec": "Recommended",
    "gamedetail_no_info":    "Game info has not been loaded yet.\nOpen context menu → \"Get Info\".",
    "gamedetail_fetching":   "Fetching…",
    "gamedetail_fetch_info": "Get Info",
    "gamedetail_more_info":  "More Info",
    "gamedetail_stage_db":         "Searching database",
    "gamedetail_stage_fetching":   "Fetching info",
    "gamedetail_stage_generating": "Generating",
    "gamedetail_stage_media":      "Downloading media",
    "gamedetail_cancel_trailer":   "Cancel download",
    "gamedetail_select_app":       "Select application",
    "gamedetail_next":             "Next",
    "gamedetail_skip":             "Skip",
    "gamedetail_generating_json":  "Generating JSON…",

    // Android TV Profiles (AndroidTvSettings.qml)
    "system_image":     "System image",
    "cores":            "Cores",
    "ram":              "RAM",
    "resolution":       "Resolution",
    "profiles":         "Profiles",
    "delete_profile":   "Delete profile",
    "clear_data":       "Clear data",
    "used_space":       "Used space",
    "qcow2_not_found":  "not found",
    "select_system_image": "Select system image",
    "no_images_found":  "No images found",
    "select_manually":  "Select manually",
    "browse":           "Browse",
    "active_profile":   "Active profile",
    "active":           "Active",
    "inactive":         "Inactive",
    "max_profiles_reached": "Maximum number of profiles reached",
    "add_profile":      "Add profile",
    "profile_name":     "Profile name",
    "new_profile":      "New profile",
    "confirm_clear_data": "Are you sure you want to clear data? All Android user data will be deleted."
}
