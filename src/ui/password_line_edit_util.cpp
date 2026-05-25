#include "password_line_edit_util.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>
#include <QVariant>

namespace
{
    constexpr int kIconSize = 14;
    constexpr int kIconCanvasSize = 18;
    constexpr const char *kPasswordHiddenFontProperty = "_airanPasswordHiddenFont";
    constexpr const char *kPasswordVisibleFontProperty = "_airanPasswordVisibleFont";

    class PasswordLineEditFilter : public QObject
    {
    public:
        explicit PasswordLineEditFilter(QLineEdit *lineEdit, QToolButton *eyeButton, QToolButton *clearButton)
            : QObject(lineEdit), m_lineEdit(lineEdit), m_eyeButton(eyeButton), m_clearButton(clearButton)
        {
        }

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            if (watched == m_lineEdit &&
                (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::StyleChange))
                polishEmbeddedButtons();
            return QObject::eventFilter(watched, event);
        }

    private:
        void polishEmbeddedButtons()
        {
            if (!m_lineEdit || !m_eyeButton)
                return;

            for (QToolButton *button : {m_eyeButton, m_clearButton})
            {
                if (!button)
                    continue;

                button->setCursor(Qt::ArrowCursor);
                button->setIconSize(QSize(kIconSize, kIconSize));
                button->setFixedSize(22, 22);
                button->setStyleSheet(QStringLiteral(
                    "QToolButton {"
                    "    background: transparent;"
                    "    border: none;"
                    "    padding: 0;"
                    "    margin: 0;"
                    "}"
                    "QToolButton:hover {"
                    "    background: rgba(131, 193, 224, 0.14);"
                    "    border-radius: 11px;"
                    "}"));
            }

            const int top = qMax(0, (m_lineEdit->height() - 22) / 2);
            if (m_clearButton)
            {
                const int clearX = m_lineEdit->width() - 6 - m_clearButton->width() + 2;
                const int eyeX = clearX - 4 - m_eyeButton->width();
                m_clearButton->move(clearX, top);
                m_eyeButton->move(eyeX, top);
                m_clearButton->raise();
                m_eyeButton->raise();
            }
            else
            {
                const int eyeX = m_lineEdit->width() - 6 - m_eyeButton->width() + 2;
                m_eyeButton->move(eyeX, top);
                m_eyeButton->raise();
            }

            const int reserved = (m_clearButton ? 58 : 32);
            QMargins margins = m_lineEdit->textMargins();
            if (margins.right() != reserved)
            {
                margins.setRight(reserved);
                m_lineEdit->setTextMargins(margins);
            }
        }

        QLineEdit *m_lineEdit{nullptr};
        QToolButton *m_eyeButton{nullptr};
        QToolButton *m_clearButton{nullptr};
    };

    QIcon makeEyeIcon(bool visible)
    {
        QPixmap pixmap(kIconCanvasSize, kIconCanvasSize);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor color(131, 193, 224);
        QPen pen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        QPainterPath eyePath;
        eyePath.moveTo(2.5, 9.0);
        eyePath.cubicTo(5.0, 4.8, 13.0, 4.8, 15.5, 9.0);
        eyePath.cubicTo(13.0, 13.2, 5.0, 13.2, 2.5, 9.0);
        painter.drawPath(eyePath);
        painter.drawEllipse(QPointF(9.0, 9.0), 2.2, 2.2);

        if (!visible)
            painter.drawLine(QPointF(3.5, 14.0), QPointF(14.5, 4.0));

        return QIcon(pixmap);
    }

    void setPasswordStyleActive(QLineEdit *lineEdit, bool active)
    {
        if (!lineEdit)
            return;

        lineEdit->setProperty("passwordField", active);
        lineEdit->style()->unpolish(lineEdit);
        lineEdit->style()->polish(lineEdit);
        lineEdit->update();
    }

    QFont passwordFontProperty(QLineEdit *lineEdit, const char *name, const QFont &fallback)
    {
        const QVariant value = lineEdit ? lineEdit->property(name) : QVariant();
        return value.canConvert<QFont>() ? value.value<QFont>() : fallback;
    }

    void applyPasswordRevealState(QLineEdit *lineEdit, QToolButton *eyeButton, bool show)
    {
        if (!lineEdit || !eyeButton)
            return;

        lineEdit->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
        setPasswordStyleActive(lineEdit, !show);
        lineEdit->setFont(passwordFontProperty(lineEdit,
                                               show ? kPasswordVisibleFontProperty : kPasswordHiddenFontProperty,
                                               lineEdit->font()));
        eyeButton->setIcon(makeEyeIcon(show));
        eyeButton->setToolTip(show ? QLineEdit::tr("Hide password")
                                   : QLineEdit::tr("Show password"));
    }
}

void installPasswordRevealButton(QLineEdit *lineEdit, bool showClearButton)
{
    if (!lineEdit)
        return;

    lineEdit->setClearButtonEnabled(false);
    lineEdit->setEchoMode(QLineEdit::Password);
    if (!lineEdit->property(kPasswordHiddenFontProperty).isValid())
        lineEdit->setProperty(kPasswordHiddenFontProperty, lineEdit->font());
    if (!lineEdit->property(kPasswordVisibleFontProperty).isValid())
        lineEdit->setProperty(kPasswordVisibleFontProperty, lineEdit->font());
    setPasswordStyleActive(lineEdit, true);

    auto *eyeButton = new QToolButton(lineEdit);
    eyeButton->setIcon(makeEyeIcon(false));
    eyeButton->setToolTip(QLineEdit::tr("Show password"));
    eyeButton->setFocusPolicy(Qt::NoFocus);
    eyeButton->setCursor(Qt::ArrowCursor);
    eyeButton->show();

    QToolButton *clearButton = nullptr;
    if (showClearButton)
    {
        clearButton = new QToolButton(lineEdit);
        clearButton->setIcon(lineEdit->style()->standardIcon(QStyle::SP_LineEditClearButton));
        clearButton->setToolTip(QLineEdit::tr("Clear"));
        clearButton->setFocusPolicy(Qt::NoFocus);
        clearButton->setCursor(Qt::ArrowCursor);
        clearButton->show();
        QObject::connect(clearButton, &QToolButton::clicked, lineEdit, &QLineEdit::clear);
    }

    auto *filter = new PasswordLineEditFilter(lineEdit, eyeButton, clearButton);
    lineEdit->installEventFilter(filter);
    QEvent showEvent(QEvent::Show);
    QCoreApplication::sendEvent(lineEdit, &showEvent);

    QObject::connect(eyeButton, &QToolButton::clicked, lineEdit, [lineEdit, eyeButton](bool)
                     {
        const bool show = lineEdit->echoMode() == QLineEdit::Password;
        applyPasswordRevealState(lineEdit, eyeButton, show); });

    if (clearButton)
    {
        QObject::connect(lineEdit, &QLineEdit::textChanged, clearButton, [clearButton](const QString &text)
                         { clearButton->setVisible(!text.isEmpty()); });
        clearButton->setVisible(!lineEdit->text().isEmpty());
    }

    setPasswordStyleActive(lineEdit, true);
    QCoreApplication::sendEvent(lineEdit, &showEvent);
}

void setPasswordRevealFonts(QLineEdit *lineEdit, const QFont &hiddenFont, const QFont &visibleFont)
{
    if (!lineEdit)
        return;

    lineEdit->setProperty(kPasswordHiddenFontProperty, hiddenFont);
    lineEdit->setProperty(kPasswordVisibleFontProperty, visibleFont);
    lineEdit->setFont(lineEdit->echoMode() == QLineEdit::Normal ? visibleFont : hiddenFont);
}
