#ifndef SETTINGS_WINDOW_H
#define SETTINGS_WINDOW_H

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QLabel;
class QShowEvent;
class QProgressBar;
class QPushButton;
class QThread;

class AudioCaptureWorker;

namespace Ui
{
    class SettingsWindow;
}

class SettingsWindow : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget *parent = nullptr);
    ~SettingsWindow() override;

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void saveSettings();
    void refreshServiceState();
    void onServiceInstallToggled(bool checked);
    void refreshAudioDevices();
    void testSpeaker();
    void toggleMicTest();
    void onMicTestLevel(float normalizedLevel);
    void onMicTestStopped();

private:
    void centerOnParent();
    void bindUiObjects();
    void populateSettingControls();
    bool isServiceInstalled() const;
    bool installService(QString *errorMessage) const;
    bool uninstallService(QString *errorMessage) const;
    bool runProcess(const QString &program, const QStringList &arguments, QString *errorMessage) const;
    QString selectedAudioDeviceValue(QComboBox *combo) const;
    void stopMicTest();

    QCheckBox *m_showUiCheck{nullptr};
    QComboBox *m_logLevelCombo{nullptr};
    QComboBox *m_languageCombo{nullptr};
    QLineEdit *m_wsUrlEdit{nullptr};
    QLineEdit *m_iceHostEdit{nullptr};
    QSpinBox *m_icePortSpin{nullptr};
    QLineEdit *m_iceUserEdit{nullptr};
    QLineEdit *m_icePasswordEdit{nullptr};
    QComboBox *m_audioMicDeviceCombo{nullptr};
    QComboBox *m_audioLoopbackDeviceCombo{nullptr};
    QPushButton *m_refreshAudioDevicesBtn{nullptr};
    QPushButton *m_testSpeakerBtn{nullptr};
    QPushButton *m_testMicBtn{nullptr};
    QProgressBar *m_micLevelBar{nullptr};

    QSpinBox *m_fpsSpin{nullptr};
    QComboBox *m_qualityCombo{nullptr};
    QComboBox *m_bitrateCombo{nullptr};
    QComboBox *m_networkCombo{nullptr};
    QComboBox *m_captureCombo{nullptr};
    QComboBox *m_resolutionCombo{nullptr};

    QCheckBox *m_serviceCheck{nullptr};
    QLabel *m_serviceStatusLabel{nullptr};
    bool m_refreshingService{false};
    bool m_centeredOnFirstShow{false};
    QThread *m_micTestThread{nullptr};
    AudioCaptureWorker *m_micTestWorker{nullptr};
    Ui::SettingsWindow *ui{nullptr};
};

#endif /* SETTINGS_WINDOW_H */
