#include "playback_exporter.h"
#include "storageservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDate>
#include <QTextStream>
#include <QDateTime>

static inline double secFromNs(qint64 ns){ return double(ns)/1e9; }

PlaybackExporter::PlaybackExporter(QObject* p): QObject(p) {}

void PlaybackExporter::setPlaylist(const QVector<PlaybackSegmentIndex::FileSeg>& pl, qint64 dayStartNs){
    playlist_ = pl; dayStartNs_ = dayStartNs;
}
void PlaybackExporter::setSelection(qint64 s, qint64 e){
    selStartNs_ = s; selEndNs_ = e;
}
void PlaybackExporter::setOptions(const ExportOptions& o){ opts_ = o; }

void PlaybackExporter::cancel(){ abort_.store(true); }

void PlaybackExporter::start(){
    emit started();
    emit log("[Export] start");
    if (selEndNs_ <= selStartNs_) { emit error("Invalid selection"); return; }
    if (playlist_.isEmpty()) { emit error("No playlist"); return; }

    // Require external storage
    auto *ss = StorageService::instance();
    if (!ss->hasExternal()) { emit error("No external media detected"); return; }

    // Default output dir if not provided
    if (opts_.outDir.isEmpty()) {
        opts_.outDir = QDir(ss->externalRoot()).filePath("CamVigilExports");
    }
    if (!ensureOutDir_(nullptr)) { emit error("Cannot create output directory"); return; }

    // Plan parts
    const auto parts = computeParts_();
    if (parts.isEmpty()) { emit error("Selection overlaps no files"); return; }

    // Free-space check on external drive for final file
    const qint64 estimate = estimateBytes_(parts);
    const qint64 free = ss->freeBytes();
    const qint64 need = qMax(opts_.minFreeBytes, estimate);
    emit log(QString("[Export] estimate=%1 MB, free=%2 MB")
             .arg(estimate/1024/1024).arg(free/1024/1024));
    if (free < need) {
        emit error(QString("Not enough free space. Need ≥ %1 MB").arg(need/1024/1024));
        return;
    }

    // Work in an internal temp dir for speed, then copy the final to USB
    QTemporaryDir tmp;
    if (!tmp.isValid()) { emit error("Temp directory creation failed"); return; }
    const QString tempDir = tmp.path();
    emit log(QString("[Export] tmp: %1").arg(tempDir));

    // Build list of inputs for concat: cut only non-whole parts
    QStringList inputPaths;
    if (!buildInputs_(parts, tempDir, &inputPaths)) { emit error("Prepare inputs failed"); return; }
    if (abort_.load()) { emit error("Canceled"); return; }

    // Concat list in temp
    QString listPath;
    if (!writeConcatList_(inputPaths, &listPath)) { emit error("Concat list write failed"); return; }

    // Concat to temp, then copy to external outDir
    const QString finalOut = uniqueOutPath_();
    const QString tmpOut   = QDir(tempDir).filePath(QFileInfo(finalOut).fileName());
    if (!concat_(listPath, tmpOut)) { emit error("Concat failed"); return; }
    if (abort_.load()) { emit error("Canceled"); return; }

    // Copy final to external volume
    if (QFile::exists(finalOut)) QFile::remove(finalOut);
    if (!QFile::copy(tmpOut, finalOut)) { emit error(QString("Failed to copy to %1").arg(finalOut)); return; }

    emit progress(100.0);
    emit log(QString("[Export] OK -> %1").arg(finalOut));
    emit finished(finalOut);
}

QVector<ClipPart> PlaybackExporter::computeParts_() const {
    QVector<ClipPart> out;
    const qint64 selAbsA = dayStartNs_ + selStartNs_;
    const qint64 selAbsB = dayStartNs_ + selEndNs_;
    for (const auto& fs : playlist_) {
        const qint64 a = std::max(fs.start_ns, selAbsA);
        const qint64 b = std::min(fs.end_ns,   selAbsB);
        if (b > a) {
            const bool whole = (a == fs.start_ns) && (b == fs.end_ns);
            out.push_back(ClipPart{
                fs.path,
                a - fs.start_ns,
                b - fs.start_ns,
                whole
            });
        }
        if (fs.end_ns >= selAbsB) break;
    }
    return out;
}

bool PlaybackExporter::ensureOutDir_(QString* err) const {
    QDir d(opts_.outDir);
    if (d.exists()) return true;
    if (QDir().mkpath(opts_.outDir)) return true;
    if (err) *err = "mkpath failed";
    return false;
}

QString PlaybackExporter::uniqueOutPath_() const {
    const QString base = opts_.baseName.isEmpty()
        ? QString("CamVigil_%1").arg(QDate::currentDate().toString("yyyy-MM-dd"))
        : opts_.baseName;
    QString out = QDir(opts_.outDir).filePath(base + ".mp4");
    int i=1;
    while (QFile::exists(out)) out = QDir(opts_.outDir).filePath(QString("%1(%2).mp4").arg(base).arg(++i));
    return out;
}

bool PlaybackExporter::runFfmpeg_(const QStringList& args, QByteArray* errOut){
    if (abort_.load()) return false;
    QProcess p;
    p.setProgram(opts_.ffmpegPath);
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start();
    if (!p.waitForStarted()) return false;

    while (p.state() == QProcess::Running) {
        if (abort_.load()) {
            p.kill();
            p.waitForFinished();
            return false;
        }
        p.waitForReadyRead(50);
    }
    if (errOut) *errOut = p.readAllStandardError();
    return p.exitStatus()==QProcess::NormalExit && p.exitCode()==0;
}

bool PlaybackExporter::buildInputs_(const QVector<ClipPart>& parts,
                                    const QString& tempDir,
                                    QStringList* inputPaths){
    const int N = parts.size();
    inputPaths->reserve(N);
    for (int i=0;i<N;++i) {
        if (abort_.load()) return false;
        const auto& part = parts[i];

        if (part.wholeFile) {
            inputPaths->push_back(QFileInfo(part.path).absoluteFilePath());
            continue;
        }

        const double ss = secFromNs(part.inStartNs);
        const double to = secFromNs(part.inEndNs);
        const QString cut = QDir(tempDir).filePath(QString("part_%1.mkv").arg(i,4,10,QChar('0')));
        inputPaths->push_back(cut);

        QStringList args; args << "-hide_banner" << "-y";
        if (opts_.precise) {
            const double coarse = std::max(0.0, ss - 3.0);
            args << "-ss" << QString::number(coarse, 'f', 3)
                 << "-i"  << part.path
                 << "-ss" << QString::number(ss - coarse, 'f', 6)
                 << "-to" << QString::number(to - coarse, 'f', 6)
                 << "-c:v" << opts_.vcodec
                 << "-preset" << opts_.preset
                 << "-crf" << QString::number(opts_.crf)
                 << "-pix_fmt" << "yuv420p"
                 << "-fflags" << "+genpts"
                 << "-reset_timestamps" << "1";
            if (opts_.copyAudio) args << "-c:a" << "copy";
            else                 args << "-c:a" << "aac" << "-b:a" << "128k";
            args << "-movflags" << "+faststart"
                 << cut;
        } else {
            args << "-ss" << QString::number(ss, 'f', 6)
                 << "-to" << QString::number(to, 'f', 6)
                 << "-i"  << part.path
                 << "-c"  << "copy"
                 << "-avoid_negative_ts" << "make_zero"
                 << cut;
        }

        QByteArray err;
        emit log(QString("[Export] cut %1/%2").arg(i+1).arg(N));
        if (!runFfmpeg_(args, &err)) { emit log(QString::fromUtf8(err)); return false; }
        emit progress( (i+1) * 100.0 / (N+2) ); // leave headroom for concat/final copy
    }
    return true;
}

bool PlaybackExporter::writeConcatList_(const QStringList& inputPaths, QString* listPath){
    const QString p = QDir::temp().absoluteFilePath("concat_inputs.txt");
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly|QIODevice::Text)) return false;
    QTextStream ts(&f);
    for (const auto& cp : inputPaths) {
        ts << "file '" << QFileInfo(cp).absoluteFilePath().replace('\'',"\\'") << "'\n";
    }
    f.close();
    *listPath = p;
    return true;
}

bool PlaybackExporter::concat_(const QString& listPath, const QString& outPath){
    QStringList args;
    args << "-hide_banner" << "-y"
         << "-f" << "concat" << "-safe" << "0"
         << "-i" << listPath;

    if (opts_.precise) {
        args << "-c:v" << opts_.vcodec
             << "-preset" << opts_.preset
             << "-crf" << QString::number(opts_.crf);
        if (opts_.copyAudio) args << "-c:a" << "copy";
    } else {
        args << "-c" << "copy";
    }
    args << outPath;

    QByteArray err;
    emit log("[Export] concat");
    const bool ok = runFfmpeg_(args, &err);
    if (!ok) emit log(QString::fromUtf8(err));
    else     emit log(QString("[Export] wrote %1").arg(outPath));
    emit progress(99.0);
    return ok;
}

qint64 PlaybackExporter::estimateBytes_(const QVector<ClipPart>& parts) const {
    // Duration-based estimate. Same path for precise/non-precise.
    double durSec = 0.0;
    for (const auto& p : parts) durSec += secFromNs(p.inEndNs - p.inStartNs);
    const double v_bps = opts_.precise ? 6.0e6 : 4.0e6; // conservative
    const double a_bps = 128.0e3;
    qint64 bytes = qint64((v_bps + a_bps) * durSec / 8.0);
    if (bytes < 200ll*1024*1024) bytes = 200ll*1024*1024; // floor 200MB
    return bytes;
}
