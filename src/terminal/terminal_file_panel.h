#ifndef TERMINAL_FILE_PANEL_H
#define TERMINAL_FILE_PANEL_H

#include <QIcon>
#include <QJsonArray>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QTableWidget;
class QTableWidgetItem;
class QToolButton;

class TerminalFilePanel : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalFilePanel(QWidget *parent = nullptr);

    QString currentRemotePath() const;

public slots:
    void setConnected(bool connected);
    void setRemotePath(const QString &path);
    void followTerminalPath(const QString &path);
    void recvGetFileList(const QJsonObject &object);
    void recvDownloadFile(bool status, const QString &filePath);
    void recvUploadFile(bool status, const QString &filePath);

signals:
    void requestFileList(const QString &path);
    void requestDownload(const QString &remotePath, const QString &localPath, bool isDirectory);
    void requestUpload(const QString &localPath, const QString &remotePath, bool isDirectory);

private slots:
    void onPathEditingFinished();
    void onParentClicked();
    void onRefreshClicked();
    void onDownloadClicked();
    void onUploadFileClicked();
    void onUploadDirectoryClicked();
    void onDriveChanged(int index);
    void onRemoteCellDoubleClicked(int row, int column);

private:
    void setupUi();
    void populateRemoteFiles();
    void updatePathEdit();
    void updateMountedPaths(const QJsonArray &paths);
    void updateDriveCombo();
    QString joinRemotePath(const QString &basePath, const QString &name) const;
    QString parentRemotePath(const QString &path) const;

    QCheckBox *m_followPathCheck = nullptr;
    QComboBox *m_driveCombo = nullptr;
    QLineEdit *m_remotePathEdit = nullptr;
    QTableWidget *m_remoteTable = nullptr;
    QToolButton *m_parentButton = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QToolButton *m_uploadFileButton = nullptr;
    QToolButton *m_uploadDirectoryButton = nullptr;
    QToolButton *m_downloadButton = nullptr;

    bool m_connected = false;
    bool m_updatingDriveCombo = false;
    QString m_currentRemotePath;
    QStringList m_mountedPaths;
    QJsonArray m_remoteFiles;
    QIcon m_dirIcon;
    QIcon m_fileIcon;
};

#endif /* TERMINAL_FILE_PANEL_H */
