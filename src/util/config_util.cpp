#include "config_util.h"
#include "util/i18n_util.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QtGlobal>

namespace
{
QString appConfigDir()
{
#if defined(Q_OS_LINUX)
    const QString homeDir = QDir::homePath();
    if (!homeDir.isEmpty())
    {
        return QDir(homeDir).filePath(QStringLiteral(".wxalh/airan-desk/conf"));
    }
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/conf");
#endif

    return QCoreApplication::applicationDirPath() + QStringLiteral("/conf");
}

void configureDefaultSettingsPath()
{
#if defined(Q_OS_LINUX)
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, appConfigDir());
    QSettings::setDefaultFormat(QSettings::IniFormat);
#endif
}

QString runtimeConfFile(const QString &fileName)
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/conf/") + fileName;
}

QStringList defaultConfCandidates(const QString &fileName)
{
    QStringList candidates;

#if defined(Q_OS_LINUX)
#ifdef AIRAN_DESK_INSTALL_DATADIR
    candidates << QStringLiteral(AIRAN_DESK_INSTALL_DATADIR) + QStringLiteral("/conf/") + fileName;
#endif
    const QString located = QStandardPaths::locate(
        QStandardPaths::GenericDataLocation,
        QStringLiteral("airan-desk/conf/") + fileName);
    if (!located.isEmpty())
    {
        candidates << located;
    }
    candidates << QStringLiteral("/usr/local/share/airan-desk/conf/") + fileName;
    candidates << QStringLiteral("/usr/share/airan-desk/conf/") + fileName;
#endif

    candidates << runtimeConfFile(fileName);
    candidates << QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../../../../resource/conf/") + fileName);

    candidates.removeDuplicates();
    return candidates;
}

QString writableConfFile(const QString &fileName, bool copyDefault)
{
    const QString dirPath = appConfigDir();
    QDir dir(dirPath);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
        SPDLOG_WARN("Failed to create config directory: {}", dirPath.toStdString());
    }

    const QString targetPath = dir.filePath(fileName);
    if (copyDefault && !QFile::exists(targetPath))
    {
        const QStringList candidates = defaultConfCandidates(fileName);
        for (const QString &candidate : candidates)
        {
            if (candidate == targetPath || !QFile::exists(candidate))
            {
                continue;
            }
            if (QFile::copy(candidate, targetPath))
            {
                QFile::setPermissions(targetPath,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                    QFileDevice::ReadUser | QFileDevice::WriteUser |
                    QFileDevice::ReadGroup | QFileDevice::ReadOther);
                SPDLOG_INFO("Copied default config from {} to {}",
                    candidate.toStdString(), targetPath.toStdString());
                break;
            }
            SPDLOG_WARN("Failed to copy default config from {} to {}",
                candidate.toStdString(), targetPath.toStdString());
        }
    }

    return targetPath;
}
}

ConfigUtilData::ConfigUtilData(QObject *parent)
    : QObject{parent}
{
    configureDefaultSettingsPath();
    initCommonIni();
    initIdIni();
    SPDLOG_INFO("local control code initialized: {}", local_id.toStdString());
}

ConfigUtilData::~ConfigUtilData()
{
    /* Ensure QSettings are cleaned up */
    if (m_idIni) {
        try { m_idIni->sync(); } catch(...) {}
        delete m_idIni;
        m_idIni = nullptr;
    }
    if (m_commonIni) {
        try { m_commonIni->sync(); } catch(...) {}
        delete m_commonIni;
        m_commonIni = nullptr;
    }
}

ConfigUtilData *ConfigUtilData::getInstance()
{
    static ConfigUtilData configUtil;
    return &configUtil;
}

QString ConfigUtilData::getOrCreateUuid()
{
    /* 璁剧疆缁勭粐鍚嶅拰搴旂敤鍚嶏紙纭畾瀛樺偍璺緞锛? */
    QCoreApplication::setOrganizationName("wxalh.com");
    QCoreApplication::setApplicationName("airan");
    QSettings settings; /* 鑷姩閫夋嫨绯荤粺榛樿浣嶇疆 */

    /* 灏濊瘯璇诲彇瀛樺偍鐨刄UID */
    QString uuidKey = "Global/Uuid";
    QString storedUuid = settings.value(uuidKey).toString().toUpper();

    /* 妫€鏌UID鏄惁鏈夋晥锛堥潪绌轰笖绗﹀悎鏍煎紡锛? */
    QUuid uuid(storedUuid);
    if (!storedUuid.isEmpty() && !uuid.isNull())
    {
        return storedUuid;
    }

    /* 鐢熸垚鏂扮殑UUID骞跺瓨鍌? */
    QUuid newUuid = QUuid::createUuid();
    QString newUuidStr = newUuid.toString().remove("{").remove("}").toUpper(); /* 绉婚櫎鑺辨嫭鍙? */
    settings.setValue(uuidKey, newUuidStr);
    settings.sync(); /* 寮哄埗鍐欏叆纾佺洏 */
    return newUuidStr;
}

QString ConfigUtilData::getOrCreateInstallId()
{
    QCoreApplication::setOrganizationName("wxalh.com");
    QCoreApplication::setApplicationName("airan");
    QSettings settings;

    const QString installIdKey = "Global/InstallId";
    QString storedInstallId = settings.value(installIdKey).toString().toUpper();

    QUuid uuid(storedInstallId);
    if (!storedInstallId.isEmpty() && !uuid.isNull())
    {
        return storedInstallId;
    }

    const QString newInstallId = QUuid::createUuid().toString().remove("{").remove("}").toUpper();
    settings.setValue(installIdKey, newInstallId);
    settings.sync();
    return newInstallId;
}

void ConfigUtilData::saveIdIni()
{
    local_id = local_id.trimmed();
    local_pwd = local_pwd.trimmed();
    m_idIni->beginGroup("local");
    m_idIni->setValue("local_id", local_id);
    m_idIni->setValue("local_pwd", local_pwd);
    m_idIni->endGroup();
    m_idIni->sync();
}

void ConfigUtilData::replaceLocalId(const QString &localId)
{
    const QString normalized = localId.trimmed().remove("{").remove("}").toUpper();
    if (normalized.isEmpty() || QUuid(normalized).isNull())
    {
        SPDLOG_WARN("Ignoring invalid local id replacement: {}", localId.toStdString());
        return;
    }

    local_id = normalized;

    QCoreApplication::setOrganizationName("wxalh.com");
    QCoreApplication::setApplicationName("airan");
    QSettings settings;
    settings.setValue("Global/Uuid", local_id);
    settings.sync();

    saveIdIni();
}

void ConfigUtilData::saveCommonIni()
{
    m_commonIni->beginGroup("local");
    m_commonIni->setValue("showUI", showUI);
    m_commonIni->setValue("logLevel", logLevelStr);
    language = I18nUtil::normalizeUiLanguage(language);
    m_commonIni->setValue("language", language);
    m_commonIni->endGroup();

    m_commonIni->beginGroup("remote");
    m_commonIni->setValue("fps", fps);
    m_commonIni->setValue("quality", remote_quality);
    m_commonIni->setValue("bitrateProfile", remote_bitrate_profile);
    m_commonIni->setValue("networkPath", remote_network_path);
    m_commonIni->setValue("captureBackend", remote_capture_backend);
    m_commonIni->setValue("width", remote_width);
    m_commonIni->setValue("height", remote_height);
    m_commonIni->endGroup();

    m_commonIni->beginGroup("signal_server");
    m_commonIni->setValue("wsUrl", wsUrl);
    m_commonIni->endGroup();

    m_commonIni->beginGroup("ice_server");
    m_commonIni->setValue("host", ice_host);
    m_commonIni->setValue("port", ice_port);
    m_commonIni->setValue("username", ice_username);
    m_commonIni->setValue("password", ice_password);
    m_commonIni->endGroup();

    m_commonIni->beginGroup("audio");
    m_commonIni->setValue("micDevice", audio_mic_device);
    m_commonIni->setValue("loopbackDevice", audio_loopback_device);
    m_commonIni->endGroup();

    m_commonIni->sync();
}

void ConfigUtilData::saveCommonConfig()
{
    saveCommonIni();
}

void ConfigUtilData::initIdIni()
{
    idFilePath = writableConfFile(QStringLiteral("id.ini"), false);
    m_idIni = new QSettings(idFilePath, QSettings::IniFormat);
    m_idIni->setIniCodec("UTF-8");

    m_idIni->beginGroup("local");
    local_id = getOrCreateUuid().trimmed();
    install_id = getOrCreateInstallId().trimmed();
    local_pwd = m_idIni->value("local_pwd", "").toString().trimmed();
    m_idIni->endGroup();

    if (local_pwd.isEmpty() || QUuid(local_pwd).isNull())
    {
        local_pwd = QUuid::createUuid().toString().remove("{").remove("}").toUpper();
    }

    setLocalPwd(local_pwd);
}

void ConfigUtilData::initCommonIni()
{
    commonFilePath = writableConfFile(QStringLiteral("common.ini"), true);
    QFile fileCheck(commonFilePath);
    bool fileExists = fileCheck.exists();

    m_commonIni = new QSettings(commonFilePath, QSettings::IniFormat);
    m_commonIni->setIniCodec("UTF-8");

    m_commonIni->beginGroup("local");
    showUI = m_commonIni->value("showUI", true).toBool();
    logLevelStr = m_commonIni->value("logLevel", "info").toString();
    language = m_commonIni->value("language", "auto").toString().trimmed();
    m_commonIni->endGroup();

    language = I18nUtil::normalizeUiLanguage(language);

    m_commonIni->beginGroup("remote");
    fps = m_commonIni->value("fps", 25).toInt(); /* 榛樿甯х巼25 */
    remote_quality = m_commonIni->value("quality", "smooth").toString().trimmed().toLower();
    remote_bitrate_profile = m_commonIni->value("bitrateProfile", "medium").toString().trimmed().toLower();
    remote_network_path = m_commonIni->value("networkPath", "auto").toString().trimmed().toLower();
    remote_capture_backend = m_commonIni->value("captureBackend", "auto").toString().trimmed().toLower();
    remote_width = m_commonIni->value("width", 0).toInt();
    remote_height = m_commonIni->value("height", 0).toInt();
    m_commonIni->endGroup();

    if (fps < 1 || fps > 60)
    {
        fps = 25;
    }
    if (remote_quality != "quality" && remote_quality != "smooth" && remote_quality != "compat")
    {
        remote_quality = "smooth";
    }
    if (remote_bitrate_profile != "low" && remote_bitrate_profile != "medium" && remote_bitrate_profile != "high")
    {
        remote_bitrate_profile = "medium";
    }
    if (remote_network_path != "auto" && remote_network_path != "direct" &&
        remote_network_path != "turn_udp" && remote_network_path != "turn_tcp")
    {
        remote_network_path = "auto";
    }
    if (remote_capture_backend != "auto" && remote_capture_backend != "wgc" &&
        remote_capture_backend != "qt" && remote_capture_backend != "pipewire")
    {
        remote_capture_backend = "auto";
    }
    if (remote_width < 0 || remote_height < 0)
    {
        remote_width = 0;
        remote_height = 0;
    }
    m_commonIni->beginGroup("signal_server");
    wsUrl = m_commonIni->value("wsUrl", "").toString();
    m_commonIni->endGroup();

    m_commonIni->beginGroup("ice_server");
    ice_host = m_commonIni->value("host", "stun.l.google.com").toString();
    ice_port = (uint16_t)(m_commonIni->value("port", 19302).toUInt());
    ice_username = m_commonIni->value("username", "").toString();
    ice_password = m_commonIni->value("password", "").toString();
    m_commonIni->endGroup();

    m_commonIni->beginGroup("audio");
    audio_mic_device = m_commonIni->value("micDevice", "").toString().trimmed();
    audio_loopback_device = m_commonIni->value("loopbackDevice", "").toString().trimmed();
    m_commonIni->endGroup();

    if (logLevelStr == "trace")
    {
        logLevel = spdlog::level::trace;
    }
    else if (logLevelStr == "debug")
    {
        logLevel = spdlog::level::debug;
    }
    else if (logLevelStr == "info")
    {
        logLevel = spdlog::level::info;
    }
    else if (logLevelStr == "warn")
    {
        logLevel = spdlog::level::warn;
    }
    else if (logLevelStr == "error")
    {
        logLevel = spdlog::level::err;
    }
    else if (logLevelStr == "critical")
    {
        logLevel = spdlog::level::critical;
    }
    else
    {
        logLevel = spdlog::level::info; /* 榛樿绾у埆 */
    }

    if (fileExists)
    {
        m_commonIni->sync();
    }
}

void ConfigUtilData::setLocalPwd(const QString &pwd)
{
    this->local_pwd = pwd.trimmed();
    QByteArray hashResult = QCryptographicHash::hash(local_pwd.toUtf8(), QCryptographicHash::Md5);
    this->local_pwd_md5 = hashResult.toHex().toUpper();
    saveIdIni();
}

QString ConfigUtilData::getLocalPwd()
{
    return local_pwd;
}
