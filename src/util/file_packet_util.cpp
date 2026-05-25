/* Split from file_packet_util.cpp by packet transfer responsibility. */

#include "file_packet_util.h"

FilePacketUtil::FilePacketUtil(QObject *parent)
    : QObject(parent)
{
}

FilePacketUtil::~FilePacketUtil()
{
    QMutexLocker locker(&m_reassemblyMutex);

    /* 清理所有临时文件和文件对象 */
    for (auto& pair : m_reassemblyBuffers) {
        if (pair.second.tempFile) {
            pair.second.tempFile->close();
            delete pair.second.tempFile;
        }
        if (!pair.second.tempFilePath.isEmpty()) {
            QFile::remove(pair.second.tempFilePath);
        }
    }

    m_reassemblyBuffers.clear();
}
