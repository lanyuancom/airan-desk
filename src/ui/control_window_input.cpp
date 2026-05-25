#include "control_window.h"

#include "util/config_util.h"
#include "util/json_util.h"
#include "util/key_util.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QWidget>

void ControlWindow::keyPressEvent(QKeyEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    /* 发送给远端 */
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_KEYBOARD)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_KEY, KeyUtil::qtKeyToWinKey(event->key()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_DOWN)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    /* LOG_DEBUG(msg); */

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    /* 发送给远端 */
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_KEYBOARD)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_KEY, KeyUtil::qtKeyToWinKey(event->key()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_UP)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    /* LOG_DEBUG(msg); */

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    /* 发送给远端 */
    QPointF pos = getNormPoint(event->pos());
    if (!isValidNormPoint(pos))
        return;
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_BUTTON, static_cast<int>(event->button()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_DOUBLECLICK)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    /* LOG_DEBUG(msg); */

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::wheelEvent(QWheelEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    /* 发送给远端 */
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QPointF pos = getNormPoint(event->position().toPoint());
#else
    QPointF pos = getNormPoint(event->pos());
#endif
    if (!isValidNormPoint(pos))
        return;
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_WHEEL)
                          .add(Constant::KEY_MOUSEDATA, event->angleDelta().y())
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    /* LOG_DEBUG(msg); */

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}


void ControlWindow::mousePressEvent(QMouseEvent *event)
{
    /* 检查是否点击在工具栏上 */
    if (m_floatingToolbar)
    {
        const QRect toolbarGlobalRect(m_floatingToolbar->mapToGlobal(QPoint(0, 0)), m_floatingToolbar->size());
        if (toolbarGlobalRect.contains(event->globalPos()))
        {
            m_draggingToolbar = true;
            m_dragStartPosition = event->globalPos();
            m_toolbarOffset = event->globalPos() - m_floatingToolbar->mapToGlobal(QPoint(0, 0));
            event->accept();
            return;
        }
    }

    if (!isReceivedImg)
    {
        return;
    }

    /* 原有的鼠标处理逻辑 */
    QPointF pos = getNormPoint(event->pos());
    if (!isValidNormPoint(pos))
        return;
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_BUTTON, static_cast<int>(event->button()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_DOWN)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingToolbar && m_floatingToolbar)
    {
        QWidget *toolbarParent = m_floatingToolbar->parentWidget();
        if (!toolbarParent)
            toolbarParent = this;

        /* 拖拽工具栏，并限制在当前父容器内。 */
        QPoint newPos = toolbarParent->mapFromGlobal(event->globalPos() - m_toolbarOffset);

        int maxX = toolbarParent->width() - m_floatingToolbar->width();
        int maxY = toolbarParent->height() - m_floatingToolbar->height();

        newPos.setX(qMax(0, qMin(newPos.x(), maxX)));
        newPos.setY(qMax(0, qMin(newPos.y(), maxY)));
        m_floatingToolbar->move(newPos);
        m_toolbarUserMoved = true;
        event->accept();
        return;
    }

    if (!isReceivedImg)
    {
        return;
    }

    /* 原有的鼠标移动处理逻辑 */
    QPointF pos = getNormPoint(event->pos());
    if (!isValidNormPoint(pos))
        return;
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_MOVE)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_draggingToolbar)
    {
        m_draggingToolbar = false;
        m_toolbarUserMoved = true;
        event->accept();
        return;
    }

    if (!isReceivedImg)
    {
        return;
    }

    /* 原有的鼠标释放处理逻辑 */
    QPointF pos = getNormPoint(event->pos());
    if (!isValidNormPoint(pos))
        return;
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_BUTTON, static_cast<int>(event->button()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_UP)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

/* 工具栏按钮槽函数实现 */
