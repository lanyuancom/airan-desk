#ifndef NATIVE_TERMINAL_WIDGET_H
#define NATIVE_TERMINAL_WIDGET_H

#include <QBasicTimer>
#include <QFont>
#include <QPoint>
#include <QVariant>
#include <QVector>
#include <QWidget>

extern "C"
{
#ifdef small
#undef small
#endif
#include <vterm.h>
}

class NativeTerminalWidget : public QWidget
{
    Q_OBJECT
public:
    explicit NativeTerminalWidget(QWidget *parent = nullptr);
    ~NativeTerminalWidget();

    QSize gridSize() const;
    void setLocalEchoEnabled(bool enabled);

public slots:
    void writePtyOutput(const QByteArray &data);
    void showStatusLine(const QString &message);

signals:
    void inputGenerated(const QByteArray &data);
    void gridSizeChanged(const QSize &size);
    void terminalTitleChanged(const QString &title);
    void currentDirectoryChanged(const QString &path);
    void bell();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void timerEvent(QTimerEvent *event) override;

private:
    static QString preferredTerminalFontFamily();
    static QString pathFromOsc7Payload(const QByteArray &payload);
    static bool isWideCharTrailingCell(const VTermScreenCell &cell);
    static bool isCellBefore(const QPoint &left, const QPoint &right);
    static QString trimTrailingSpaces(QString text);

    void initializeTerminal(int cols, int rows);
    void updateGridFromViewport();
    void sendKey(VTermKey key, VTermModifier modifiers);
    void sendText(const QString &text, VTermModifier modifiers);
    void sendClipboardPaste();
    QByteArray terminalInputBytesFromText(const QString &text) const;
    bool clipboardHasText() const;
    void showContextMenu(const QPoint &globalPos);
    void sendMouseButton(QMouseEvent *event, bool pressed);
    void sendMouseMove(QMouseEvent *event);
    QPoint cellFromPosition(const QPoint &pos) const;
    bool visibleCell(int row, int col, VTermScreenCell *cell) const;
    int visibleGlobalLine(int row) const;
    void scrollHistory(int lines);
    bool shouldStartLocalSelection(QMouseEvent *event) const;
    void beginSelection(const QPoint &cell);
    void updateSelection(const QPoint &cell);
    void finishSelection();
    void clearSelection();
    bool hasSelection() const;
    bool isCellSelected(int row, int col) const;
    bool isCellRangeSelected(int row, int col, int width) const;
    QString selectedText() const;
    void copySelectionToClipboard();
    VTermModifier modifiersFromQt(Qt::KeyboardModifiers modifiers) const;
    VTermModifier mouseModifiersFromQt(Qt::KeyboardModifiers modifiers) const;
    QColor colorFromVTerm(VTermColor color, bool foreground) const;
    QString cellText(const VTermScreenCell &cell) const;
    QByteArray localEchoBytes(const char *data, size_t len) const;
    void flushDamage();
    void handleOutputBytes(const char *data, size_t len);
    void updateCursorBlink();
    void parseOsc7Directory(const QByteArray &data);

    static void outputCallback(const char *data, size_t len, void *user);
    static int damageCallback(VTermRect rect, void *user);
    static int cursorCallback(VTermPos pos, VTermPos oldpos, int visible, void *user);
    static int termPropCallback(VTermProp prop, VTermValue *val, void *user);
    static int bellCallback(void *user);
    static int resizeCallback(int rows, int cols, void *user);
    static int scrollbackPushCallback(int cols, const VTermScreenCell *cells, void *user);
    static int scrollbackPopCallback(int cols, VTermScreenCell *cells, void *user);
    static int scrollbackClearCallback(void *user);

    VTerm *m_vterm = nullptr;
    VTermScreen *m_screen = nullptr;
    QSize m_gridSize = QSize(80, 24);
    QFont m_font;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_ascent = 12;
    VTermPos m_cursorPos{0, 0};
    bool m_cursorVisible = true;
    bool m_cursorBlink = false;
    bool m_cursorBlinkState = true;
    int m_cursorShape = VTERM_PROP_CURSORSHAPE_BLOCK;
    int m_mouseMode = VTERM_PROP_MOUSE_NONE;
    bool m_focusReport = false;
    bool m_localEchoEnabled = false;
    bool m_selecting = false;
    bool m_hasSelection = false;
    QPoint m_selectionAnchor;
    QPoint m_selectionCursor;
    QVector<QVector<VTermScreenCell>> m_scrollback;
    int m_scrollbackOffset = 0;
    QBasicTimer m_blinkTimer;
};

#endif /* NATIVE_TERMINAL_WIDGET_H */
