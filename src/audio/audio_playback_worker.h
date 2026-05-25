#ifndef AUDIO_PLAYBACK_WORKER_H
#define AUDIO_PLAYBACK_WORKER_H

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <atomic>
#include <memory>

#include "rtc/rtc.hpp"

class AudioPlaybackWorker : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlaybackWorker(QObject *parent = nullptr);
    ~AudioPlaybackWorker() override;

    // 线程安全：仅写原子标志，可从任意线程直接调用（无需排队）
    void forceStop();

public slots:
    void start();
    void stop();
    void playOpusFrame(std::shared_ptr<rtc::binary> encodedData, quint64 timestampUs);

private:
    struct Impl;
    bool initialize();
    void cleanup();
    QMutex m_mutex;
    std::unique_ptr<Impl> m_impl;
    bool m_started{false};
    std::atomic<bool> m_stopping{false};
};

#endif /* AUDIO_PLAYBACK_WORKER_H */
