#include "i18n_util.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QSet>
#include <QTranslator>

namespace
{
QString cleanPath(const QString &path)
{
    return QDir::cleanPath(path);
}
}

namespace I18nUtil
{
QString autoLanguageKey()
{
    return QStringLiteral("auto");
}

QString zhCnLanguageKey()
{
    return QStringLiteral("zh_CN");
}

QString enUsLanguageKey()
{
    return QStringLiteral("en_US");
}

QStringList supportedUiLanguages()
{
    return {autoLanguageKey(), zhCnLanguageKey(), enUsLanguageKey()};
}

QString normalizeUiLanguage(const QString &language)
{
    const QString normalized = language.trimmed();
    return supportedUiLanguages().contains(normalized) ? normalized : autoLanguageKey();
}

QString resolveUiLanguage(const QString &configuredLanguage)
{
    const QString normalized = normalizeUiLanguage(configuredLanguage);
    if (normalized != autoLanguageKey())
        return normalized;

    const QLocale systemLocale;
    return systemLocale.language() == QLocale::Chinese ? zhCnLanguageKey() : enUsLanguageKey();
}

QStringList translationSearchPaths()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        cleanPath(appDir + QStringLiteral("/locale")),
        cleanPath(appDir + QStringLiteral("/../share/airan-desk/locale")),
        cleanPath(appDir + QStringLiteral("/../Resources/locale")),
        QStringLiteral(":/locale")};

    QStringList paths;
    QSet<QString> seen;
    for (const QString &path : candidates)
    {
        if (!seen.contains(path))
        {
            paths << path;
            seen.insert(path);
        }
    }
    return paths;
}

bool installTranslator(QCoreApplication &app, QTranslator &translator, const QString &baseName, const QString &localeName)
{
    const QString qmName = baseName + localeName + QStringLiteral(".qm");
    for (const QString &path : translationSearchPaths())
    {
        if (translator.load(qmName, path))
        {
            app.installTranslator(&translator);
            return true;
        }
    }
    return false;
}
}
