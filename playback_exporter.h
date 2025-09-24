#pragma once
#include <QObject>
#include <QVector>
#include <QString>
#include <atomic>
#include <QProcess>
#include <QTemporaryDir>
#include "playback_segment_index.h" // for FileSeg

struct ExportOptions {
    QString ffmpegPath = "ffmpeg";
    QString outDir;              // must be set to externalRoot()/CamVigilExports
    QString baseName;            // e.g., "CamVigil_YYYY-MM-DD"
    bool precise = false;        // false => -c copy, true => re-encode
    QString vcodec = "libx264";  // used if precise
    QString preset = "veryfast"; // used if precise
    int crf = 18;                // used if precise
    bool copyAudio = true;       // precise mode: copy audio if possible

    // Optional guardrails
    qint64 minFreeBytes = 512ll * 1024 * 1024; // 512 MB
};

struct ClipPart {
    QString path;
    qint64  inStartNs;   // offset inside file
    qint64  inEndNs;     // offset inside file
};

class PlaybackExporter final : public QObject {
    Q_OBJECT
public:
    explicit PlaybackExporter(QObject* parent=nullptr);

    void setPlaylist(const QVector<PlaybackSegmentIndex::FileSeg>& playlist, qint64 dayStartNs);
    void setSelection(qint64 selStartNs, qint64 selEndNs); // ns from midnight
    void setOptions(const ExportOptions& opts);

public slots:
    void start();      // run export
    void cancel();     // best-effort cancel

signals:
    void progress(double pct);   // 0..100
    void log(QString line);
    void finished(QString outPath);  // success
    void error(QString msg);         // failed
    void started();                  // notify UI to go busy

private:
    QVector<PlaybackSegmentIndex::FileSeg> playlist_;
    qint64 dayStartNs_{0};
    qint64 selStartNs_{0};
    qint64 selEndNs_{0};
    ExportOptions opts_;
    std::atomic_bool abort_{false};

    QVector<ClipPart> computeParts_() const;
    bool ensureOutDir_(QString* err) const;
    QString uniqueOutPath_() const;
    bool runFfmpeg_(const QStringList& args, QByteArray* errOut);
    bool cutParts_(const QVector<ClipPart>& parts, QStringList* cutPaths, const QString& tmpDir);
    bool writeConcatList_(const QStringList& cutPaths, const QString& listPath);
    bool concat_(const QString& listPath, const QString& outPath);

    qint64 estimateBytes_(const QVector<ClipPart>& parts) const;
};
