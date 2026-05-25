#include "audio_playback_worker.h"

#include "audio_echo_suppression.h"
#include "common/logger_manager.h"
#include "util/config_util.h"
#include "util/ffmpeg_util.h"

#include <QDateTime>
#include <QThread>
#include <algorithm>
#include <vector>

extern "C"
{
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <mmsystem.h>
#elif defined(Q_OS_LINUX)
#include <alsa/asoundlib.h>
#endif

namespace
{
constexpr int kPlaybackSampleRate = 48000;
constexpr int kPlaybackChannels = 2;
constexpr int kPlaybackBits = 16;
constexpr int kPlaybackBufferCount = 8;
constexpr const char *kAudioDeviceNoneValue = "__none__";

QString ffmpegError(int err)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, errbuf, sizeof(errbuf));
    return QString::fromUtf8(errbuf);
}

#if AIRAN_FFMPEG_HAS_CH_LAYOUT
using AudioChannelLayout = AVChannelLayout;

void initDefaultLayout(AudioChannelLayout &layout, int channels)
{
    av_channel_layout_default(&layout, channels);
}

void uninitLayout(AudioChannelLayout &layout)
{
    av_channel_layout_uninit(&layout);
}

int allocPlaybackResampler(SwrContext **swr, const AudioChannelLayout &outputLayout, const AVFrame *frame)
{
    if (!frame)
        return AVERROR(EINVAL);
    return swr_alloc_set_opts2(swr,
                               &outputLayout, AV_SAMPLE_FMT_S16, kPlaybackSampleRate,
                               &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate,
                               0, nullptr);
}
#else
using AudioChannelLayout = uint64_t;

void initDefaultLayout(AudioChannelLayout &layout, int channels)
{
    layout = channels > 0 ? static_cast<AudioChannelLayout>(av_get_default_channel_layout(channels)) : 0;
}

void uninitLayout(AudioChannelLayout &layout)
{
    layout = 0;
}

AudioChannelLayout frameLayout(const AVFrame *frame)
{
    if (!frame)
        return 0;
    if (frame->channel_layout != 0)
        return frame->channel_layout;
    return frame->channels > 0 ? static_cast<AudioChannelLayout>(av_get_default_channel_layout(frame->channels)) : 0;
}

int allocPlaybackResampler(SwrContext **swr, const AudioChannelLayout &outputLayout, const AVFrame *frame)
{
    if (!swr || !frame || outputLayout == 0)
        return AVERROR(EINVAL);
    const AudioChannelLayout inputLayout = frameLayout(frame);
    if (inputLayout == 0)
        return AVERROR(EINVAL);
    *swr = swr_alloc_set_opts(nullptr,
                              static_cast<int64_t>(outputLayout), AV_SAMPLE_FMT_S16, kPlaybackSampleRate,
                              static_cast<int64_t>(inputLayout), static_cast<AVSampleFormat>(frame->format), frame->sample_rate,
                              0, nullptr);
    return *swr ? 0 : AVERROR(ENOMEM);
}
#endif

#if defined(Q_OS_WIN)
struct WaveBuffer
{
    WAVEHDR header{};
    std::vector<char> data;
};

UINT resolveWaveOutDeviceId(const QString &configuredOutput)
{
    const QString hint = configuredOutput.trimmed();
    if (hint.isEmpty())
        return WAVE_MAPPER;

    const QString hintLower = hint.toLower();
    const UINT deviceCount = waveOutGetNumDevs();
    int bestScore = -1;
    UINT bestId = WAVE_MAPPER;
    for (UINT i = 0; i < deviceCount; ++i)
    {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
            continue;

        const QString name = QString::fromWCharArray(caps.szPname).trimmed();
        if (name.isEmpty())
            continue;

        int score = -1;
        if (name.compare(hint, Qt::CaseInsensitive) == 0)
            score = 3;
        else if (name.toLower().contains(hintLower))
            score = 2;

        if (score > bestScore)
        {
            bestScore = score;
            bestId = i;
        }
    }

    return bestScore >= 2 ? bestId : WAVE_MAPPER;
}

void waitForWaveHeader(HWAVEOUT waveOut, WAVEHDR &header, const std::atomic<bool> &stopping)
{
    while ((header.dwFlags & WHDR_DONE) == 0 && !stopping.load())
    {
        waveOutUnprepareHeader(waveOut, &header, sizeof(header));
        if ((header.dwFlags & WHDR_PREPARED) == 0)
            break;
        QThread::msleep(2);
    }
}
#elif defined(Q_OS_LINUX)
bool openAlsaPlayback(snd_pcm_t **pcm)
{
    if (!pcm)
        return false;
    int err = snd_pcm_open(pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        LOG_ERROR("ALSA playback open failed: {}", snd_strerror(err));
        return false;
    }

    err = snd_pcm_set_params(*pcm,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             kPlaybackChannels,
                             kPlaybackSampleRate,
                             1,
                             20000);
    if (err < 0)
    {
        LOG_ERROR("ALSA playback set params failed: {}", snd_strerror(err));
        snd_pcm_close(*pcm);
        *pcm = nullptr;
        return false;
    }
    return true;
}

void writeAlsaPcm(snd_pcm_t *pcm, const std::vector<uint8_t> &pcmBytes)
{
    if (!pcm || pcmBytes.empty())
        return;

    const int bytesPerFrame = kPlaybackChannels * (kPlaybackBits / 8);
    const char *cursor = reinterpret_cast<const char *>(pcmBytes.data());
    snd_pcm_sframes_t framesLeft = static_cast<snd_pcm_sframes_t>(pcmBytes.size() / static_cast<size_t>(bytesPerFrame));
    while (framesLeft > 0)
    {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, cursor, framesLeft);
        if (written == -EPIPE)
        {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (written < 0)
        {
            written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
            if (written < 0)
            {
                LOG_WARN("ALSA playback write failed: {}", snd_strerror(static_cast<int>(written)));
                return;
            }
            continue;
        }
        cursor += written * bytesPerFrame;
        framesLeft -= written;
    }
}
#endif
} /* namespace */

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
struct AudioPlaybackWorker::Impl
{
    AVCodecContext *decoder{nullptr};
    SwrContext *swr{nullptr};
    AVPacket *packet{nullptr};
    AVFrame *frame{nullptr};
    AudioChannelLayout outputLayout{};
#if defined(Q_OS_WIN)
    HWAVEOUT waveOut{nullptr};
    std::vector<std::unique_ptr<WaveBuffer>> buffers;
    int nextBuffer{0};
#elif defined(Q_OS_LINUX)
    snd_pcm_t *pcm{nullptr};
#endif
};
#else
struct AudioPlaybackWorker::Impl
{
};
#endif

AudioPlaybackWorker::AudioPlaybackWorker(QObject *parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
}

AudioPlaybackWorker::~AudioPlaybackWorker()
{
    stop();
}

void AudioPlaybackWorker::start()
{
    m_stopping.store(false);
    QMutexLocker locker(&m_mutex);
    if (m_started)
        return;
    m_started = initialize();
}

void AudioPlaybackWorker::stop()
{
    /* 先原子置位，让事件队列中已积压的 playOpusFrame 调用快速返回，
     * 避免停止时因大量积压帧重新 initialize 导致长时间阻塞主线程。 */
    m_stopping.store(true);
    QMutexLocker locker(&m_mutex);
    cleanup();
    m_started = false;
    AudioEchoSuppression::clearPlayback();
}

void AudioPlaybackWorker::forceStop()
{
    /* 可从任意线程直接调用（仅写原子变量，无锁）。
     * 效果：队列中所有积压的 playOpusFrame 在下次检查时立即返回，
     *       waitForWaveHeader 自旋循环也将在下一次迭代中退出。 */
    m_stopping.store(true);
}

bool AudioPlaybackWorker::initialize()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    const QString configuredOutput = ConfigUtil->audio_loopback_device.trimmed();
    if (configuredOutput.compare(QString::fromLatin1(kAudioDeviceNoneValue), Qt::CaseInsensitive) == 0)
    {
        LOG_INFO("Audio playback disabled by config: output=none");
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec)
    {
        LOG_ERROR("Opus decoder not found");
        return false;
    }

    m_impl->decoder = avcodec_alloc_context3(codec);
    if (!m_impl->decoder)
    {
        LOG_ERROR("Could not allocate Opus decoder context");
        return false;
    }

    int ret = avcodec_open2(m_impl->decoder, codec, nullptr);
    if (ret < 0)
    {
        LOG_ERROR("Could not open Opus decoder: {}", ffmpegError(ret));
        cleanup();
        return false;
    }

    m_impl->packet = av_packet_alloc();
    m_impl->frame = av_frame_alloc();
    if (!m_impl->packet || !m_impl->frame)
    {
        LOG_ERROR("Could not allocate Opus playback packet/frame");
        cleanup();
        return false;
    }
    initDefaultLayout(m_impl->outputLayout, kPlaybackChannels);

#if defined(Q_OS_WIN)
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kPlaybackChannels;
    format.nSamplesPerSec = kPlaybackSampleRate;
    format.wBitsPerSample = kPlaybackBits;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const UINT preferredDeviceId = resolveWaveOutDeviceId(configuredOutput);
    MMRESULT mm = waveOutOpen(&m_impl->waveOut, preferredDeviceId, &format, 0, 0, CALLBACK_NULL);
    if (mm != MMSYSERR_NOERROR && preferredDeviceId != WAVE_MAPPER)
    {
        LOG_WARN("waveOutOpen preferred device failed, fallback to default mapper: {}", static_cast<int>(mm));
        mm = waveOutOpen(&m_impl->waveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    }
    if (mm != MMSYSERR_NOERROR)
    {
        LOG_ERROR("waveOutOpen failed: {}", static_cast<int>(mm));
        cleanup();
        return false;
    }

    if (preferredDeviceId != WAVE_MAPPER)
        LOG_INFO("Audio playback using configured waveOut device id={}", static_cast<int>(preferredDeviceId));
    else
        LOG_INFO("Audio playback using default waveOut mapper");

    for (int i = 0; i < kPlaybackBufferCount; ++i)
    {
        auto buffer = std::make_unique<WaveBuffer>();
        buffer->header.dwFlags = WHDR_DONE;
        m_impl->buffers.push_back(std::move(buffer));
    }
#elif defined(Q_OS_LINUX)
    if (!openAlsaPlayback(&m_impl->pcm))
    {
        cleanup();
        return false;
    }
#endif

    LOG_INFO("Audio playback initialized");
    return true;
#else
    LOG_WARN("Audio playback is not implemented on this platform yet");
    return false;
#endif
}

void AudioPlaybackWorker::cleanup()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
#if defined(Q_OS_WIN)
    if (m_impl->waveOut)
    {
        waveOutReset(m_impl->waveOut);
        for (auto &buffer : m_impl->buffers)
        {
            if (buffer && (buffer->header.dwFlags & WHDR_PREPARED) != 0)
                waveOutUnprepareHeader(m_impl->waveOut, &buffer->header, sizeof(buffer->header));
        }
        waveOutClose(m_impl->waveOut);
        m_impl->waveOut = nullptr;
    }
    m_impl->buffers.clear();
#elif defined(Q_OS_LINUX)
    if (m_impl->pcm)
    {
        snd_pcm_drop(m_impl->pcm); // 立即丢弃缓冲音频，非阻塞关闭
        snd_pcm_close(m_impl->pcm);
        m_impl->pcm = nullptr;
    }
#endif
    if (m_impl->swr)
        swr_free(&m_impl->swr);
    if (m_impl->packet)
        av_packet_free(&m_impl->packet);
    if (m_impl->frame)
        av_frame_free(&m_impl->frame);
    if (m_impl->decoder)
        avcodec_free_context(&m_impl->decoder);
    uninitLayout(m_impl->outputLayout);
#endif
}

void AudioPlaybackWorker::playOpusFrame(std::shared_ptr<rtc::binary> encodedData, quint64 timestampUs)
{
    Q_UNUSED(timestampUs);
    /* 若 stop() 已被调用，丢弃所有积压帧，避免卡住调用线程。 */
    if (m_stopping.load())
        return;
    if (!encodedData || encodedData->empty())
        return;

    QMutexLocker locker(&m_mutex);
    if (!m_started && !initialize())
        return;
    m_started = true;

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    int ret = av_new_packet(m_impl->packet, static_cast<int>(encodedData->size()));
    if (ret < 0)
    {
        LOG_WARN("Could not allocate Opus packet: {}", ffmpegError(ret));
        return;
    }
    memcpy(m_impl->packet->data, encodedData->data(), encodedData->size());

    ret = avcodec_send_packet(m_impl->decoder, m_impl->packet);
    av_packet_unref(m_impl->packet);
    if (ret < 0)
    {
        LOG_WARN("Could not send Opus packet to decoder: {}", ffmpegError(ret));
        return;
    }

    while (true)
    {
        ret = avcodec_receive_frame(m_impl->decoder, m_impl->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
        {
            LOG_WARN("Could not receive decoded Opus frame: {}", ffmpegError(ret));
            break;
        }

        if (!m_impl->swr)
        {
            ret = allocPlaybackResampler(&m_impl->swr, m_impl->outputLayout, m_impl->frame);
            if (ret < 0 || !m_impl->swr || swr_init(m_impl->swr) < 0)
            {
                LOG_WARN("Could not initialize playback resampler: {}", ffmpegError(ret));
                av_frame_unref(m_impl->frame);
                break;
            }
        }

        const int maxSamples = swr_get_out_samples(m_impl->swr, m_impl->frame->nb_samples);
        const int outBytes = av_samples_get_buffer_size(nullptr, kPlaybackChannels, maxSamples, AV_SAMPLE_FMT_S16, 0);
        if (outBytes <= 0)
        {
            av_frame_unref(m_impl->frame);
            continue;
        }

        std::vector<uint8_t> pcm(static_cast<size_t>(outBytes));
        uint8_t *outData[1] = {pcm.data()};
        const int converted = swr_convert(m_impl->swr, outData, maxSamples,
                                          const_cast<const uint8_t **>(m_impl->frame->extended_data), m_impl->frame->nb_samples);
        av_frame_unref(m_impl->frame);
        if (converted <= 0)
            continue;

        const int actualBytes = av_samples_get_buffer_size(nullptr, kPlaybackChannels, converted, AV_SAMPLE_FMT_S16, 0);
        if (actualBytes <= 0)
            continue;
        pcm.resize(static_cast<size_t>(actualBytes));
        AudioEchoSuppression::notePlaybackPcm16(pcm);

#if defined(Q_OS_WIN)
        if (m_impl->buffers.empty() || !m_impl->waveOut)
            continue;

        WaveBuffer *buffer = m_impl->buffers[static_cast<size_t>(m_impl->nextBuffer)].get();
        m_impl->nextBuffer = (m_impl->nextBuffer + 1) % static_cast<int>(m_impl->buffers.size());
        if ((buffer->header.dwFlags & WHDR_PREPARED) != 0)
            waitForWaveHeader(m_impl->waveOut, buffer->header, m_stopping);
        if ((buffer->header.dwFlags & WHDR_PREPARED) != 0)
            waveOutUnprepareHeader(m_impl->waveOut, &buffer->header, sizeof(buffer->header));

        buffer->data.assign(reinterpret_cast<const char *>(pcm.data()), reinterpret_cast<const char *>(pcm.data() + pcm.size()));
        memset(&buffer->header, 0, sizeof(buffer->header));
        buffer->header.lpData = buffer->data.data();
        buffer->header.dwBufferLength = static_cast<DWORD>(buffer->data.size());
        MMRESULT mm = waveOutPrepareHeader(m_impl->waveOut, &buffer->header, sizeof(buffer->header));
        if (mm != MMSYSERR_NOERROR)
        {
            LOG_WARN("waveOutPrepareHeader failed: {}", static_cast<int>(mm));
            continue;
        }
        mm = waveOutWrite(m_impl->waveOut, &buffer->header, sizeof(buffer->header));
        if (mm != MMSYSERR_NOERROR)
        {
            LOG_WARN("waveOutWrite failed: {}", static_cast<int>(mm));
            waveOutUnprepareHeader(m_impl->waveOut, &buffer->header, sizeof(buffer->header));
        }
#elif defined(Q_OS_LINUX)
        writeAlsaPcm(m_impl->pcm, pcm);
#endif
    }
#endif
}
