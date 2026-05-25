#ifndef AUDIO_ECHO_SUPPRESSION_H
#define AUDIO_ECHO_SUPPRESSION_H

#include <QDateTime>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

namespace AudioEchoSuppression
{
inline std::atomic<qint64> g_lastPlaybackMs{0};
inline std::atomic<int> g_playbackLevelMilli{0};

inline void notePlaybackPcm16(const std::vector<uint8_t> &pcm)
{
    if (pcm.empty())
        return;

    const size_t sampleCount = pcm.size() / sizeof(int16_t);
    if (sampleCount == 0)
        return;

    double sumSquares = 0.0;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        int16_t value = 0;
        memcpy(&value, pcm.data() + i * sizeof(value), sizeof(value));
        const double sample = static_cast<double>(value) / 32768.0;
        sumSquares += sample * sample;
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
    g_playbackLevelMilli.store(static_cast<int>(std::min(1.0, rms) * 1000.0), std::memory_order_relaxed);
    g_lastPlaybackMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
}

inline void clearPlayback()
{
    g_playbackLevelMilli.store(0, std::memory_order_relaxed);
    g_lastPlaybackMs.store(0, std::memory_order_relaxed);
}

inline float microphoneGain()
{
    const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - g_lastPlaybackMs.load(std::memory_order_relaxed);
    if (ageMs < 0 || ageMs > 250)
        return 1.0f;

    const float level = static_cast<float>(g_playbackLevelMilli.load(std::memory_order_relaxed)) / 1000.0f;
    if (level < 0.015f)
        return 1.0f;

    const float normalized = std::min(1.0f, level / 0.18f);
    return 1.0f - normalized * 0.78f;
}
} /* namespace AudioEchoSuppression */

#endif /* AUDIO_ECHO_SUPPRESSION_H */
