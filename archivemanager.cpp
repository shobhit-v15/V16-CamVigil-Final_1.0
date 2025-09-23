// archivemanager.cpp
#include "archivemanager.h"

#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QDebug>
#include <QStorageInfo>
#include <QUuid>

#include "db_writer.h"

// Resolve storage root. Env override supported.
QString ArchiveManager::defaultStorageRoot() {
    const QString env = qEnvironmentVariable("CAMVIGIL_ARCHIVE_ROOT");
    if (!env.isEmpty()) return env;
    return QDir::homePath() + "/CamVigil_StoragePartition";
}

ArchiveManager::ArchiveManager(QObject *parent)
    : QObject(parent),
      defaultDuration(300)  // 5 min
{
    // Set archiveDir immediately under home-based root
    archiveDir = defaultStorageRoot() + "/CamVigilArchives";
    QDir().mkpath(archiveDir);

    connect(&cleanupTimer, &QTimer::timeout, this, &ArchiveManager::cleanupArchive);
    cleanupTimer.start(60 * 60 * 1000);  // hourly

    qDebug() << "[ArchiveManager] Initialized. archiveDir=" << archiveDir;
}

ArchiveManager::~ArchiveManager()
{
    stopRecording();

    // stop DB thread
    if (dbThread) {
        dbThread->quit();
        dbThread->wait();
        dbThread = nullptr;
    }
    qDebug() << "[ArchiveManager] Destroyed.";
}

void ArchiveManager::startRecording(const std::vector<CamHWProfile> &camProfiles)
{
    cameraProfiles = camProfiles;

    // Ensure path each start, allow env override at runtime
    archiveDir = defaultStorageRoot() + "/CamVigilArchives";
    QDir().mkpath(archiveDir);

    // Optional low-space warning (5 GB threshold)
    QStorageInfo si(archiveDir);
    if (si.isValid() && si.bytesAvailable() > 0 && si.bytesAvailable() < 5LL*1024*1024*1024) {
        qWarning() << "[ArchiveManager] Low free space in" << archiveDir
                   << "avail=" << si.bytesAvailable();
    }

    // DB bring-up
    if (!dbThread) {
        dbThread = new QThread(this);
        db = new DbWriter();
        db->moveToThread(dbThread);
        connect(dbThread, &QThread::finished, db, &QObject::deleteLater);
        dbThread->start();
        QMetaObject::invokeMethod(
            db, "openAt", Qt::BlockingQueuedConnection,
            Q_ARG(QString, archiveDir + "/camvigil.sqlite"));
    }

    // Ensure cameras
    for (const auto& p : camProfiles) {
        QMetaObject::invokeMethod(db, "ensureCamera", Qt::QueuedConnection,
            Q_ARG(QString, QString::fromStdString(p.url)),
            Q_ARG(QString, QString::fromStdString(p.suburl)),
            Q_ARG(QString, QString::fromStdString(p.displayName)));
    }

    // New session
    sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QMetaObject::invokeMethod(db, "beginSession", Qt::QueuedConnection,
        Q_ARG(QString, sessionId), Q_ARG(QString, archiveDir), Q_ARG(int, defaultDuration));

    // Master start timestamp for aligned chunk names
    QDateTime masterStart = QDateTime::currentDateTime();
    qDebug() << "[ArchiveManager] Master start:" << masterStart.toString("yyyyMMdd_HHmmss");

    // Launch workers
    for (size_t i = 0; i < camProfiles.size(); ++i) {
        const auto &profile = camProfiles[i];
        auto* worker = new ArchiveWorker(
            profile.url,
            static_cast<int>(i),
            archiveDir,
            defaultDuration,
            masterStart
        );

        connect(worker, &ArchiveWorker::recordingError, [](const std::string &err){
            qDebug() << "[ArchiveManager] ArchiveWorker error:" << QString::fromStdString(err);
        });

        // segment → DB: open
        connect(worker, &ArchiveWorker::segmentOpened, this,
            [this, camProfiles](int camIdx, const QString& path, qint64 startNs){
                const QString camUrl = QString::fromStdString(camProfiles[camIdx].url);
                QMetaObject::invokeMethod(db, "addSegmentOpened", Qt::QueuedConnection,
                    Q_ARG(QString, sessionId), Q_ARG(QString, camUrl),
                    Q_ARG(QString, path), Q_ARG(qint64, startNs));
            });

        // segment → DB: finalize
        connect(worker, &ArchiveWorker::segmentClosed, this,
            [this](int camIdx, const QString& path, qint64 endNs, qint64 durMs){
                Q_UNUSED(camIdx);
                QMetaObject::invokeMethod(db, "finalizeSegmentByPath", Qt::QueuedConnection,
                    Q_ARG(QString, path), Q_ARG(qint64, endNs), Q_ARG(qint64, durMs));
            });

        connect(worker, &ArchiveWorker::segmentFinalized, this, &ArchiveManager::segmentWritten);

        workers.push_back(worker);
        worker->start();
        qDebug() << "[ArchiveManager] Started ArchiveWorker for cam" << i;
    }

    qDebug() << "[ArchiveManager] Recording at" << archiveDir;
}

void ArchiveManager::stopRecording()
{
    for (auto* worker : workers) {
        worker->stop();
        worker->wait();
        delete worker;
    }
    workers.clear();
    qDebug() << "[ArchiveManager] All ArchiveWorkers stopped.";
}

void ArchiveManager::updateSegmentDuration(int seconds)
{
    qDebug() << "[ArchiveManager] Update segment duration to" << seconds << "s";
    for (auto *worker : workers) {
        QMetaObject::invokeMethod(worker, "updateSegmentDuration", Qt::QueuedConnection,
                                  Q_ARG(int, seconds));
    }
}

void ArchiveManager::cleanupArchive()
{
    if (archiveDir.isEmpty()) {
        qDebug() << "[ArchiveManager] No archive directory set.";
        return;
    }
    QDir dir(archiveDir);
    if (!dir.exists()) {
        qDebug() << "[ArchiveManager] Archive directory does not exist:" << archiveDir;
        return;
    }

    // Placeholder for retention policy (age/size purge) if needed.
    qDebug() << "[ArchiveManager] Cleanup complete.";
}
