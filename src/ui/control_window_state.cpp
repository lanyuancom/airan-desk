/* Split from control_window_view.cpp to keep ControlWindow UI responsibilities modular. */

#include "control_window.h"

#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>

void ControlWindow::onRemoteCaptureBackendsChanged(const QStringList &backends, const QString &currentBackend)
{
    if (!m_captureActionGroup)
        return;

    QStringList effectiveBackends = backends;
    effectiveBackends.removeAll(QStringLiteral("auto"));
    if (effectiveBackends.isEmpty())
    {
        effectiveBackends = QStringList{QStringLiteral("qt")};
    }
    for (QAction *action : m_captureActionGroup->actions())
    {
        const QString backend = action->data().toString();
        const bool enabled = effectiveBackends.contains(backend);
        action->setEnabled(enabled);
        action->setVisible(enabled);
        LOG_INFO("Toolbar capture backend item: backend={}, enabled={}", backend, enabled);
    }

    if (currentBackend == QStringLiteral("wgc") || currentBackend == QStringLiteral("qt"))
    {
        for (QAction *action : m_captureActionGroup->actions())
        {
            if (action->data().toString() == currentBackend)
            {
                QSignalBlocker blocker(m_captureActionGroup);
                action->setChecked(true);
                m_confirmedCaptureBackend = currentBackend;
                m_pendingCaptureBackend.clear();
                if (m_captureBackendRollbackTimer)
                    m_captureBackendRollbackTimer->stop();
                break;
            }
        }
    }
    LOG_INFO("Toolbar capture backends synced from remote: current={}, available={}", currentBackend, effectiveBackends.join(","));
}

void ControlWindow::onRemoteStreamModeChanged(const QString &mode)
{
    if (!m_channelActionGroup)
        return;

    for (QAction *action : m_channelActionGroup->actions())
    {
        if (action->data().toString() == mode)
        {
            QSignalBlocker blocker(m_channelActionGroup);
            action->setChecked(true);
            m_streamMode = mode;
            LOG_INFO("Toolbar stream mode synced from remote: {}", mode);
            return;
        }
    }
}

void ControlWindow::refreshStatsLabel()
{
    if (!m_statsLabel)
        return;

    const QString resolution = m_remoteResolution.isValid()
                                   ? QString("%1x%2").arg(m_remoteResolution.width()).arg(m_remoteResolution.height())
                                   : QString("--");
    const QString encoder = m_remoteEncoderName.isEmpty() ? QStringLiteral("--") : m_remoteEncoderName;
    QStringList encoderTypes;
    if (m_remoteEncoderType.isEmpty())
    {
        encoderTypes << QStringLiteral("--");
    }
    else
    {
        encoderTypes << (m_remoteEncoderType == QStringLiteral("hardware")
                             ? tr("硬编")
                             : tr("软编"));
    }
    if (m_remoteEncoderZeroCopy)
    {
        encoderTypes << tr("零拷贝");
    }
    m_statsLabel->setText(QString("FPS: %1 | Kbps: %2 | %3 | %4 (%5)")
                              .arg(m_currentFps, 0, 'f', 1)
                              .arg(m_currentKbps, 0, 'f', 0)
                              .arg(resolution)
                              .arg(encoder)
                              .arg(encoderTypes.join(QStringLiteral(", "))));
}

void ControlWindow::rollbackCaptureBackendSelection()
{
    if (!m_captureActionGroup || m_confirmedCaptureBackend.isEmpty())
        return;

    if (m_pendingCaptureBackend.isEmpty() || m_pendingCaptureBackend == m_confirmedCaptureBackend)
        return;

    for (QAction *action : m_captureActionGroup->actions())
    {
        if (action->data().toString() == m_confirmedCaptureBackend)
        {
            QSignalBlocker blocker(m_captureActionGroup);
            action->setChecked(true);
            break;
        }
    }
    m_pendingCaptureBackend.clear();
    LOG_WARN("Capture backend change timed out, rollback to confirmed backend: {}", m_confirmedCaptureBackend);
}

void ControlWindow::onRuntimeDiagnosticsUpdated(const QString &diagnostics)
{
    m_runtimeDiagnostics = diagnostics;
}

void ControlWindow::onDiagnosticsClicked()
{
    const QString resolution = m_remoteResolution.isValid()
                                   ? QString("%1x%2").arg(m_remoteResolution.width()).arg(m_remoteResolution.height())
                                   : QStringLiteral("--");
    QString text;
    text += tr("Local receive") + QLatin1Char('\n');
    text += tr("fps=%1 kbps=%2 resolution=%3")
                .arg(m_currentFps, 0, 'f', 1)
                .arg(m_currentKbps, 0, 'f', 0)
                .arg(resolution) +
            QLatin1Char('\n');
    text += tr("stream=%1 bitrate=%2 network=%3 audio=%4")
                .arg(m_streamMode, m_bitrateProfile, m_networkPath, m_audioMode) +
            QLatin1Char('\n');
    text += tr("remoteOs=%1 desktopLocked=%2 androidNav=%3")
                .arg(m_remoteOsName.isEmpty() ? QStringLiteral("--") : m_remoteOsName)
                .arg(m_remoteDesktopLocked)
                .arg(m_androidNavigationVisible) +
            QLatin1Char('\n');
    text += tr("encoder=%1 type=%2 zeroCopy=%3 capture=%4 pendingCapture=%5")
                .arg(m_remoteEncoderName.isEmpty() ? QStringLiteral("--") : m_remoteEncoderName)
                .arg(m_remoteEncoderType.isEmpty() ? QStringLiteral("--") : m_remoteEncoderType)
                .arg(m_remoteEncoderZeroCopy)
                .arg(m_confirmedCaptureBackend.isEmpty() ? QStringLiteral("--") : m_confirmedCaptureBackend)
                .arg(m_pendingCaptureBackend.isEmpty() ? QStringLiteral("--") : m_pendingCaptureBackend) +
            QStringLiteral("\n\n");
    text += tr("Receiver feedback") + QLatin1Char('\n');
    text += m_runtimeDiagnostics.isEmpty() ? tr("No runtime feedback has been received yet.") : m_runtimeDiagnostics;

    auto *box = new QMessageBox(this);
    box->setWindowTitle(tr("运行时诊断"));
    box->setTextFormat(Qt::PlainText);
    box->setText(text);
    box->setStandardButtons(QMessageBox::Ok);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
}
