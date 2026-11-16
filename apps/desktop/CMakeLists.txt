# Must be set before qt_add_qml_module() to register Theme as a singleton.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(lansync main.cpp app_controller.cpp)

qt_add_qml_module(lansync
    URI LanSync.UI
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/AppButton.qml
        qml/Card.qml
        qml/Badge.qml
        qml/ConfirmDialog.qml
        qml/DialogFooter.qml
        qml/NavButton.qml
        qml/StatCard.qml
        qml/EmptyState.qml
        qml/TransferRow.qml
        qml/VersionsDialog.qml
        qml/DashboardPage.qml
        qml/DevicesPage.qml
        qml/FoldersPage.qml
        qml/TransfersPage.qml
        qml/ConflictsPage.qml
        qml/HistoryPage.qml
        qml/SettingsPage.qml
    SOURCES
        app_controller.hpp
)
target_link_libraries(lansync PRIVATE
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    lansync_app lansync_options)

# Qt's `emit` macro would collide with the core's emit() methods.
target_compile_definitions(lansync PRIVATE QT_NO_KEYWORDS)
