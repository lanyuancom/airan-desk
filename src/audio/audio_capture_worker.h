#ifndef AUDIO_CAPTURE_WORKER_H
#define AUDIO_CAPTURE_WORKER_H

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QString>
#include <atomic>
#include <memory>

#include "rtc/rtc.hpp"

class AudioCaptureWorker : public QObject
{
    Q_OBJECT
public:
    enum class CaptureSource
    {
        SystemLoopback,
        Microphone,
        MicrophoneAndSystem
    };

    explicit AudioCaptureWorker(CaptureSource source = CaptureSource::SystemLoopback,
                                const QString &preferredDevice = QString(),
                                QObject *parent = nullptr);
    ~AudioCaptureWorker() override;

public slots:
    void start();
    void stop();

signals:
    void opusFrameReady(std::shared_ptr<rtc::binary> encodedData, quint64 timestampUs);
    void inputLevelUpdated(float normalizedLevel);
    void stopped();

private:
    void runCaptureLoop();

    CaptureSource m_source{CaptureSource::SystemLoopback};
    QString m_preferredDevice;
    std::atomic_bool m_stopRequested{false};
    bool m_running{false};
};

#endif /* AUDIO_CAPTURE_WORKER_H */
