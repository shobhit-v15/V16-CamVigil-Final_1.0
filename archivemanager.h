// archivemanager.h
#ifndef ARCHIVEMANAGER_H
#define ARCHIVEMANAGER_H

#include <QObject>
#include <QTimer>
#include <QThread>
#include <vector>
#include <string>

#include "archiveworker.h"
#include "camerastreams.h" // CamHWProfile

class DbWriter;

class ArchiveManager : public QObject {
    Q_OBJECT
public:
    explicit ArchiveManager(QObject* parent = nullptr);
    ~ArchiveManager();

    QString archiveRoot() const { return archiveDir; }
    QString getArchiveDir() const { return archiveDir; } // kept for compatibility

    void startRecording(const std::vector<CamHWProfile>& cameraProfiles);
    void stopRecording();
    void updateSegmentDuration(int seconds);

    // Default storage root: $HOME/CamVigil_StoragePartition
    static QString defaultStorageRoot();

public slots:
    void cleanupArchive();

signals:
    void segmentWritten();

private:
    QTimer cleanupTimer;
    std::vector<ArchiveWorker*> workers;
    QString archiveDir;
    int defaultDuration;  // seconds

    // Cached camera profiles
    std::vector<CamHWProfile> cameraProfiles;

    // DB thread + writer
    QThread* dbThread = nullptr;
    DbWriter* db = nullptr;
    QString sessionId;
};

#endif // ARCHIVEMANAGER_H
