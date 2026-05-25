#include "control_window.h"

#include "file_transfer_window.h"
#include "util/config_util.h"
#include "util/json_util.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QMenu>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QTimer>

namespace
{
constexpr int kCaptureBackendEnabledRole = Qt::UserRole + 100;

QString networkPathLabel(const QString &path)
{
    if (path == QStringLiteral("direct"))
        return QCoreApplication::translate("ControlWindow", "Direct");
    if (path == QStringLiteral("turn_udp"))
        return QCoreApplication::translate("ControlWindow", "TURN UDP");
    if (path == QStringLiteral("turn_tcp"))
        return QCoreApplication::translate("ControlWindow", "TURN TCP");
    return QCoreApplication::translate("ControlWindow", "Auto");
}
} /* namespace */
void ControlWindow::onSwitchScreenClicked()
{
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_SWITCH_SCREEN)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Switch screen request sent to remote: {}", remote_id);
}


void ControlWindow::sendAndroidNavigation(const QString &action)
{
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_ANDROID_NAVIGATION)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_ACTION, action)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Android navigation requested: {}", action);
}

void ControlWindow::onRemoteOsChanged(const QString &osName)
{
    m_remoteOsName = osName.trimmed();
    const bool android = osName.trimmed().toLower().contains(QStringLiteral("android"));
    setAndroidNavigationVisible(android);
    LOG_INFO("Remote OS changed: {}, androidNavigationVisible={}", osName, android);
}

void ControlWindow::onQualityModeChanged(int index)
{
    if (index < 0 || !m_qualityModeCombo)
        return;

    const QString mode = m_qualityModeCombo->itemData(index).toString();
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_QUALITY, mode)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Video quality mode changed: {}", mode);
}

void ControlWindow::onMoreMenuRequested()
{
    if (!ConfigUtil->showUI)
        return;

    if (!m_moreMenu || !m_moreBtn)
        return;
    m_moreMenu->exec(m_moreBtn->mapToGlobal(QPoint(0, m_moreBtn->height())));
}

void ControlWindow::onChannelModeSelected(QAction *action)
{
    if (!action)
        return;

    m_streamMode = action->data().toString();
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_QUALITY, m_streamMode)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Video channel mode changed: {}", m_streamMode);
}

void ControlWindow::onBitrateProfileSelected(QAction *action)
{
    if (!action)
        return;

    m_bitrateProfile = action->data().toString();
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_BITRATE_PROFILE, m_bitrateProfile)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Video bitrate profile changed: {}", m_bitrateProfile);
}

void ControlWindow::onResolutionSelected(QAction *action)
{
    if (!action)
        return;

    const QSize resolution = action->data().toSize();
    const int width = resolution.width();
    const int height = resolution.height();

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_WIDTH, width)
                          .add(Constant::KEY_HEIGHT, height)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);

    if (width > 0 && height > 0)
        LOG_INFO("Transfer resolution changed: {}x{}", width, height);
    else
        LOG_INFO("Transfer resolution changed: original");
}

void ControlWindow::onCaptureBackendSelected(QAction *action)
{
    if (!action)
        return;

    const QString backend = action->data().toString();
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_CAPTURE_BACKEND, backend)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Capture backend change requested: {}", backend);

    m_pendingCaptureBackend = backend;
    if (m_captureBackendRollbackTimer)
        m_captureBackendRollbackTimer->start(3000);
}

void ControlWindow::onNetworkPathSelected(QAction *action)
{
    if (!action)
        return;

    m_networkPath = action->data().toString();
    QMetaObject::invokeMethod(&m_rtc_ctl, "setNetworkPath", Qt::QueuedConnection,
                              Q_ARG(QString, m_networkPath));
    LOG_INFO("Network path change requested: {}", m_networkPath);
}

void ControlWindow::onNetworkPathStateChanged(const QStringList &availablePaths, const QString &selectedPath, const QString &requestedPath)
{
    if (!m_networkActionGroup)
        return;

    const QString current = selectedPath.isEmpty() ? requestedPath : selectedPath;
    const QStringList visiblePaths = availablePaths.isEmpty() ? QStringList{QStringLiteral("auto")} : availablePaths;
    QSignalBlocker blocker(m_networkActionGroup);

    for (QAction *action : m_networkActionGroup->actions())
    {
        const QString path = action->data().toString();
        const bool isAuto = path == QStringLiteral("auto");
        const bool isRequested = path == requestedPath;
        const bool isCurrent = path == current;
        const bool available = isAuto || visiblePaths.contains(path);

        action->setVisible(isAuto || available || isRequested || isCurrent);
        action->setEnabled(isAuto || available || isRequested || isCurrent);
        action->setChecked(path == (requestedPath.isEmpty() ? QStringLiteral("auto") : requestedPath));

        QString text = networkPathLabel(path);
        if (isCurrent)
            text += tr("（当前）");
        else if (available && !isAuto)
            text += tr("（可用）");
        else if (isRequested && selectedPath.isEmpty())
            text += tr("（协商中）");
        action->setText(text);

        QString tooltip = tr("请求：%1，当前：%2")
                              .arg(networkPathLabel(requestedPath.isEmpty() ? QStringLiteral("auto") : requestedPath),
                                   networkPathLabel(current.isEmpty() ? QStringLiteral("auto") : current));
        if (!available && !isAuto)
            tooltip += tr("，本次协商未发现该候选路径");
        action->setToolTip(tooltip);
    }

    LOG_INFO("Network path state updated: requested={}, selected={}, available={}",
             requestedPath, selectedPath, visiblePaths.join(","));
}

void ControlWindow::onDisplayModeSelected(QAction *action)
{
    if (!action)
        return;

    const QString mode = action->data().toString();
    m_fitToWindow = (mode == "fit");
    scrollArea.setWidgetResizable(m_fitToWindow);
    label.setSizePolicy(m_fitToWindow ? QSizePolicy::Ignored : QSizePolicy::Expanding, QSizePolicy::Ignored);
    scrollArea.setVerticalScrollBarPolicy(m_fitToWindow ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    scrollArea.setHorizontalScrollBarPolicy(m_fitToWindow ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);

    if (!m_fitToWindow && !m_sourcePixmap.isNull())
        label.resize(m_sourcePixmap.size());
    updateScaledPixmap();
    updateToolbarPosition();
    LOG_INFO("Display mode changed: {}", m_fitToWindow ? "fit-to-window" : "actual-size");
}

void ControlWindow::onRemoteOperationTriggered()
{
    if (!ConfigUtil->showUI)
        return;

    QMenu menu(this);
    menu.addAction(tr("锁定"))->setData("lock");
    menu.addAction(tr("注销"))->setData("logoff");
    menu.addAction(tr("重新启动"))->setData("restart");
    menu.addAction(tr("关机"))->setData("shutdown");
    menu.addAction(tr("资源管理器"))->setData("resource_manager");
    menu.addAction(tr("任务管理器"))->setData("task_manager");

    QAction *selected = menu.exec(m_remoteOperationBtn ? m_remoteOperationBtn->mapToGlobal(QPoint(0, m_remoteOperationBtn->height())) : QCursor::pos());
    if (!selected)
        return;

    const QString action = selected->data().toString();
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_REMOTE_OPERATION)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_ACTION, action)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Remote operation requested: {}", action);
}

void ControlWindow::onCaptureBackendChanged(int index)
{
    if (index < 0 || !m_captureBackendCombo)
        return;

    const QString backend = m_captureBackendCombo->itemData(index).toString();
    const QVariant enabledData = m_captureBackendCombo->itemData(index, kCaptureBackendEnabledRole);
    const bool enabled = !enabledData.isValid() || enabledData.toBool();
    if (!enabled)
    {
        LOG_WARN("Capture backend change ignored because toolbar item is disabled: {}", backend);
        return;
    }

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_CAPTURE_BACKEND, backend)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Capture backend change requested: {}", backend);

    m_pendingCaptureBackend = backend;
    if (m_captureBackendRollbackTimer)
    {
        m_captureBackendRollbackTimer->start(3000);
    }
}

void ControlWindow::onTransferResolutionChanged(int index)
{
    if (index < 0 || !m_resolutionCombo)
        return;

    const QSize resolution = m_resolutionCombo->itemData(index).toSize();
    const int width = resolution.width();
    const int height = resolution.height();

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_WIDTH, width)
                          .add(Constant::KEY_HEIGHT, height)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);

    if (width > 0 && height > 0)
    {
        LOG_INFO("Transfer resolution changed: {}x{}", width, height);
    }
    else
    {
        LOG_INFO("Transfer resolution changed: original");
    }
}

void ControlWindow::onDisplayModeChanged(int index)
{
    if (index < 0 || !m_displayModeCombo)
        return;

    const QString mode = m_displayModeCombo->itemData(index).toString();
    m_fitToWindow = (mode == "fit");
    scrollArea.setWidgetResizable(m_fitToWindow);
    label.setSizePolicy(m_fitToWindow ? QSizePolicy::Ignored : QSizePolicy::Expanding, QSizePolicy::Ignored);
    scrollArea.setVerticalScrollBarPolicy(m_fitToWindow ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    scrollArea.setHorizontalScrollBarPolicy(m_fitToWindow ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);

    if (!m_fitToWindow && !m_sourcePixmap.isNull())
    {
        label.resize(m_sourcePixmap.size());
    }
    updateScaledPixmap();
    updateToolbarPosition();
    LOG_INFO("Display mode changed: {}", m_fitToWindow ? "fit-to-window" : "actual-size");
}
