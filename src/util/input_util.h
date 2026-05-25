#ifndef INPUT_UTIL_H
#define INPUT_UTIL_H

#include <QObject>

class InputUtil : public QObject
{
    Q_OBJECT
public:
    explicit InputUtil(QObject *parent = nullptr);
    static void execMouseEvent(int button, qreal x_n, qreal y_n,int mouseData,const QString& dwFlags);
    static void execKeyboardEvent(int keyCode,const QString& dwFlags);
    static void execKeyboardText(const QString &text);
    static bool execRemoteOperation(const QString &action, QString *errorMessage = nullptr);
    static bool execAndroidNavigation(const QString &action, QString *errorMessage = nullptr);
    static bool runProgram(const QString &path, QString *errorMessage = nullptr);
signals:
};

#endif /* INPUT_UTIL_H */
