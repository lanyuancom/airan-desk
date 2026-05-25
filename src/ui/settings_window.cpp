#include "settings_window.h"
#include "common/constant.h"
#include "ui/app_title_bar.h"
#include "ui/adaptive_ui.h"
#include "audio/audio_capture_worker.h"
#include "ui/password_line_edit_util.h"
#include "ui_settings_window.h"
#include "util/config_util.h"
#include "util/i18n_util.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QAbstractButton>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QCloseEvent>
#include <QScreen>
#include <QShowEvent>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QProgressBar>
#include <QSizePolicy>
#include <QStyledItemDelegate>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QSize>
#include <cmath>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#endif

namespace
{
    constexpr const char *kServiceName = "airan-desk";
    constexpr const char *kServiceDisplayName = "Airan Desk";
    const QString kAudioDeviceNoneValue = QStringLiteral("__none__");

    class ComboPopupDelegate : public QStyledItemDelegate
    {
    public:
        explicit ComboPopupDelegate(QObject *parent = nullptr)
            : QStyledItemDelegate(parent)
        {
        }

        QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
        {
            QSize size = QStyledItemDelegate::sizeHint(option, index);
            size.setHeight(qMax(size.height(), 34));
            return size;
        }
    };

    void setComboByData(QComboBox *combo, const QString &data)
    {
        const int index = combo ? combo->findData(data) : -1;
        if (index >= 0)
            combo->setCurrentIndex(index);
    }

    void polishSettingsField(QWidget *field)
    {
        if (!field)
            return;

        field->setMinimumHeight(34);
        field->setMinimumWidth(260);
        field->setSizePolicy(QSizePolicy::Expanding, field->sizePolicy().verticalPolicy());
    }

    void polishSettingsCombo(QComboBox *combo)
    {
        polishSettingsField(combo);
        combo->setItemDelegate(new ComboPopupDelegate(combo));
        if (auto *listView = qobject_cast<QListView *>(combo->view()))
            listView->setSpacing(2);
        combo->view()->setTextElideMode(Qt::ElideRight);
    }

    struct AudioDeviceItem
    {
        QString id;
        QString displayName;
        bool loopback{false};
    };

#if defined(Q_OS_LINUX)
    QList<AudioDeviceItem> enumerateAudioDevices()
    {
        QList<AudioDeviceItem> devices;
        QProcess process;
        process.start(QStringLiteral("pactl"),
                      QStringList() << QStringLiteral("list") << QStringLiteral("short") << QStringLiteral("sources"));
        if (!process.waitForFinished(1200) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return devices;

        const QString output = QString::fromUtf8(process.readAllStandardOutput());
        for (const QString &line : output.split(QLatin1Char('\n')))
        {
            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.size() < 2)
                continue;
            const QString name = parts[1].trimmed();
            if (name.isEmpty())
                continue;
            const bool loopback = name.endsWith(QStringLiteral(".monitor"));
            AudioDeviceItem item;
            item.id = name;
            item.displayName = name;
            item.loopback = loopback;
            devices.push_back(item);
        }
        return devices;
    }
#elif defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    template <typename T>
    void releaseCom(T *&ptr)
    {
        if (ptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
    }

    QString windowsDeviceId(IMMDevice *device)
    {
        if (!device)
            return QString();

        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || !id)
            return QString();
        const QString result = QString::fromWCharArray(id);
        CoTaskMemFree(id);
        return result;
    }

    QString windowsFriendlyName(IMMDevice *device)
    {
        if (!device)
            return QString();

        IPropertyStore *store = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store)
            return QString();

        PROPVARIANT varName;
        PropVariantInit(&varName);
        QString name;
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR && varName.pwszVal)
            name = QString::fromWCharArray(varName.pwszVal);
        PropVariantClear(&varName);
        store->Release();
        return name;
    }

    IMMDevice *resolveWindowsOutputDevice(IMMDeviceEnumerator *enumerator, const QString &configured)
    {
        if (!enumerator)
            return nullptr;

        if (!configured.trimmed().isEmpty())
        {
            IMMDevice *direct = nullptr;
            const std::wstring wid = configured.trimmed().toStdWString();
            if (!wid.empty() && SUCCEEDED(enumerator->GetDevice(wid.c_str(), &direct)) && direct)
                return direct;
        }

        if (!configured.trimmed().isEmpty())
        {
            IMMDeviceCollection *collection = nullptr;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) && collection)
            {
                const QString hint = configured.trimmed();
                const QString hintLower = hint.toLower();
                UINT count = 0;
                collection->GetCount(&count);

                IMMDevice *best = nullptr;
                int bestScore = -1;
                for (UINT i = 0; i < count; ++i)
                {
                    IMMDevice *candidate = nullptr;
                    if (FAILED(collection->Item(i, &candidate)) || !candidate)
                        continue;

                    const QString id = windowsDeviceId(candidate);
                    const QString name = windowsFriendlyName(candidate);
                    int score = -1;
                    if (!id.isEmpty() && id.compare(hint, Qt::CaseInsensitive) == 0)
                        score = 4;
                    else if (!name.isEmpty() && name.compare(hint, Qt::CaseInsensitive) == 0)
                        score = 3;
                    else if (id.toLower().contains(hintLower) || name.toLower().contains(hintLower))
                        score = 2;

                    if (score > bestScore)
                    {
                        releaseCom(best);
                        best = candidate;
                        bestScore = score;
                    }
                    else
                    {
                        candidate->Release();
                    }
                }
                collection->Release();
                if (best)
                    return best;
            }
        }

        IMMDevice *fallback = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &fallback)) && fallback)
            return fallback;
        return nullptr;
    }

    bool playToneOnWindowsOutput(const QString &configuredOutput)
    {
        const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninit = SUCCEEDED(initHr);

        IMMDeviceEnumerator *enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
        if (FAILED(hr) || !enumerator)
        {
            if (shouldUninit)
                CoUninitialize();
            return false;
        }

        IMMDevice *device = resolveWindowsOutputDevice(enumerator, configuredOutput);
        if (!device)
        {
            enumerator->Release();
            if (shouldUninit)
                CoUninitialize();
            return false;
        }

        IAudioClient *audioClient = nullptr;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&audioClient));
        if (FAILED(hr) || !audioClient)
        {
            device->Release();
            enumerator->Release();
            if (shouldUninit)
                CoUninitialize();
            return false;
        }

        WAVEFORMATEX *mixFormat = nullptr;
        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat)
        {
            releaseCom(audioClient);
            device->Release();
            enumerator->Release();
            if (shouldUninit)
                CoUninitialize();
            return false;
        }

        constexpr REFERENCE_TIME kBufferDuration = 2000000; // 200ms
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, mixFormat, nullptr);
        if (FAILED(hr))
        {
            CoTaskMemFree(mixFormat);
            releaseCom(audioClient);
            device->Release();
            enumerator->Release();
            if (shouldUninit)
                CoUninitialize();
            return false;
        }

        IAudioRenderClient *renderClient = nullptr;
        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&renderClient));
        if (FAILED(hr) || !renderClient)
        {
            CoTaskMemFree(mixFormat);
            releaseCom(audioClient);
            device->Release();
            enumerator->Release();
            if (shouldUninit)
                CoUninitialize();
            return false;
        }

        UINT32 bufferFrames = 0;
        audioClient->GetBufferSize(&bufferFrames);
        hr = audioClient->Start();
        if (FAILED(hr))
        {
            releaseCom(renderClient);
            CoTaskMemFree(mixFormat);
            releaseCom(audioClient);
            device->Release();
            enumerator->Release();
            if (shouldUninit)
                CoUninitialize();
            return false;
        }

        const int sampleRate = qMax(8000, static_cast<int>(mixFormat->nSamplesPerSec));
        const UINT32 totalFrames = static_cast<UINT32>(sampleRate / 3); // ~333ms
        const int channels = qMax(1, static_cast<int>(mixFormat->nChannels));
        const bool isFloat = mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                             (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                              reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mixFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

        UINT32 written = 0;
        double phase = 0.0;
        const double omega = 2.0 * 3.14159265358979323846 * 880.0 / static_cast<double>(sampleRate);
        while (written < totalFrames)
        {
            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding)))
                break;
            const UINT32 available = bufferFrames > padding ? (bufferFrames - padding) : 0;
            if (available == 0)
            {
                Sleep(5);
                continue;
            }

            const UINT32 frames = qMin(available, totalFrames - written);
            BYTE *data = nullptr;
            if (FAILED(renderClient->GetBuffer(frames, &data)) || !data)
                break;

            if (isFloat && mixFormat->wBitsPerSample == 32)
            {
                float *out = reinterpret_cast<float *>(data);
                for (UINT32 i = 0; i < frames; ++i)
                {
                    const float s = static_cast<float>(std::sin(phase) * 0.18);
                    phase += omega;
                    for (int c = 0; c < channels; ++c)
                        out[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)] = s;
                }
            }
            else if (mixFormat->wBitsPerSample == 16)
            {
                qint16 *out = reinterpret_cast<qint16 *>(data);
                for (UINT32 i = 0; i < frames; ++i)
                {
                    const qint16 s = static_cast<qint16>(std::sin(phase) * 6000.0);
                    phase += omega;
                    for (int c = 0; c < channels; ++c)
                        out[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)] = s;
                }
            }
            else
            {
                memset(data, 0, static_cast<size_t>(frames) * static_cast<size_t>(mixFormat->nBlockAlign));
            }

            renderClient->ReleaseBuffer(frames, 0);
            written += frames;
        }

        Sleep(80);
        audioClient->Stop();

        releaseCom(renderClient);
        CoTaskMemFree(mixFormat);
        releaseCom(audioClient);
        device->Release();
        enumerator->Release();
        if (shouldUninit)
            CoUninitialize();
        return true;
    }

    QList<AudioDeviceItem> enumerateAudioDevices()
    {
        QList<AudioDeviceItem> devices;

        const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninit = SUCCEEDED(initHr);

        IMMDeviceEnumerator *enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
        if (FAILED(hr) || !enumerator)
        {
            if (shouldUninit)
                CoUninitialize();
            return devices;
        }

        auto appendFlow = [&](EDataFlow flow, bool loopbackFlag)
        {
            IMMDeviceCollection *collection = nullptr;
            if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) || !collection)
                return;

            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i)
            {
                IMMDevice *device = nullptr;
                if (FAILED(collection->Item(i, &device)) || !device)
                    continue;

                LPWSTR idW = nullptr;
                QString id;
                if (SUCCEEDED(device->GetId(&idW)) && idW)
                {
                    id = QString::fromWCharArray(idW);
                    CoTaskMemFree(idW);
                }

                IPropertyStore *store = nullptr;
                QString friendly;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store)
                {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &varName)) &&
                        varName.vt == VT_LPWSTR && varName.pwszVal)
                    {
                        friendly = QString::fromWCharArray(varName.pwszVal);
                    }
                    PropVariantClear(&varName);
                    store->Release();
                }

                AudioDeviceItem item;
                item.id = id;
                item.displayName = friendly.isEmpty() ? id : friendly;
                item.loopback = loopbackFlag;
                devices.push_back(item);
                device->Release();
            }
            collection->Release();
        };

        appendFlow(eCapture, false);
        appendFlow(eRender, true);

        enumerator->Release();
        if (shouldUninit)
            CoUninitialize();

        return devices;
    }
#else
    QList<AudioDeviceItem> enumerateAudioDevices()
    {
        return {};
    }
#endif
}

SettingsWindow::SettingsWindow(QWidget *parent)
    : QDialog(parent)
{
    ui = new Ui::SettingsWindow();
    ui->setupUi(this);
    setObjectName(QStringLiteral("settingsWindowFrame"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(tr("Settings"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setModal(false);
    UiAdaptive::applyAdaptiveWindowSize(this, QSize(720, 520), QSize(420, 320));

    ui->titleBarLayout->addWidget(new AppTitleBar(this, true, false, this));
    ui->settingsTabs->setDocumentMode(true);
    bindUiObjects();
    populateSettingControls();
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &SettingsWindow::saveSettings);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &SettingsWindow::close);
    if (QAbstractButton *closeBtn = ui->buttonBox->button(QDialogButtonBox::Close))
        connect(closeBtn, &QAbstractButton::clicked, this, &SettingsWindow::close);
    connect(m_serviceCheck, &QCheckBox::toggled, this, &SettingsWindow::onServiceInstallToggled);
    connect(m_refreshAudioDevicesBtn, &QPushButton::clicked, this, &SettingsWindow::refreshAudioDevices);
    connect(m_testSpeakerBtn, &QPushButton::clicked, this, &SettingsWindow::testSpeaker);
    connect(m_testMicBtn, &QPushButton::clicked, this, &SettingsWindow::toggleMicTest);

    refreshServiceState();
}

SettingsWindow::~SettingsWindow()
{
    stopMicTest();
    delete ui;
}

void SettingsWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!m_centeredOnFirstShow)
    {
        centerOnParent();
        m_centeredOnFirstShow = true;
    }
}

void SettingsWindow::closeEvent(QCloseEvent *event)
{
    if (m_micTestThread && m_micTestThread->isRunning())
    {
        // 立即响应关闭：界面先隐藏，采集线程后台异步停止。
        stopMicTest();
        hide();
        event->ignore();
        return;
    }

    QDialog::closeEvent(event);
}

void SettingsWindow::centerOnParent()
{
    QWidget *parentWindow = parentWidget() ? parentWidget()->window() : nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    QRect anchor = parentWindow ? parentWindow->frameGeometry()
                                : (this->screen() ? this->screen()->availableGeometry()
                                                  : QGuiApplication::primaryScreen()->availableGeometry());
#else
    QRect anchor = parentWindow ? parentWindow->frameGeometry()
                                : QGuiApplication::primaryScreen()->availableGeometry();
#endif

    QRect target(QPoint(0, 0), size());
    target.moveCenter(anchor.center());

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    QScreen *targetScreen = parentWindow ? parentWindow->screen() : this->screen();
#else
    QScreen *targetScreen = QGuiApplication::primaryScreen();
#endif
    const QRect available = targetScreen ? targetScreen->availableGeometry()
                                         : QGuiApplication::primaryScreen()->availableGeometry();
    if (target.left() < available.left())
        target.moveLeft(available.left());
    if (target.top() < available.top())
        target.moveTop(available.top());
    if (target.right() > available.right())
        target.moveRight(available.right());
    if (target.bottom() > available.bottom())
        target.moveBottom(available.bottom());

    move(target.topLeft());
}

void SettingsWindow::bindUiObjects()
{
    m_showUiCheck = ui->showUiCheck;
    m_logLevelCombo = ui->logLevelCombo;
    m_languageCombo = ui->languageCombo;
    m_wsUrlEdit = ui->wsUrlEdit;
    m_iceHostEdit = ui->iceHostEdit;
    m_icePortSpin = ui->icePortSpin;
    m_iceUserEdit = ui->iceUserEdit;
    m_icePasswordEdit = ui->icePasswordEdit;
    m_audioMicDeviceCombo = ui->audioMicDeviceCombo;
    m_audioLoopbackDeviceCombo = ui->audioLoopbackDeviceCombo;
    m_refreshAudioDevicesBtn = ui->refreshAudioDevicesBtn;
    m_testSpeakerBtn = ui->testSpeakerBtn;
    m_testMicBtn = ui->testMicBtn;
    m_micLevelBar = ui->micLevelBar;
    m_fpsSpin = ui->fpsSpin;
    m_qualityCombo = ui->qualityCombo;
    m_bitrateCombo = ui->bitrateCombo;
    m_networkCombo = ui->networkCombo;
    m_captureCombo = ui->captureCombo;
    m_resolutionCombo = ui->resolutionCombo;
    m_serviceCheck = ui->serviceCheck;
    m_serviceStatusLabel = ui->serviceStatusLabel;
}

void SettingsWindow::populateSettingControls()
{
    m_showUiCheck->setChecked(ConfigUtil->showUI);

    for (const QString &level : {QStringLiteral("trace"), QStringLiteral("debug"), QStringLiteral("info"),
                                 QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("critical")})
        m_logLevelCombo->addItem(level, level);
    setComboByData(m_logLevelCombo, ConfigUtil->logLevelStr);

    m_languageCombo->addItem(tr("Auto"), I18nUtil::autoLanguageKey());
    m_languageCombo->addItem(tr("Simplified Chinese"), I18nUtil::zhCnLanguageKey());
    m_languageCombo->addItem(tr("English"), I18nUtil::enUsLanguageKey());
    setComboByData(m_languageCombo, I18nUtil::normalizeUiLanguage(ConfigUtil->language));

    m_wsUrlEdit->setText(ConfigUtil->wsUrl);
    m_iceHostEdit->setText(ConfigUtil->ice_host);
    m_icePortSpin->setRange(1, 65535);
    m_icePortSpin->setValue(ConfigUtil->ice_port);
    m_iceUserEdit->setText(ConfigUtil->ice_username);
    m_icePasswordEdit->setText(ConfigUtil->ice_password);
    refreshAudioDevices();
    setComboByData(m_audioMicDeviceCombo, ConfigUtil->audio_mic_device);
    setComboByData(m_audioLoopbackDeviceCombo, ConfigUtil->audio_loopback_device);
    m_micLevelBar->setValue(0);
    installPasswordRevealButton(m_icePasswordEdit);

    m_fpsSpin->setRange(1, 60);
    m_fpsSpin->setValue(ConfigUtil->fps);

    m_qualityCombo->addItem(tr("Stable"), QStringLiteral("quality"));
    m_qualityCombo->addItem(tr("Smooth"), QStringLiteral("smooth"));
    m_qualityCombo->addItem(tr("Compatible"), QStringLiteral("compat"));
    setComboByData(m_qualityCombo, ConfigUtil->remote_quality);

    m_bitrateCombo->addItem(tr("Low"), QStringLiteral("low"));
    m_bitrateCombo->addItem(tr("Medium"), QStringLiteral("medium"));
    m_bitrateCombo->addItem(tr("High"), QStringLiteral("high"));
    setComboByData(m_bitrateCombo, ConfigUtil->remote_bitrate_profile);

    m_networkCombo->addItem(tr("Auto"), QStringLiteral("auto"));
    m_networkCombo->addItem(tr("Direct"), QStringLiteral("direct"));
    m_networkCombo->addItem(tr("TURN UDP"), QStringLiteral("turn_udp"));
    m_networkCombo->addItem(tr("TURN TCP"), QStringLiteral("turn_tcp"));
    setComboByData(m_networkCombo, ConfigUtil->remote_network_path);

    m_captureCombo->addItem(tr("Auto"), QStringLiteral("auto"));
    m_captureCombo->addItem(tr("GPU"), QStringLiteral("wgc"));
    m_captureCombo->addItem(tr("CPU"), QStringLiteral("qt"));
    setComboByData(m_captureCombo, ConfigUtil->remote_capture_backend);

    m_resolutionCombo->addItem(tr("Original"), QSize(0, 0));
    m_resolutionCombo->addItem(QStringLiteral("1280x720"), QSize(1280, 720));
    m_resolutionCombo->addItem(QStringLiteral("1920x1080"), QSize(1920, 1080));
    const QSize currentResolution(ConfigUtil->remote_width, ConfigUtil->remote_height);
    for (int i = 0; i < m_resolutionCombo->count(); ++i)
    {
        if (m_resolutionCombo->itemData(i).toSize() == currentResolution)
        {
            m_resolutionCombo->setCurrentIndex(i);
            break;
        }
    }

    const QList<QWidget *> fields{m_logLevelCombo, m_languageCombo, m_wsUrlEdit, m_iceHostEdit,
                                  m_icePortSpin, m_iceUserEdit, m_icePasswordEdit,
                                  m_audioMicDeviceCombo, m_audioLoopbackDeviceCombo, m_fpsSpin,
                                  m_qualityCombo, m_bitrateCombo, m_networkCombo, m_captureCombo,
                                  m_resolutionCombo};
    for (QWidget *field : fields)
        polishSettingsField(field);

    for (QComboBox *combo : {m_logLevelCombo, m_languageCombo, m_qualityCombo, m_bitrateCombo,
                             m_networkCombo, m_captureCombo, m_resolutionCombo,
                             m_audioMicDeviceCombo, m_audioLoopbackDeviceCombo})
        polishSettingsCombo(combo);

    setPasswordRevealFonts(m_icePasswordEdit, m_icePasswordEdit->font(), m_iceUserEdit->font());
}

void SettingsWindow::saveSettings()
{
    ConfigUtil->showUI = m_showUiCheck->isChecked();
    ConfigUtil->logLevelStr = m_logLevelCombo->currentData().toString();
    ConfigUtil->language = I18nUtil::normalizeUiLanguage(m_languageCombo->currentData().toString());
    ConfigUtil->wsUrl = m_wsUrlEdit->text().trimmed();
    ConfigUtil->ice_host = m_iceHostEdit->text().trimmed();
    ConfigUtil->ice_port = static_cast<uint16_t>(m_icePortSpin->value());
    ConfigUtil->ice_username = m_iceUserEdit->text().trimmed();
    ConfigUtil->ice_password = m_icePasswordEdit->text();
    ConfigUtil->audio_mic_device = selectedAudioDeviceValue(m_audioMicDeviceCombo).trimmed();
    ConfigUtil->audio_loopback_device = selectedAudioDeviceValue(m_audioLoopbackDeviceCombo).trimmed();

    ConfigUtil->fps = m_fpsSpin->value();
    ConfigUtil->remote_quality = m_qualityCombo->currentData().toString();
    ConfigUtil->remote_bitrate_profile = m_bitrateCombo->currentData().toString();
    ConfigUtil->remote_network_path = m_networkCombo->currentData().toString();
    ConfigUtil->remote_capture_backend = m_captureCombo->currentData().toString();
    const QSize res = m_resolutionCombo->currentData().toSize();
    ConfigUtil->remote_width = res.width();
    ConfigUtil->remote_height = res.height();

    ConfigUtil->saveCommonConfig();
    if (ConfigUtil->showUI)
        QMessageBox::information(this, tr("Settings"), tr("Saved."));
}

void SettingsWindow::refreshServiceState()
{
    if (!m_serviceCheck || !m_serviceStatusLabel)
        return;

    m_refreshingService = true;
    const bool installed = isServiceInstalled();
    m_serviceCheck->setChecked(installed);
    m_serviceStatusLabel->setText(installed ? tr("Service installed.") : tr("Service not installed."));
    m_refreshingService = false;
}

void SettingsWindow::onServiceInstallToggled(bool checked)
{
    if (m_refreshingService)
        return;

    QString error;
    bool ok = checked ? installService(&error) : uninstallService(&error);
    if (!ok)
    {
        if (ConfigUtil->showUI)
            QMessageBox::critical(this, tr("Service"), error.isEmpty() ? tr("Service operation failed.") : error);
        refreshServiceState();
        return;
    }

    refreshServiceState();
}

bool SettingsWindow::isServiceInstalled() const
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    QProcess proc;
    proc.start(QStringLiteral("sc.exe"), {QStringLiteral("query"), QString::fromUtf8(kServiceName)});
    proc.waitForFinished(3000);
    return proc.exitCode() == 0;
#else
    return false;
#endif
}

bool SettingsWindow::runProcess(const QString &program, const QStringList &arguments, QString *errorMessage) const
{
    QProcess proc;
    proc.start(program, arguments);
    if (!proc.waitForStarted(3000))
    {
        if (errorMessage)
            *errorMessage = proc.errorString();
        return false;
    }
    if (!proc.waitForFinished(10000))
    {
        proc.kill();
        if (errorMessage)
            *errorMessage = tr("Process timeout.");
        return false;
    }
    if (proc.exitCode() != 0)
    {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(proc.readAllStandardError());
        return false;
    }
    return true;
}

QString SettingsWindow::selectedAudioDeviceValue(QComboBox *combo) const
{
    if (!combo)
        return QString();

    const QString text = combo->currentText().trimmed();
    const int idx = combo->currentIndex();
    if (idx >= 0 && idx < combo->count() && text == combo->itemText(idx).trimmed())
    {
        const QString id = combo->itemData(idx).toString().trimmed();
        if (!id.isEmpty())
            return id;
    }
    return text;
}

void SettingsWindow::refreshAudioDevices()
{
    const QString currentMic = selectedAudioDeviceValue(m_audioMicDeviceCombo);
    const QString currentLoopback = selectedAudioDeviceValue(m_audioLoopbackDeviceCombo);

    m_audioMicDeviceCombo->clear();
    m_audioLoopbackDeviceCombo->clear();

    const QList<AudioDeviceItem> devices = enumerateAudioDevices();
    for (const AudioDeviceItem &item : devices)
    {
        if (item.loopback)
            m_audioLoopbackDeviceCombo->addItem(item.displayName, item.id);
        else
            m_audioMicDeviceCombo->addItem(item.displayName, item.id);
    }

    m_audioMicDeviceCombo->addItem(tr("None"), kAudioDeviceNoneValue);
    m_audioLoopbackDeviceCombo->addItem(tr("None"), kAudioDeviceNoneValue);

    // 即使枚举失败，也要提供可选项，避免“无下拉可选设备”的体验。
    if (m_audioMicDeviceCombo->count() == 0)
        m_audioMicDeviceCombo->addItem(tr("System default"), QStringLiteral("default"));
    if (m_audioLoopbackDeviceCombo->count() == 0)
    {
#if defined(Q_OS_LINUX)
        m_audioLoopbackDeviceCombo->addItem(QStringLiteral("@DEFAULT_MONITOR@"), QStringLiteral("@DEFAULT_MONITOR@"));
#else
        m_audioLoopbackDeviceCombo->addItem(tr("System default"), QStringLiteral("default"));
#endif
    }

    // 纯下拉选择，避免看起来像输入框且无法明确下拉交互。
    m_audioMicDeviceCombo->setInsertPolicy(QComboBox::NoInsert);
    m_audioLoopbackDeviceCombo->setInsertPolicy(QComboBox::NoInsert);
    m_audioMicDeviceCombo->setEditable(false);
    m_audioLoopbackDeviceCombo->setEditable(false);

    if (currentMic.isEmpty() && m_audioMicDeviceCombo->count() > 0)
        m_audioMicDeviceCombo->setCurrentIndex(0);
    else
        setComboByData(m_audioMicDeviceCombo, currentMic);

    if (currentLoopback.isEmpty() && m_audioLoopbackDeviceCombo->count() > 0)
        m_audioLoopbackDeviceCombo->setCurrentIndex(0);
    else
        setComboByData(m_audioLoopbackDeviceCombo, currentLoopback);
}

void SettingsWindow::testSpeaker()
{
    if (selectedAudioDeviceValue(m_audioLoopbackDeviceCombo).compare(kAudioDeviceNoneValue, Qt::CaseInsensitive) == 0)
        return;

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    const QString preferredOutput = selectedAudioDeviceValue(m_audioLoopbackDeviceCombo);
    if (!playToneOnWindowsOutput(preferredOutput))
        MessageBeep(MB_ICONASTERISK);
#elif defined(Q_OS_LINUX)
    QString sink = selectedAudioDeviceValue(m_audioLoopbackDeviceCombo).trimmed();
    if (sink.endsWith(QStringLiteral(".monitor")))
        sink.chop(static_cast<int>(QStringLiteral(".monitor").size()));

    if (sink == QStringLiteral("@DEFAULT_MONITOR@") || sink.isEmpty())
    {
        QProcess proc;
        proc.start(QStringLiteral("pactl"), QStringList() << QStringLiteral("get-default-sink"));
        if (proc.waitForFinished(500) && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0)
            sink = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    }

    const QStringList candidates{
        QStringLiteral("/usr/share/sounds/freedesktop/stereo/bell.oga"),
        QStringLiteral("/usr/share/sounds/freedesktop/stereo/complete.oga"),
        QStringLiteral("/usr/share/sounds/alsa/Front_Center.wav")};
    QString soundFile;
    for (const QString &candidate : candidates)
    {
        if (QFile::exists(candidate))
        {
            soundFile = candidate;
            break;
        }
    }

    if (!soundFile.isEmpty())
    {
        QStringList args;
        if (!sink.isEmpty())
            args << QStringLiteral("--device=") + sink;
        args << soundFile;
        if (QProcess::startDetached(QStringLiteral("paplay"), args))
            return;
    }

    QApplication::beep();
#else
    QApplication::beep();
#endif
}

void SettingsWindow::toggleMicTest()
{
    if (m_micTestThread)
    {
        stopMicTest();
        return;
    }

    const QString preferredMic = selectedAudioDeviceValue(m_audioMicDeviceCombo);
    if (preferredMic.compare(kAudioDeviceNoneValue, Qt::CaseInsensitive) == 0)
        return;

    m_micTestThread = new QThread(this);
    m_micTestWorker = new AudioCaptureWorker(AudioCaptureWorker::CaptureSource::Microphone, preferredMic);
    m_micTestWorker->moveToThread(m_micTestThread);

    connect(m_micTestThread, &QThread::started, m_micTestWorker, &AudioCaptureWorker::start);
    connect(m_micTestWorker, &AudioCaptureWorker::inputLevelUpdated,
            this, &SettingsWindow::onMicTestLevel, Qt::QueuedConnection);
    connect(m_micTestWorker, &AudioCaptureWorker::stopped, this, &SettingsWindow::onMicTestStopped, Qt::QueuedConnection);
    connect(m_micTestWorker, &AudioCaptureWorker::stopped, m_micTestThread, &QThread::quit, Qt::QueuedConnection);
    connect(m_micTestThread, &QThread::finished, m_micTestWorker, &QObject::deleteLater);
    connect(m_micTestThread, &QThread::finished, m_micTestThread, &QObject::deleteLater);
    connect(m_micTestThread, &QThread::finished, this, [this]() {
        m_micTestWorker = nullptr;
        m_micTestThread = nullptr;
        m_testMicBtn->setEnabled(true);
        m_testMicBtn->setText(tr("Test"));
        if (m_micLevelBar)
            m_micLevelBar->setValue(0);
    });

    m_testMicBtn->setText(tr("Stop"));
    m_micLevelBar->setValue(0);
    m_micTestThread->start();
}

void SettingsWindow::stopMicTest()
{
    if (!m_micTestThread)
        return;

    // 关键修复：stop() 直接写原子标志，线程安全。
    // 若使用 QueuedConnection，runCaptureLoop 占用线程时 stop 可能得不到执行，导致卡死。
    if (m_micTestWorker)
        m_micTestWorker->stop();

    // 非阻塞等待，避免在 UI 线程 wait() 导致界面卡死。
    m_testMicBtn->setEnabled(false);
    m_testMicBtn->setText(tr("Stopping..."));
    if (m_micTestThread->isRunning())
        m_micTestThread->quit();

    QTimer::singleShot(1500, this, [this]() {
        if (m_micTestThread && m_micTestThread->isRunning())
        {
            LOG_WARN("Mic test thread still running after timeout, forcing terminate");
            m_micTestThread->terminate();
            m_micTestThread->wait();
        }
    });
}

void SettingsWindow::onMicTestLevel(float normalizedLevel)
{
    const int percent = qBound(0, static_cast<int>(normalizedLevel * 100.0f), 100);
    m_micLevelBar->setValue(percent);
}

void SettingsWindow::onMicTestStopped()
{
    m_testMicBtn->setEnabled(true);
    m_testMicBtn->setText(tr("Test"));
    QTimer::singleShot(150, this, [this]() {
        if (m_micLevelBar)
            m_micLevelBar->setValue(0);
    });
}

bool SettingsWindow::installService(QString *errorMessage) const
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString binPath = QStringLiteral("\"%1\" --service --no-ui").arg(exe);
    QStringList args{
        QStringLiteral("create"),
        QString::fromUtf8(kServiceName),
        QStringLiteral("binPath="), binPath,
        QStringLiteral("start="), QStringLiteral("auto"),
        QStringLiteral("DisplayName="), QString::fromUtf8(kServiceDisplayName)};
    if (!runProcess(QStringLiteral("sc.exe"), args, errorMessage))
        return false;
    QString startError;
    runProcess(QStringLiteral("sc.exe"), {QStringLiteral("start"), QString::fromUtf8(kServiceName)}, &startError);
    return true;
#else
    Q_UNUSED(errorMessage);
    return false;
#endif
}

bool SettingsWindow::uninstallService(QString *errorMessage) const
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    QString stopError;
    runProcess(QStringLiteral("sc.exe"), {QStringLiteral("stop"), QString::fromUtf8(kServiceName)}, &stopError);
    return runProcess(QStringLiteral("sc.exe"), {QStringLiteral("delete"), QString::fromUtf8(kServiceName)}, errorMessage);
#else
    Q_UNUSED(errorMessage);
    return false;
#endif
}
