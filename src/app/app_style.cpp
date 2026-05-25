#include "app_style.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>

namespace
{
    QString readTextFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();

        return QString::fromUtf8(file.readAll());
    }
}

void applyGlobalStyle(QApplication &app)
{
    QStringList qssCandidates;
    qssCandidates << (QCoreApplication::applicationDirPath() + QStringLiteral("/conf/app.qss"));

#if defined(Q_OS_LINUX)
#ifdef AIRAN_DESK_INSTALL_DATADIR
    qssCandidates << (QStringLiteral(AIRAN_DESK_INSTALL_DATADIR) + QStringLiteral("/conf/app.qss"));
#endif
    qssCandidates << QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../share/airan-desk/conf/app.qss"));
    qssCandidates << QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../share/airan-desk/conf/app.qss"));
#endif

    qssCandidates << QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../../../resource/conf/app.qss"));

    for (const QString &path : qssCandidates)
    {
        const QString qss = readTextFile(path);
        if (!qss.isEmpty())
        {
            app.setStyleSheet(qss);
            return;
        }
    }

    app.setStyleSheet(QStringLiteral(
        "QWidget { background-color: #181818; color: rgb(131,193,224); }"
        "QDialog, QMainWindow { background-color: #181818; }"
        "QLabel { background: transparent; color: rgb(131,193,224); }"
        "QLineEdit, QComboBox, QSpinBox, QPlainTextEdit, QTextEdit {"
        "    background-color: #181818; color: rgb(131,193,224);"
        "    border: 2px solid #858585; border-radius: 8px; padding: 6px 8px;"
        "    selection-background-color: #783041; selection-color: white;"
        "}"
        "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus, QTextEdit:focus { border-color: rgb(131,193,224); }"
        "QLineEdit[passwordField=\"true\"] { lineedit-password-character: 42; lineedit-password-mask-delay: 0; }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background-color: #242424; color: #e5eaf3; border: 1px solid #4c4d4f; outline: 0; selection-background-color: #783041; }"
        "QComboBox QListView::item { min-height: 34px; padding: 8px 12px; }"
        "QPushButton { background-color: #783041; color: rgb(131,193,224); border: 2px solid #858585; border-radius: 8px; padding: 7px 14px; }"
        "QPushButton:hover { background-color: #e0e0e0; color: #888888; }"
        "QPushButton:pressed { background-color: #d0d0d0; color: #888888; }"
        "QPushButton:disabled { background-color: #303030; color: #777777; border-color: #555555; }"
        "QCheckBox, QRadioButton { background: transparent; color: rgb(131,193,224); spacing: 8px; }"
        "QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; }"
        "QTabWidget::pane { border: 1px solid #3a3a3a; background: #181818; top: -1px; }"
        "QTabBar::tab { background: #202020; color: rgb(131,193,224); border: 1px solid #3a3a3a; padding: 8px 18px; }"
        "QTabBar::tab:selected { background: #181818; border-bottom-color: #181818; }"
        "QTabBar::tab:hover { background: #2a2a2a; }"
        "#settingsWindowFrame { background-color: #151515; border: 1px solid #6b6d71; }"
        "#settingsWindowFrame #appTitleBar { border-bottom: 1px solid #4c4d4f; }"
        "#settingsContent { background-color: #151515; border-top: 1px solid #2f3033; }"
        "#settingsTabs::pane { border: 1px solid #5b5c5f; border-radius: 6px; background: #1f1f1f; }"
        "#settingsTabs QTabBar::tab { background: #181818; border-color: #3a3a3a; border-bottom-color: #5b5c5f; color: #a3a6ad; }"
        "#settingsTabs QTabBar::tab:selected { background: #1f1f1f; color: rgb(131,193,224); border-bottom-color: #1f1f1f; }"
        "#settingsTabs QTabBar::tab:hover { background: #242424; color: #e5eaf3; }"
        "#settingsPage { background-color: #1f1f1f; }"
        "#settingsContent QLabel { color: #cfd3dc; }"
        "#settingsContent QLineEdit, #settingsContent QComboBox, #settingsContent QSpinBox {"
        "    background-color: #242424; border: 1px solid #6b6d71; border-radius: 6px;"
        "    color: #e5eaf3; padding: 6px 10px;"
        "}"
        "#settingsContent QLineEdit:focus, #settingsContent QComboBox:focus, #settingsContent QSpinBox:focus { border: 1px solid rgb(131,193,224); }"
        "#settingsStatusLabel { background-color: #242424; border: 1px solid #4c4d4f; border-radius: 6px; padding: 9px 12px; color: #e5eaf3; }"
        "#settingsHintLabel { color: #909399; }"
        "QGroupBox { border: 1px solid #3a3a3a; border-radius: 6px; margin-top: 10px; padding-top: 12px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
        "QDialogButtonBox QPushButton { min-width: 86px; }"
        "#mainContent { background-color: #181818; color: rgb(131,193,224); }"
        "#MainWindow QLineEdit { background: transparent; border: 0; border-radius: 0; padding: 0; }"
        "#MainWindow QLineEdit[passwordField=\"true\"], #settingsContent QLineEdit[passwordField=\"true\"] { lineedit-password-character: 42; lineedit-password-mask-delay: 0; }"
        "#MainWindow #local_id_border, #MainWindow #local_pwd_border,"
        "#MainWindow #remote_id_border, #MainWindow #remote_pwd_border {"
        "    border: 2px solid #858585; border-radius: 10px; padding: 0 8px;"
        "}"
        "#MainWindow #local_id_label, #MainWindow #local_pwd_label,"
        "#MainWindow #remote_id_label, #MainWindow #remote_pwd_label { color: #6a6a6a; }"
        "#MainWindow QPushButton {"
        "    background-color: #783041; border: 2px solid #858585; border-radius: 10px;"
        "    padding: 5px; font-size: 14px;"
        "}"
        "#MainWindow QPushButton:hover { background-color: #e0e0e0; color: #888888; }"
        "#MainWindow QPushButton:pressed { background-color: #d0d0d0; border: 2px solid #666666; padding-top: 6px; padding-bottom: 4px; color: #888888; }"
        "#MainWindow QPushButton:disabled { background-color: #f0f0f0; border: 2px solid #aaaaaa; color: #888888; }"
        "QTableWidget, QTableView, QTreeView, QListView { background-color: #181818; alternate-background-color: #202020; color: rgb(131,193,224); gridline-color: #3a3a3a; border: 1px solid #3a3a3a; selection-background-color: #783041; }"
        "QHeaderView::section { background-color: #202020; color: rgb(131,193,224); border: 1px solid #3a3a3a; padding: 5px; }"
        "QMenu { background-color: #181818; color: rgb(131,193,224); border: 1px solid #3a3a3a; }"
        "QMenu::item { padding: 7px 24px; }"
        "QMenu::item:selected { background-color: #783041; color: white; }"
        "QScrollBar:vertical { background: #181818; width: 10px; margin: 0; }"
        "QScrollBar:horizontal { background: #181818; height: 10px; margin: 0; }"
        "QScrollBar::handle { background: #555555; border-radius: 5px; }"
        "QScrollBar::handle:hover { background: #777777; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"));
}
