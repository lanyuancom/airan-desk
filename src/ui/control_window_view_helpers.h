#ifndef CONTROL_WINDOW_VIEW_HELPERS_H
#define CONTROL_WINDOW_VIEW_HELPERS_H

#include <QFontMetrics>
#include <QPushButton>
#include <QSize>
#include <QStyle>
#include <QStyleOption>

inline int controlWindowTextWidth(const QFontMetrics &metrics, const QString &text)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    return metrics.horizontalAdvance(text);
#else
    return metrics.width(text);
#endif
}

inline void fitControlButtonWidthToText(QPushButton *button)
{
    if (!button)
        return;

    button->ensurePolished();

    QFontMetrics metrics(button->font());
    QStyleOptionButton option;
    option.initFrom(button);
    option.text = button->text();
    option.icon = button->icon();
    option.iconSize = button->iconSize();

    QSize contentSize(controlWindowTextWidth(metrics, button->text()), metrics.height());
    const int width = button->style()->sizeFromContents(QStyle::CT_PushButton, &option, contentSize, button).width();
    button->setFixedWidth(width);
}

#endif /* CONTROL_WINDOW_VIEW_HELPERS_H */
