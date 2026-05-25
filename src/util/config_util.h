#ifndef CONFIG_UTIL_H
#define CONFIG_UTIL_H

#include <QObject>
#include <QSettings>
#include <QUuid>
#include <spdlog/spdlog.h>

#define ConfigUtil ConfigUtilData::getInstance()

class ConfigUtilData : public QObject
{
    Q_OBJECT
private:
    QString getOrCreateUuid();
    QString getOrCreateInstallId();
    void saveIdIni();
    void saveCommonIni();
    void initIdIni();
    void initCommonIni();
public:
    explicit ConfigUtilData(QObject *parent = nullptr);
    ~ConfigUtilData();
    static ConfigUtilData *getInstance();
    void setLocalPwd(const QString &pwd);
    void replaceLocalId(const QString &localId);
    QString getLocalPwd();
    void saveCommonConfig();

public:
    QString commonFilePath;
    QSettings *m_commonIni;
    QString idFilePath;
    QSettings *m_idIni;
    /* 帧率 */
    int fps;
    /* 是否显示UI */
    bool showUI;
    /* UI language: auto, zh_CN, en_US */
    QString language;
    /* 本机sn码 */
    QString local_id;
    QString install_id;
    QString local_pwd_md5;
    /* websocket服务器 */
    QString wsUrl;

    QString ice_host;
    uint16_t ice_port;
    QString ice_username;
    QString ice_password;
    QString remote_quality;
    QString remote_bitrate_profile;
    QString remote_network_path;
    QString remote_capture_backend;
    int remote_width;
    int remote_height;
    QString audio_mic_device;
    QString audio_loopback_device;
    spdlog::level::level_enum logLevel;
    QString logLevelStr;

private:
    /* 本机访问密码 */
    QString local_pwd;
signals:
};

#endif /* CONFIG_UTIL_H */
