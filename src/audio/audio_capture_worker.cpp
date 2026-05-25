#include "audio_capture_worker.h"

#include "audio_echo_suppression.h"
#include "common/logger_manager.h"
#include "util/config_util.h"
#include "util/ffmpeg_util.h"

#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C"
{
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#if defined(Q_OS_WIN)
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <comdef.h>
#include <propidl.h>
#endif

namespace
{
constexpr int kOpusSampleRate = 48000;
constexpr int kOpusChannels = 2;
constexpr int kOpusFrameMs = 20;
constexpr int kOpusFrameSamples = kOpusSampleRate * kOpusFrameMs / 1000;
constexpr int kOpusBitrate = 48000;
constexpr const char *kAudioDeviceNoneValue = "__none__";

bool isDeviceDisabled(const QString &device)
{
    return device.trimmed().compare(QString::fromLatin1(kAudioDeviceNoneValue), Qt::CaseInsensitive) == 0;
}

QString ffmpegError(int err)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, errbuf, sizeof(errbuf));
    return QString::fromUtf8(errbuf);
}

AVSampleFormat chooseEncoderSampleFormat(const AVCodec *codec)
{
#if AIRAN_FFMPEG_HAS_SUPPORTED_CONFIG
    const AVSampleFormat *sampleFormats = nullptr;
    int sampleFormatCount = 0;
    if (codec && avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                              0, reinterpret_cast<const void **>(&sampleFormats), &sampleFormatCount) >= 0 &&
        sampleFormats)
    {
        for (int i = 0; i < sampleFormatCount && sampleFormats[i] != AV_SAMPLE_FMT_NONE; ++i)
        {
            if (sampleFormats[i] == AV_SAMPLE_FMT_S16)
                return AV_SAMPLE_FMT_S16;
        }
        for (int i = 0; i < sampleFormatCount && sampleFormats[i] != AV_SAMPLE_FMT_NONE; ++i)
        {
            if (sampleFormats[i] == AV_SAMPLE_FMT_FLT)
                return AV_SAMPLE_FMT_FLT;
        }
        return sampleFormats[0];
    }
#else
    if (codec && codec->sample_fmts)
    {
        for (const AVSampleFormat *fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; ++fmt)
        {
            if (*fmt == AV_SAMPLE_FMT_S16)
                return AV_SAMPLE_FMT_S16;
        }
        for (const AVSampleFormat *fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; ++fmt)
        {
            if (*fmt == AV_SAMPLE_FMT_FLT)
                return AV_SAMPLE_FMT_FLT;
        }
        return codec->sample_fmts[0];
    }
#endif
    return AV_SAMPLE_FMT_S16;
}

bool copyPacketToBinary(AVPacket *packet, std::shared_ptr<rtc::binary> &out)
{
    if (!packet || packet->size <= 0)
        return false;

    out = std::make_shared<rtc::binary>(static_cast<size_t>(packet->size));
    memcpy(out->data(), packet->data, static_cast<size_t>(packet->size));
    return true;
}

const char *captureSourceName(AudioCaptureWorker::CaptureSource source)
{
    switch (source)
    {
    case AudioCaptureWorker::CaptureSource::Microphone:
        return "microphone";
    case AudioCaptureWorker::CaptureSource::MicrophoneAndSystem:
        return "microphone+loopback";
    case AudioCaptureWorker::CaptureSource::SystemLoopback:
    default:
        return "loopback";
    }
}

std::vector<AudioCaptureWorker::CaptureSource> expandCaptureSources(AudioCaptureWorker::CaptureSource source)
{
    if (source == AudioCaptureWorker::CaptureSource::MicrophoneAndSystem)
        return {AudioCaptureWorker::CaptureSource::Microphone, AudioCaptureWorker::CaptureSource::SystemLoopback};
    return {source};
}

void appendConvertedAudio(SwrContext *swr, const std::vector<uint8_t> &captureBytes, int capturedFrames,
                          AVSampleFormat outputFormat, std::vector<uint8_t> &pcmBuffer)
{
    if (!swr || captureBytes.empty() || capturedFrames <= 0)
        return;

    const uint8_t *inputData[1] = {captureBytes.data()};
    const int maxOutSamples = swr_get_out_samples(swr, capturedFrames);
    const int bytesPerSample = av_get_bytes_per_sample(outputFormat);
    const int outLineBytes = maxOutSamples * kOpusChannels * bytesPerSample;
    const size_t oldSize = pcmBuffer.size();
    pcmBuffer.resize(oldSize + static_cast<size_t>(qMax(0, outLineBytes)));
    uint8_t *outData[1] = {pcmBuffer.data() + oldSize};
    const int convertedSamples = swr_convert(swr, outData, maxOutSamples, inputData, capturedFrames);
    if (convertedSamples < 0)
    {
        LOG_WARN("Audio resample failed: {}", ffmpegError(convertedSamples));
        pcmBuffer.resize(oldSize);
        return;
    }

    const size_t producedBytes = static_cast<size_t>(convertedSamples) * kOpusChannels * static_cast<size_t>(bytesPerSample);
    pcmBuffer.resize(oldSize + producedBytes);
}

float readPackedSampleAsFloat(const uint8_t *bytes, AVSampleFormat format)
{
    switch (format)
    {
    case AV_SAMPLE_FMT_S16:
    {
        int16_t value = 0;
        memcpy(&value, bytes, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }
    case AV_SAMPLE_FMT_S32:
    {
        int32_t value = 0;
        memcpy(&value, bytes, sizeof(value));
        return static_cast<float>(static_cast<double>(value) / 2147483648.0);
    }
    case AV_SAMPLE_FMT_FLT:
    {
        float value = 0.0f;
        memcpy(&value, bytes, sizeof(value));
        return value;
    }
    default:
        return 0.0f;
    }
}

void writeFloatAsPackedSample(float value, uint8_t *bytes, AVSampleFormat format)
{
    value = qMax(-1.0f, qMin(1.0f, value));
    switch (format)
    {
    case AV_SAMPLE_FMT_S16:
    {
        const int16_t out = static_cast<int16_t>(std::lrint(value * 32767.0f));
        memcpy(bytes, &out, sizeof(out));
        break;
    }
    case AV_SAMPLE_FMT_S32:
    {
        const int32_t out = static_cast<int32_t>(std::llrint(static_cast<double>(value) * 2147483647.0));
        memcpy(bytes, &out, sizeof(out));
        break;
    }
    case AV_SAMPLE_FMT_FLT:
        memcpy(bytes, &value, sizeof(value));
        break;
    default:
        break;
    }
}

void applyGainToPackedFrame(uint8_t *bytes, size_t byteCount, AVSampleFormat format, float gain)
{
    if (!bytes || byteCount == 0 || gain >= 0.995f)
        return;

    const int bytesPerSample = av_get_bytes_per_sample(format);
    if (bytesPerSample <= 0)
        return;

    const size_t sampleCount = byteCount / static_cast<size_t>(bytesPerSample);
    for (size_t i = 0; i < sampleCount; ++i)
    {
        uint8_t *sample = bytes + i * static_cast<size_t>(bytesPerSample);
        writeFloatAsPackedSample(readPackedSampleAsFloat(sample, format) * gain, sample, format);
    }
}

// 计算一帧 PCM 数据的 RMS 能量，用于 VAD 门控
float computeFrameRms(const uint8_t *data, size_t byteCount, AVSampleFormat format)
{
    const int bytesPerSample = av_get_bytes_per_sample(format);
    if (!data || bytesPerSample <= 0 || byteCount == 0)
        return 0.0f;

    const size_t sampleCount = byteCount / static_cast<size_t>(bytesPerSample);
    float sumSq = 0.0f;
    for (size_t i = 0; i < sampleCount; ++i)
    {
        const float s = readPackedSampleAsFloat(data + i * static_cast<size_t>(bytesPerSample), format);
        sumSq += s * s;
    }
    return std::sqrt(sumSq / static_cast<float>(sampleCount));
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

int setEncoderLayout(AVCodecContext *encoder, const AudioChannelLayout &layout)
{
    return av_channel_layout_copy(&encoder->ch_layout, &layout);
}

int setFrameLayout(AVFrame *frame, const AudioChannelLayout &layout, int channels)
{
    Q_UNUSED(channels);
    return av_channel_layout_copy(&frame->ch_layout, &layout);
}

int allocResampler(SwrContext **swr, const AudioChannelLayout &outputLayout, AVSampleFormat outputFormat, int outputSampleRate,
                   const AudioChannelLayout &inputLayout, AVSampleFormat inputFormat, int inputSampleRate)
{
    return swr_alloc_set_opts2(swr, &outputLayout, outputFormat, outputSampleRate,
                               &inputLayout, inputFormat, inputSampleRate, 0, nullptr);
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

int setEncoderLayout(AVCodecContext *encoder, const AudioChannelLayout &layout)
{
    if (!encoder || layout == 0)
        return AVERROR(EINVAL);
    encoder->channel_layout = layout;
    encoder->channels = kOpusChannels;
    return 0;
}

int setFrameLayout(AVFrame *frame, const AudioChannelLayout &layout, int channels)
{
    if (!frame || layout == 0)
        return AVERROR(EINVAL);
    frame->channel_layout = layout;
    frame->channels = channels;
    return 0;
}

int allocResampler(SwrContext **swr, const AudioChannelLayout &outputLayout, AVSampleFormat outputFormat, int outputSampleRate,
                   const AudioChannelLayout &inputLayout, AVSampleFormat inputFormat, int inputSampleRate)
{
    if (!swr || outputLayout == 0 || inputLayout == 0)
        return AVERROR(EINVAL);
    *swr = swr_alloc_set_opts(nullptr,
                              static_cast<int64_t>(outputLayout), outputFormat, outputSampleRate,
                              static_cast<int64_t>(inputLayout), inputFormat, inputSampleRate,
                              0, nullptr);
    return *swr ? 0 : AVERROR(ENOMEM);
}
#endif

#if defined(Q_OS_WIN)
struct ComGuard
{
    bool initialized{false};

    ComGuard()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }

    ~ComGuard()
    {
        if (initialized)
            CoUninitialize();
    }
};

template <typename T>
void releaseCom(T *&ptr)
{
    if (ptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

AVSampleFormat sampleFormatFromWaveFormat(const WAVEFORMATEX *format)
{
    if (!format)
        return AV_SAMPLE_FMT_NONE;

    const bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                          reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    if (isFloat && format->wBitsPerSample == 32)
        return AV_SAMPLE_FMT_FLT;
    if (format->wBitsPerSample == 16)
        return AV_SAMPLE_FMT_S16;
    if (format->wBitsPerSample == 32)
        return AV_SAMPLE_FMT_S32;
    return AV_SAMPLE_FMT_NONE;
}

class WasapiLoopbackCapture
{
public:
    ~WasapiLoopbackCapture()
    {
        cleanup();
    }

    bool initialize(AudioCaptureWorker::CaptureSource source, const QString &preferredDevice)
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&m_enumerator));
        if (FAILED(hr))
        {
            LOG_ERROR("Audio capture CoCreateInstance failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }

        const QString configuredDevice = preferredDevice.isEmpty() ? configuredDeviceForSource(source) : preferredDevice;
        if (isDeviceDisabled(configuredDevice))
        {
            LOG_INFO("WASAPI audio capture disabled by config: source={}",
                     source == AudioCaptureWorker::CaptureSource::Microphone ? "microphone" : "loopback");
            return false;
        }
        const EDataFlow dataFlow = source == AudioCaptureWorker::CaptureSource::Microphone ? eCapture : eRender;
        if (!tryOpenConfiguredOrDefaultDevice(dataFlow, configuredDevice))
        {
            LOG_ERROR("Audio capture could not open endpoint: source={}, configuredDevice={}",
                      source == AudioCaptureWorker::CaptureSource::Microphone ? "microphone" : "loopback",
                      configuredDevice.toStdString());
            return false;
        }

        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&m_audioClient));
        if (FAILED(hr))
        {
            LOG_ERROR("Audio capture Activate IAudioClient failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }

        hr = m_audioClient->GetMixFormat(&m_mixFormat);
        if (FAILED(hr) || !m_mixFormat)
        {
            LOG_ERROR("Audio capture GetMixFormat failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }

        REFERENCE_TIME bufferDuration = 10000000;
        const DWORD streamFlags = source == AudioCaptureWorker::CaptureSource::Microphone ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
        hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                       bufferDuration, 0, m_mixFormat, nullptr);
        if (FAILED(hr))
        {
            LOG_ERROR("Audio capture Initialize loopback failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }

        hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&m_captureClient));
        if (FAILED(hr))
        {
            LOG_ERROR("Audio capture GetService failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }

        hr = m_audioClient->Start();
        if (FAILED(hr))
        {
            LOG_ERROR("Audio capture Start failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }

        m_started = true;
        LOG_INFO("WASAPI audio capture initialized: source={}, sampleRate={}, channels={}, bits={}",
                 source == AudioCaptureWorker::CaptureSource::Microphone ? "microphone" : "loopback",
                 m_mixFormat->nSamplesPerSec, m_mixFormat->nChannels, m_mixFormat->wBitsPerSample);
        return true;
    }

    void cleanup()
    {
        if (m_audioClient && m_started)
            m_audioClient->Stop();
        m_started = false;
        if (m_mixFormat)
        {
            CoTaskMemFree(m_mixFormat);
            m_mixFormat = nullptr;
        }
        releaseCom(m_captureClient);
        releaseCom(m_audioClient);
        releaseCom(m_device);
        releaseCom(m_enumerator);
    }

    bool readPacket(std::vector<uint8_t> &out, int &frames, bool &silent)
    {
        out.clear();
        frames = 0;
        silent = false;

        UINT32 packetFrames = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetFrames);
        if (FAILED(hr))
            return false;
        if (packetFrames == 0)
            return true;

        BYTE *data = nullptr;
        DWORD flags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;
        hr = m_captureClient->GetBuffer(&data, &packetFrames, &flags, &devicePosition, &qpcPosition);
        if (FAILED(hr))
            return false;

        const int bytesPerFrame = m_mixFormat->nBlockAlign;
        const size_t byteCount = static_cast<size_t>(packetFrames) * static_cast<size_t>(bytesPerFrame);
        out.resize(byteCount);
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data)
        {
            std::fill(out.begin(), out.end(), 0);
            silent = true;
        }
        else
        {
            memcpy(out.data(), data, byteCount);
        }
        frames = static_cast<int>(packetFrames);
        m_captureClient->ReleaseBuffer(packetFrames);
        return true;
    }

    int sampleRate() const
    {
        return m_mixFormat ? static_cast<int>(m_mixFormat->nSamplesPerSec) : 0;
    }

    int channels() const
    {
        return m_mixFormat ? static_cast<int>(m_mixFormat->nChannels) : 0;
    }

    AVSampleFormat sampleFormat() const
    {
        return sampleFormatFromWaveFormat(m_mixFormat);
    }

private:
    QString configuredDeviceForSource(AudioCaptureWorker::CaptureSource source) const
    {
        const QString envName = source == AudioCaptureWorker::CaptureSource::Microphone
                                    ? QStringLiteral("AIRAN_DESK_AUDIO_MIC_DEVICE")
                                    : QStringLiteral("AIRAN_DESK_AUDIO_LOOPBACK_DEVICE");
        const QString iniConfiguredDevice = source == AudioCaptureWorker::CaptureSource::Microphone
                                                ? ConfigUtil->audio_mic_device
                                                : ConfigUtil->audio_loopback_device;
        const QString envConfiguredDevice = QProcessEnvironment::systemEnvironment().value(envName).trimmed();
        return !iniConfiguredDevice.trimmed().isEmpty() ? iniConfiguredDevice.trimmed() : envConfiguredDevice;
    }

    QString deviceFriendlyName(IMMDevice *device) const
    {
        if (!device)
            return QString();

        IPropertyStore *store = nullptr;
        HRESULT hr = device->OpenPropertyStore(STGM_READ, &store);
        if (FAILED(hr) || !store)
            return QString();

        PROPVARIANT varName;
        PropVariantInit(&varName);
        QString name;
        hr = store->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR && varName.pwszVal)
            name = QString::fromWCharArray(varName.pwszVal);
        PropVariantClear(&varName);
        store->Release();
        return name;
    }

    QString deviceId(IMMDevice *device) const
    {
        if (!device)
            return QString();

        LPWSTR id = nullptr;
        HRESULT hr = device->GetId(&id);
        if (FAILED(hr) || !id)
            return QString();
        const QString result = QString::fromWCharArray(id);
        CoTaskMemFree(id);
        return result;
    }

    bool tryGetConfiguredDevice(EDataFlow dataFlow, const QString &configured, IMMDevice **outDevice) const
    {
        if (!outDevice)
            return false;
        *outDevice = nullptr;
        if (configured.isEmpty() || !m_enumerator)
            return false;

        // 优先按 endpoint ID 直连：UI 下拉里保存的是 WASAPI 设备 ID。
        {
            IMMDevice *directDevice = nullptr;
            const std::wstring wid = configured.toStdWString();
            if (!wid.empty() && SUCCEEDED(m_enumerator->GetDevice(wid.c_str(), &directDevice)) && directDevice)
            {
                *outDevice = directDevice;
                return true;
            }
        }

        IMMDeviceCollection *collection = nullptr;
        HRESULT hr = m_enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr) || !collection)
            return false;

        const QString hint = configured.trimmed();
        const QString hintLower = hint.toLower();
        UINT count = 0;
        collection->GetCount(&count);

        IMMDevice *bestDevice = nullptr;
        int bestScore = -1;
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice *candidate = nullptr;
            if (FAILED(collection->Item(i, &candidate)) || !candidate)
                continue;

            const QString id = deviceId(candidate);
            const QString name = deviceFriendlyName(candidate);
            const QString idLower = id.toLower();
            const QString nameLower = name.toLower();

            int score = -1;
            if (!id.isEmpty() && id.compare(hint, Qt::CaseInsensitive) == 0)
                score = 4;
            else if (!name.isEmpty() && name.compare(hint, Qt::CaseInsensitive) == 0)
                score = 3;
            else if ((!idLower.isEmpty() && idLower.contains(hintLower)) ||
                     (!nameLower.isEmpty() && nameLower.contains(hintLower)))
                score = 2;

            if (score > bestScore)
            {
                if (bestDevice)
                    bestDevice->Release();
                bestDevice = candidate;
                bestScore = score;
            }
            else
            {
                candidate->Release();
            }
        }
        collection->Release();

        if (bestDevice && bestScore >= 2)
        {
            *outDevice = bestDevice;
            return true;
        }

        if (bestDevice)
            bestDevice->Release();
        return false;
    }

    bool tryOpenConfiguredOrDefaultDevice(EDataFlow dataFlow, const QString &configured)
    {
        if (!m_enumerator)
            return false;

        IMMDevice *selectedDevice = nullptr;
        if (!configured.isEmpty())
        {
            if (tryGetConfiguredDevice(dataFlow, configured, &selectedDevice))
            {
                m_device = selectedDevice;
                LOG_INFO("WASAPI audio capture selected configured device: {}", configured.toStdString());
                return true;
            }

            LOG_WARN("WASAPI configured device not found, fallback to default: {}", configured.toStdString());
        }

        const HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &m_device);
        if (FAILED(hr))
        {
            LOG_ERROR("Audio capture GetDefaultAudioEndpoint failed: 0x{:08x}", static_cast<unsigned>(hr));
            return false;
        }
        return true;
    }

    IMMDeviceEnumerator *m_enumerator{nullptr};
    IMMDevice *m_device{nullptr};
    IAudioClient *m_audioClient{nullptr};
    IAudioCaptureClient *m_captureClient{nullptr};
    WAVEFORMATEX *m_mixFormat{nullptr};
    bool m_started{false};
};
#elif defined(Q_OS_LINUX)
class AvDeviceAudioCapture
{
public:
    ~AvDeviceAudioCapture()
    {
        cleanup();
    }

    bool initialize(AudioCaptureWorker::CaptureSource source, const QString &preferredDevice)
    {
        avdevice_register_all();
        const QString envName = source == AudioCaptureWorker::CaptureSource::Microphone
                                    ? QStringLiteral("AIRAN_DESK_AUDIO_MIC_DEVICE")
                                    : QStringLiteral("AIRAN_DESK_AUDIO_LOOPBACK_DEVICE");
        const QString iniConfiguredDevice = source == AudioCaptureWorker::CaptureSource::Microphone
                                                ? ConfigUtil->audio_mic_device
                                                : ConfigUtil->audio_loopback_device;
        const QString envConfiguredDevice = QProcessEnvironment::systemEnvironment().value(envName).trimmed();
        const QString configuredDevice = !preferredDevice.trimmed().isEmpty()
                             ? preferredDevice.trimmed()
                             : (!iniConfiguredDevice.trimmed().isEmpty()
                                ? iniConfiguredDevice.trimmed()
                                : envConfiguredDevice);
        if (isDeviceDisabled(configuredDevice))
        {
            LOG_INFO("Linux audio capture disabled by config: source={}",
                     source == AudioCaptureWorker::CaptureSource::Microphone ? "microphone" : "loopback");
            return false;
        }
        const QString selectedDevice = configuredDevice.isEmpty() ? defaultDevice(source) : configuredDevice;

        if (!selectedDevice.isEmpty())
        {
            if (openInput(QStringLiteral("pulse"), selectedDevice))
                return true;
            if (openInput(QStringLiteral("alsa"), selectedDevice))
                return true;
        }

        if (source == AudioCaptureWorker::CaptureSource::Microphone)
        {
            if (selectedDevice.isEmpty())
            {
                LOG_WARN("Linux microphone source unavailable (default source is monitor or empty)");
                return false;
            }

            // 麦克风可回退到 "default" 默认录音设备
            if (selectedDevice != QStringLiteral("default") && openInput(QStringLiteral("pulse"), QStringLiteral("default")))
                return true;
            if (selectedDevice != QStringLiteral("default") && openInput(QStringLiteral("alsa"), QStringLiteral("default")))
                return true;
        }
        else
        {
            // 回环路：额外尝试 @DEFAULT_MONITOR@ 别名
            if (selectedDevice != QStringLiteral("@DEFAULT_MONITOR@")
                && openInput(QStringLiteral("pulse"), QStringLiteral("@DEFAULT_MONITOR@")))
                return true;
            // 严禁回退到 "default"："default" 指向麦克风而非系统音回环，
            // 回退会导致监听模式变为两路麦克风混音，音质极差。
        }

        LOG_ERROR("Audio capture could not open Linux audio device");
        return false;
    }

    void setStopFlag(std::atomic<bool> *flag) { m_stopFlag = flag; }

    void cleanup()
    {
        if (m_packet)
            av_packet_free(&m_packet);
        if (m_formatContext)
            avformat_close_input(&m_formatContext);
        m_streamIndex = -1;
        m_sampleRate = 0;
        m_channels = 0;
        m_sampleFormat = AV_SAMPLE_FMT_NONE;
    }

    bool readPacket(std::vector<uint8_t> &out, int &frames, bool &silent)
    {
        out.clear();
        frames = 0;
        silent = false;
        if (!m_formatContext || !m_packet)
            return false;

        while (true)
        {
            m_readTimedOut.store(false);
            m_readDeadlineMs.store(QDateTime::currentMSecsSinceEpoch() + kReadTimeoutMs);
            const int ret = av_read_frame(m_formatContext, m_packet);
            m_readDeadlineMs.store(0);
            if (ret < 0)
            {
                // 超时中断不视为失败：让混音循环继续处理其他音源，避免卡顿。
                if (ret == AVERROR_EXIT && m_readTimedOut.load())
                    return true;
                return false;
            }
            if (m_packet->stream_index != m_streamIndex)
            {
                av_packet_unref(m_packet);
                continue;
            }

            out.resize(static_cast<size_t>(m_packet->size));
            if (m_packet->size > 0)
                memcpy(out.data(), m_packet->data, static_cast<size_t>(m_packet->size));
            frames = estimateFrames(m_packet->size);
            av_packet_unref(m_packet);
            return true;
        }
    }

    int sampleRate() const
    {
        return m_sampleRate;
    }

    int channels() const
    {
        return m_channels;
    }

    AVSampleFormat sampleFormat() const
    {
        return m_sampleFormat == AV_SAMPLE_FMT_NONE ? AV_SAMPLE_FMT_S16 : m_sampleFormat;
    }

private:
    int sourceStateScore(const QString &state) const
    {
        const QString normalized = state.trimmed().toUpper();
        if (normalized == QStringLiteral("RUNNING"))
            return 3;
        if (normalized == QStringLiteral("IDLE"))
            return 2;
        if (normalized == QStringLiteral("SUSPENDED"))
            return 1;
        return 0;
    }

    QString defaultDevice(AudioCaptureWorker::CaptureSource source) const
    {
        if (source == AudioCaptureWorker::CaptureSource::Microphone)
        {
            QProcess process;
            process.start(QStringLiteral("pactl"), QStringList() << QStringLiteral("get-default-source"));
            if (process.waitForFinished(300) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
            {
                const QString defaultSource = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
                if (!defaultSource.isEmpty())
                {
                    // 没有真实麦克风时，默认 source 可能被系统指向 *.monitor。
                    // 在 microphone+loopback 模式下这会把系统声混入两次并造成顿挫。
                    if (defaultSource.endsWith(QStringLiteral(".monitor"), Qt::CaseInsensitive))
                        return QString();
                    return defaultSource;
                }
            }

            return QStringLiteral("default");
        }

        QString preferredMonitor;
        {
            QProcess process;
            process.start(QStringLiteral("pactl"), QStringList() << QStringLiteral("get-default-sink"));
            if (process.waitForFinished(300) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
            {
                const QString sinkName = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
                if (!sinkName.isEmpty())
                    preferredMonitor = sinkName + QStringLiteral(".monitor");
            }
        }

        // 枚举 source：优先 RUNNING/IDLE 的 monitor，避免选到 SUSPENDED 的 HDMI monitor。
        {
            QProcess process;
            process.start(QStringLiteral("pactl"),
                          QStringList() << QStringLiteral("list") << QStringLiteral("short") << QStringLiteral("sources"));
            if (process.waitForFinished(500) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
            {
                const QString output = QString::fromUtf8(process.readAllStandardOutput());
                QString firstMonitor;
                QString bestMonitor;
                int bestScore = -1;
                bool preferredFound = false;
                int preferredScore = -1;

                for (const QString &line : output.split(QLatin1Char('\n')))
                {
                    // 每行格式：id<TAB>name<TAB>module<TAB>sample<TAB>state
                    const QStringList parts = line.split(QLatin1Char('\t'));
                    if (parts.size() < 2)
                        continue;
                    const QString name = parts[1].trimmed();
                    if (!name.endsWith(QStringLiteral(".monitor")))
                        continue;

                    if (firstMonitor.isEmpty())
                        firstMonitor = name;

                    const int score = sourceStateScore(parts.size() >= 5 ? parts[4] : QString());
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestMonitor = name;
                    }

                    if (!preferredMonitor.isEmpty() && name == preferredMonitor)
                    {
                        preferredFound = true;
                        preferredScore = score;
                    }
                }

                if (preferredFound && preferredScore >= 2)
                    return preferredMonitor;
                if (!bestMonitor.isEmpty() && bestScore >= 2)
                    return bestMonitor;
                if (!preferredMonitor.isEmpty())
                    return preferredMonitor;
                if (!firstMonitor.isEmpty())
                    return firstMonitor;
            }
        }

        // 兜底：PulseAudio/PipeWire 内置别名（>=PulseAudio 8.0 / 所有 PipeWire 版本）
        // 绝不回退到 "default"——那会打开麦克风而非系统音回环
        return QStringLiteral("@DEFAULT_MONITOR@");
    }

    bool openInput(const QString &formatName, const QString &deviceName)
    {
        cleanup();

        AVInputFormat *inputFormat = const_cast<AVInputFormat *>(av_find_input_format(formatName.toUtf8().constData()));
        if (!inputFormat)
            return false;

        AVDictionary *options = nullptr;
        av_dict_set(&options, "sample_rate", QByteArray::number(kOpusSampleRate).constData(), 0);
        av_dict_set(&options, "channels", QByteArray::number(kOpusChannels).constData(), 0);
        /* 20ms 帧对应的字节数：48000 * 2ch * 2bytes * 0.02s = 3840，要求 PulseAudio
         * 以 20ms 为单位交付数据，避免其默认 ~2s 的大 buffer 造成高延迟。 */
        if (formatName == QStringLiteral("pulse"))
            av_dict_set(&options, "fragment_size", "3840", 0);

        AVFormatContext *context = avformat_alloc_context();
        if (!context)
        {
            LOG_WARN("Audio capture alloc context failed");
            av_dict_free(&options);
            return false;
        }
        context->interrupt_callback.callback = &AvDeviceAudioCapture::interruptCallback;
        context->interrupt_callback.opaque = this;
        const QByteArray deviceBytes = deviceName.toUtf8();
        int ret = avformat_open_input(&context, deviceBytes.constData(), inputFormat, &options);
        av_dict_free(&options);
        if (ret < 0)
        {
            LOG_WARN("Audio capture open {}:{} failed: {}", formatName.toStdString(), deviceName.toStdString(), ffmpegError(ret));
            return false;
        }

        ret = avformat_find_stream_info(context, nullptr);
        if (ret < 0)
        {
            LOG_WARN("Audio capture stream info failed: {}", ffmpegError(ret));
            avformat_close_input(&context);
            return false;
        }

        int audioStream = -1;
        for (unsigned int i = 0; i < context->nb_streams; ++i)
        {
            const AVCodecParameters *codecpar = context->streams[i]->codecpar;
            if (codecpar && codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                audioStream = static_cast<int>(i);
                break;
            }
        }
        if (audioStream < 0)
        {
            LOG_WARN("Audio capture device has no audio stream");
            avformat_close_input(&context);
            return false;
        }

        const AVCodecParameters *codecpar = context->streams[audioStream]->codecpar;
        m_sampleRate = codecpar->sample_rate > 0 ? codecpar->sample_rate : kOpusSampleRate;
#if AIRAN_FFMPEG_HAS_CH_LAYOUT
        m_channels = codecpar->ch_layout.nb_channels > 0 ? codecpar->ch_layout.nb_channels : kOpusChannels;
#else
        m_channels = codecpar->channels > 0 ? codecpar->channels : kOpusChannels;
#endif
        m_sampleFormat = static_cast<AVSampleFormat>(codecpar->format);
        if (m_sampleFormat == AV_SAMPLE_FMT_NONE)
            m_sampleFormat = AV_SAMPLE_FMT_S16;
        m_streamIndex = audioStream;
        m_formatContext = context;
        m_packet = av_packet_alloc();
        if (!m_packet)
        {
            LOG_ERROR("Could not allocate Linux audio capture packet");
            cleanup();
            return false;
        }

        LOG_INFO("Linux audio capture initialized: input={}, device={}, sampleRate={}, channels={}, sampleFormat={}",
                 formatName.toStdString(), deviceName.toStdString(), m_sampleRate, m_channels, av_get_sample_fmt_name(m_sampleFormat));
        return true;
    }

    int estimateFrames(int bytes) const
    {
        const int bytesPerSample = av_get_bytes_per_sample(sampleFormat());
        const int bytesPerFrame = qMax(1, m_channels * qMax(1, bytesPerSample));
        return bytes > 0 ? bytes / bytesPerFrame : 0;
    }

    AVFormatContext *m_formatContext{nullptr};
    AVPacket *m_packet{nullptr};
    int m_streamIndex{-1};
    int m_sampleRate{0};
    int m_channels{0};
    AVSampleFormat m_sampleFormat{AV_SAMPLE_FMT_NONE};
    std::atomic<bool> *m_stopFlag{nullptr};
    std::atomic<qint64> m_readDeadlineMs{0};
    std::atomic<bool> m_readTimedOut{false};
    static constexpr qint64 kReadTimeoutMs = 12;

    static int interruptCallback(void *opaque)
    {
        auto *self = static_cast<AvDeviceAudioCapture *>(opaque);
        if (!self)
            return 0;

        if (self->m_stopFlag && self->m_stopFlag->load())
            return 1;

        const qint64 deadline = self->m_readDeadlineMs.load();
        if (deadline > 0 && QDateTime::currentMSecsSinceEpoch() >= deadline)
        {
            self->m_readTimedOut.store(true);
            return 1;
        }
        return 0;
    }
};
#endif

#if defined(Q_OS_WIN)
using PlatformAudioCapture = WasapiLoopbackCapture;
#elif defined(Q_OS_LINUX)
using PlatformAudioCapture = AvDeviceAudioCapture;
#else
class UnsupportedAudioCapture
{
public:
    bool initialize(AudioCaptureWorker::CaptureSource source, const QString &preferredDevice)
    {
        Q_UNUSED(source);
        Q_UNUSED(preferredDevice);
        return false;
    }

    bool readPacket(std::vector<uint8_t> &out, int &frames, bool &silent)
    {
        out.clear();
        frames = 0;
        silent = false;
        return false;
    }

    int sampleRate() const
    {
        return kOpusSampleRate;
    }

    int channels() const
    {
        return kOpusChannels;
    }

    AVSampleFormat sampleFormat() const
    {
        return AV_SAMPLE_FMT_S16;
    }
};

using PlatformAudioCapture = UnsupportedAudioCapture;
#endif
} /* namespace */

AudioCaptureWorker::AudioCaptureWorker(CaptureSource source, const QString &preferredDevice, QObject *parent)
    : QObject(parent), m_source(source), m_preferredDevice(preferredDevice.trimmed())
{
}

AudioCaptureWorker::~AudioCaptureWorker()
{
    stop();
}

void AudioCaptureWorker::start()
{
    if (m_running)
        return;

    m_running = true;
    m_stopRequested.store(false);
    runCaptureLoop();
    m_running = false;
    emit stopped();
}

void AudioCaptureWorker::stop()
{
    m_stopRequested.store(true);
}

void AudioCaptureWorker::runCaptureLoop()
{
#if defined(Q_OS_WIN)
    ComGuard comGuard;
    if (!comGuard.initialized)
    {
        LOG_ERROR("Audio capture COM initialization failed");
        return;
    }
#elif defined(Q_OS_LINUX)
#else
    LOG_WARN("Audio capture is not implemented on this platform yet");
    return;
#endif

    struct CapturePipeline
    {
        AudioCaptureWorker::CaptureSource source{AudioCaptureWorker::CaptureSource::SystemLoopback};
        std::unique_ptr<PlatformAudioCapture> capture;
        AudioChannelLayout inputLayout{};
        SwrContext *swr{nullptr};
        std::vector<uint8_t> captureBytes;
        std::vector<uint8_t> pcmBuffer;
    };

    const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
    if (!codec)
        codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec)
    {
        LOG_ERROR("Opus encoder not found");
        return;
    }

    AVCodecContext *encoder = avcodec_alloc_context3(codec);
    if (!encoder)
    {
        LOG_ERROR("Could not allocate Opus encoder context");
        return;
    }

    AudioChannelLayout outputLayout{};
    initDefaultLayout(outputLayout, kOpusChannels);

    encoder->sample_rate = kOpusSampleRate;
    encoder->sample_fmt = chooseEncoderSampleFormat(codec);
    encoder->bit_rate = kOpusBitrate;
    encoder->time_base = AVRational{1, kOpusSampleRate};
    int ret = setEncoderLayout(encoder, outputLayout);
    if (ret < 0)
    {
        LOG_ERROR("Could not configure Opus encoder channel layout: {}", ffmpegError(ret));
        uninitLayout(outputLayout);
        avcodec_free_context(&encoder);
        return;
    }
    encoder->frame_size = kOpusFrameSamples;

    AVDictionary *encoderOptions = nullptr;
    av_dict_set(&encoderOptions, "application", "audio", 0);
    av_dict_set(&encoderOptions, "vbr", "on", 0);
    av_dict_set(&encoderOptions, "compression_level", "10", 0);
    av_dict_set(&encoderOptions, "frame_duration", "20", 0);

    ret = avcodec_open2(encoder, codec, &encoderOptions);
    av_dict_free(&encoderOptions);
    if (ret < 0)
    {
        LOG_ERROR("Could not open Opus encoder: {}", ffmpegError(ret));
        uninitLayout(outputLayout);
        avcodec_free_context(&encoder);
        return;
    }

    std::vector<CapturePipeline> pipelines;
    for (AudioCaptureWorker::CaptureSource source : expandCaptureSources(m_source))
    {
        auto capture = std::make_unique<PlatformAudioCapture>();
#if defined(Q_OS_LINUX)
        capture->setStopFlag(&m_stopRequested);
#endif
        if (!capture->initialize(source, m_preferredDevice))
        {
            LOG_WARN("Audio capture source unavailable: {}", captureSourceName(source));
            continue;
        }

        CapturePipeline pipeline;
        pipeline.source = source;
        pipeline.capture = std::move(capture);
        initDefaultLayout(pipeline.inputLayout, pipeline.capture->channels());
        ret = allocResampler(&pipeline.swr, outputLayout, encoder->sample_fmt, kOpusSampleRate,
                             pipeline.inputLayout, pipeline.capture->sampleFormat(), pipeline.capture->sampleRate());
        if (ret < 0 || !pipeline.swr)
        {
            LOG_WARN("Could not allocate audio resampler for {}: {}", captureSourceName(source), ffmpegError(ret));
            swr_free(&pipeline.swr);
            uninitLayout(pipeline.inputLayout);
            continue;
        }
        ret = swr_init(pipeline.swr);
        if (ret < 0)
        {
            LOG_WARN("Could not initialize audio resampler for {}: {}", captureSourceName(source), ffmpegError(ret));
            swr_free(&pipeline.swr);
            uninitLayout(pipeline.inputLayout);
            continue;
        }
        pipelines.push_back(std::move(pipeline));
    }

    if (pipelines.empty())
    {
        LOG_ERROR("No audio capture source initialized for {}", captureSourceName(m_source));
        uninitLayout(outputLayout);
        avcodec_free_context(&encoder);
        return;
    }

    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    if (!frame || !packet)
    {
        LOG_ERROR("Could not allocate audio frame or packet");
        av_packet_free(&packet);
        av_frame_free(&frame);
        for (CapturePipeline &pipeline : pipelines)
        {
            swr_free(&pipeline.swr);
            uninitLayout(pipeline.inputLayout);
        }
        uninitLayout(outputLayout);
        avcodec_free_context(&encoder);
        return;
    }

    frame->format = encoder->sample_fmt;
    frame->sample_rate = kOpusSampleRate;
    frame->nb_samples = kOpusFrameSamples;
    ret = setFrameLayout(frame, outputLayout, kOpusChannels);
    if (ret < 0)
    {
        LOG_ERROR("Could not configure audio frame channel layout: {}", ffmpegError(ret));
        av_packet_free(&packet);
        av_frame_free(&frame);
        for (CapturePipeline &pipeline : pipelines)
        {
            swr_free(&pipeline.swr);
            uninitLayout(pipeline.inputLayout);
        }
        uninitLayout(outputLayout);
        avcodec_free_context(&encoder);
        return;
    }
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
    {
        LOG_ERROR("Could not allocate audio frame buffer: {}", ffmpegError(ret));
        av_packet_free(&packet);
        av_frame_free(&frame);
        for (CapturePipeline &pipeline : pipelines)
        {
            swr_free(&pipeline.swr);
            uninitLayout(pipeline.inputLayout);
        }
        uninitLayout(outputLayout);
        avcodec_free_context(&encoder);
        return;
    }

    qint64 pts = 0;
    qint64 lastNoPacketLogMs = 0;
    bool micVoiceGateOpen = false;
    int micAboveThresholdFrames = 0;
    int micBelowThresholdFrames = 0;
    const int bytesPerSample = av_get_bytes_per_sample(encoder->sample_fmt);
    const size_t frameBytes = static_cast<size_t>(kOpusFrameSamples) * kOpusChannels * static_cast<size_t>(bytesPerSample);

    auto encodeAndEmit = [&](const std::vector<uint8_t> &pcmFrame) {
        if (pcmFrame.size() < frameBytes)
            return;

        av_frame_make_writable(frame);
        memcpy(frame->data[0], pcmFrame.data(), frameBytes);
        frame->pts = pts;
        pts += kOpusFrameSamples;

        ret = avcodec_send_frame(encoder, frame);
        if (ret < 0)
        {
            LOG_WARN("Send audio frame to Opus encoder failed: {}", ffmpegError(ret));
            return;
        }

        while (true)
        {
            ret = avcodec_receive_packet(encoder, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
            {
                LOG_WARN("Receive Opus packet failed: {}", ffmpegError(ret));
                break;
            }

            std::shared_ptr<rtc::binary> encoded;
            if (copyPacketToBinary(packet, encoded))
            {
                qint64 packetPts = packet->pts;
                if (packetPts == AV_NOPTS_VALUE || packetPts < 0)
                    packetPts = qMax<qint64>(0, pts - kOpusFrameSamples);
                const quint64 timestampUs = static_cast<quint64>(av_rescale_q(packetPts, encoder->time_base, AVRational{1, 1000000}));
                emit opusFrameReady(encoded, timestampUs);
            }
            av_packet_unref(packet);
        }
    };

    LOG_INFO("Audio capture started: requestedSource={}, activeSources={}", captureSourceName(m_source), pipelines.size());

    int micIndex = -1;
    int loopbackIndex = -1;
    for (size_t i = 0; i < pipelines.size(); ++i)
    {
        if (pipelines[i].source == AudioCaptureWorker::CaptureSource::Microphone)
            micIndex = static_cast<int>(i);
        else if (pipelines[i].source == AudioCaptureWorker::CaptureSource::SystemLoopback)
            loopbackIndex = static_cast<int>(i);
    }

    const bool useThreadedMix = (m_source == AudioCaptureWorker::CaptureSource::MicrophoneAndSystem &&
                                 micIndex >= 0 && loopbackIndex >= 0 && pipelines.size() >= 2);

    if (useThreadedMix)
    {
        struct SharedFrame
        {
            std::mutex mutex;
            std::vector<uint8_t> frame;
            float rms{0.0f};
            quint64 seq{0};
        };

        SharedFrame micShared;
        SharedFrame loopShared;

        auto captureThreadMain = [&](CapturePipeline &pipeline, SharedFrame &shared, bool reportMicRms) {
            while (!m_stopRequested.load())
            {
                int capturedFrames = 0;
                bool silent = false;
                if (!pipeline.capture->readPacket(pipeline.captureBytes, capturedFrames, silent))
                {
                    QThread::msleep(2);
                    continue;
                }
                if (capturedFrames <= 0 || pipeline.captureBytes.empty())
                {
                    Q_UNUSED(silent);
                    continue;
                }

                appendConvertedAudio(pipeline.swr, pipeline.captureBytes, capturedFrames, encoder->sample_fmt, pipeline.pcmBuffer);
                while (pipeline.pcmBuffer.size() >= frameBytes)
                {
                    std::vector<uint8_t> oneFrame(frameBytes);
                    memcpy(oneFrame.data(), pipeline.pcmBuffer.data(), frameBytes);
                    pipeline.pcmBuffer.erase(pipeline.pcmBuffer.begin(),
                                             pipeline.pcmBuffer.begin() + static_cast<std::ptrdiff_t>(frameBytes));

                    float rms = 0.0f;
                    if (reportMicRms)
                        rms = computeFrameRms(oneFrame.data(), frameBytes, encoder->sample_fmt);

                    std::lock_guard<std::mutex> guard(shared.mutex);
                    shared.frame = std::move(oneFrame);
                    shared.rms = rms;
                    ++shared.seq;
                }
            }
        };

        std::thread micThread(captureThreadMain, std::ref(pipelines[static_cast<size_t>(micIndex)]), std::ref(micShared), true);
        std::thread loopThread(captureThreadMain, std::ref(pipelines[static_cast<size_t>(loopbackIndex)]), std::ref(loopShared), false);

        quint64 lastMicSeq = 0;
        quint64 lastLoopSeq = 0;
        std::vector<uint8_t> latestMicFrame;
        std::vector<uint8_t> latestLoopFrame;

        constexpr float kMicVadThresholdEnter = 0.10f;
        constexpr float kMicVadThresholdExit = 0.07f;
        constexpr int kMicVadEnterFrames = 2;
        constexpr int kMicVadExitFrames = 4;
        constexpr float kMicGainWhenActive = 1.25f;
        constexpr float kLoopbackGainWhenMicActive = 0.70f;

        while (!m_stopRequested.load())
        {
            const qint64 tickStartMs = QDateTime::currentMSecsSinceEpoch();
            bool hasMicFresh = false;
            bool hasLoopFresh = false;
            float micRms = 0.0f;

            {
                std::lock_guard<std::mutex> guard(micShared.mutex);
                if (micShared.seq != lastMicSeq && micShared.frame.size() >= frameBytes)
                {
                    lastMicSeq = micShared.seq;
                    latestMicFrame = micShared.frame;
                    micRms = micShared.rms;
                    hasMicFresh = true;
                }
            }
            {
                std::lock_guard<std::mutex> guard(loopShared.mutex);
                if (loopShared.seq != lastLoopSeq && loopShared.frame.size() >= frameBytes)
                {
                    lastLoopSeq = loopShared.seq;
                    latestLoopFrame = loopShared.frame;
                    hasLoopFresh = true;
                }
            }

            if (hasMicFresh)
            {
                emit inputLevelUpdated(micRms);
                if (micRms >= kMicVadThresholdEnter)
                {
                    ++micAboveThresholdFrames;
                    micBelowThresholdFrames = 0;
                }
                else if (micRms <= kMicVadThresholdExit)
                {
                    ++micBelowThresholdFrames;
                    micAboveThresholdFrames = 0;
                }
                else
                {
                    micAboveThresholdFrames = 0;
                    micBelowThresholdFrames = 0;
                }
            }
            else
            {
                ++micBelowThresholdFrames;
                micAboveThresholdFrames = 0;
            }

            if (!micVoiceGateOpen && micAboveThresholdFrames >= kMicVadEnterFrames)
                micVoiceGateOpen = true;
            if (micVoiceGateOpen && micBelowThresholdFrames >= kMicVadExitFrames)
                micVoiceGateOpen = false;

            std::vector<uint8_t> mixed(frameBytes, 0);
            const size_t sampleBytes = static_cast<size_t>(qMax(1, bytesPerSample));
            const size_t sampleCount = frameBytes / sampleBytes;
            const bool hasMic = hasMicFresh && latestMicFrame.size() >= frameBytes;
            const bool hasLoop = hasLoopFresh && latestLoopFrame.size() >= frameBytes;

            if (hasMic || hasLoop)
            {
                for (size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
                {
                    float mixedSample = 0.0f;
                    if (hasMic && micVoiceGateOpen)
                    {
                        float micSample = readPackedSampleAsFloat(latestMicFrame.data() + sampleIndex * sampleBytes,
                                                                   encoder->sample_fmt);
                        mixedSample += micSample * kMicGainWhenActive;
                    }
                    if (hasLoop)
                    {
                        float loopSample = readPackedSampleAsFloat(latestLoopFrame.data() + sampleIndex * sampleBytes,
                                                                    encoder->sample_fmt);
                        if (micVoiceGateOpen)
                            loopSample *= kLoopbackGainWhenMicActive;
                        mixedSample += loopSample;
                    }

                    writeFloatAsPackedSample(mixedSample, mixed.data() + sampleIndex * sampleBytes, encoder->sample_fmt);
                }
                encodeAndEmit(mixed);
            }

            const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - tickStartMs;
            if (elapsed < kOpusFrameMs)
                QThread::msleep(static_cast<unsigned long>(kOpusFrameMs - elapsed));
        }

        if (micThread.joinable())
            micThread.join();
        if (loopThread.joinable())
            loopThread.join();
    }
    else
    {
        while (!m_stopRequested.load())
        {
            bool producedAny = false;
            bool readFailure = false;

            for (CapturePipeline &pipeline : pipelines)
            {
                int capturedFrames = 0;
                bool silent = false;
                if (!pipeline.capture->readPacket(pipeline.captureBytes, capturedFrames, silent))
                {
                    readFailure = true;
                    continue;
                }

                if (capturedFrames <= 0 || pipeline.captureBytes.empty())
                    continue;

                const size_t oldSize = pipeline.pcmBuffer.size();
                appendConvertedAudio(pipeline.swr, pipeline.captureBytes, capturedFrames, encoder->sample_fmt, pipeline.pcmBuffer);
                producedAny = producedAny || pipeline.pcmBuffer.size() > oldSize;
                Q_UNUSED(silent);
            }

            if (readFailure)
            {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - lastNoPacketLogMs > 2000)
                {
                    LOG_WARN("Audio capture read failed for one or more sources; retrying");
                    lastNoPacketLogMs = now;
                }
            }

            while (!pipelines.empty() && !m_stopRequested.load())
            {
                bool hasFrame = false;
                for (const CapturePipeline &pipeline : pipelines)
                {
                    if (pipeline.pcmBuffer.size() >= frameBytes)
                    {
                        hasFrame = true;
                        break;
                    }
                }
                if (!hasFrame)
                    break;

                std::vector<uint8_t> encodedPcm(frameBytes, 0);
                if (pipelines.size() == 1)
                {
                    CapturePipeline &pipeline = pipelines.front();
                    if (pipeline.pcmBuffer.size() >= frameBytes)
                    {
                        memcpy(encodedPcm.data(), pipeline.pcmBuffer.data(), frameBytes);
                        if (pipeline.source == AudioCaptureWorker::CaptureSource::Microphone)
                        {
                            applyGainToPackedFrame(encodedPcm.data(), frameBytes, encoder->sample_fmt, AudioEchoSuppression::microphoneGain());
                            emit inputLevelUpdated(computeFrameRms(encodedPcm.data(), frameBytes, encoder->sample_fmt));
                        }
                        pipeline.pcmBuffer.erase(pipeline.pcmBuffer.begin(), pipeline.pcmBuffer.begin() + static_cast<std::ptrdiff_t>(frameBytes));
                    }
                }
                else
                {
                    const size_t sampleBytes = static_cast<size_t>(qMax(1, bytesPerSample));
                    const size_t sampleCount = frameBytes / sampleBytes;

                    int micPipeIndex = -1;
                    int loopPipeIndex = -1;
                    bool micFrameReady = false;
                    bool loopbackFrameReady = false;
                    bool micActive = false;

                    for (size_t pi = 0; pi < pipelines.size(); ++pi)
                    {
                        const bool ready = pipelines[pi].pcmBuffer.size() >= frameBytes;
                        if (pipelines[pi].source == AudioCaptureWorker::CaptureSource::Microphone)
                        {
                            micPipeIndex = static_cast<int>(pi);
                            micFrameReady = ready;
                            if (ready)
                            {
                                const float micRms = computeFrameRms(pipelines[pi].pcmBuffer.data(), frameBytes, encoder->sample_fmt);
                                constexpr float kMicVadThreshold = 0.10f;
                                micActive = micRms >= kMicVadThreshold;
                                emit inputLevelUpdated(micRms);
                            }
                        }
                        else if (pipelines[pi].source == AudioCaptureWorker::CaptureSource::SystemLoopback)
                        {
                            loopPipeIndex = static_cast<int>(pi);
                            loopbackFrameReady = ready;
                        }
                    }

                    for (size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
                    {
                        float mixed = 0.0f;
                        if (micFrameReady && micPipeIndex >= 0 && micActive)
                        {
                            float micSample = readPackedSampleAsFloat(
                                pipelines[static_cast<size_t>(micPipeIndex)].pcmBuffer.data() + sampleIndex * sampleBytes,
                                encoder->sample_fmt);
                            mixed += micSample * 1.25f;
                        }
                        if (loopbackFrameReady && loopPipeIndex >= 0)
                        {
                            float loopSample = readPackedSampleAsFloat(
                                pipelines[static_cast<size_t>(loopPipeIndex)].pcmBuffer.data() + sampleIndex * sampleBytes,
                                encoder->sample_fmt);
                            if (micActive)
                                loopSample *= 0.70f;
                            mixed += loopSample;
                        }
                        writeFloatAsPackedSample(mixed, encodedPcm.data() + sampleIndex * sampleBytes, encoder->sample_fmt);
                    }

                    for (CapturePipeline &pipeline : pipelines)
                    {
                        if (pipeline.pcmBuffer.size() >= frameBytes)
                            pipeline.pcmBuffer.erase(pipeline.pcmBuffer.begin(), pipeline.pcmBuffer.begin() + static_cast<std::ptrdiff_t>(frameBytes));
                    }
                }

                encodeAndEmit(encodedPcm);
            }

            if (!producedAny)
                QThread::msleep(5);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    for (CapturePipeline &pipeline : pipelines)
    {
        swr_free(&pipeline.swr);
        uninitLayout(pipeline.inputLayout);
    }
    uninitLayout(outputLayout);
    avcodec_free_context(&encoder);
    LOG_INFO("Audio capture stopped");
}
