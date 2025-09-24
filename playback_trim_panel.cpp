#include "playback_trim_panel.h"
#include <QHBoxLayout>
#include <QCheckBox>
#include <QTimeEdit>
#include <QPushButton>
#include <QLabel>
#include <QTime>
#include <QProgressBar>

static inline QTime nsToTime(qint64 ns){
    qint64 s = ns/1000000000LL; return QTime::fromMSecsSinceStartOfDay(int((s%86400)*1000));
}

PlaybackTrimPanel::PlaybackTrimPanel(QWidget* p): QWidget(p){
    auto *h = new QHBoxLayout(this); h->setContentsMargins(12,4,12,4); h->setSpacing(12);
    enableBox_ = new QCheckBox("Enable Trim/Export", this);
    startEdit_ = new QTimeEdit(this); startEdit_->setDisplayFormat("HH:mm:ss");
    endEdit_   = new QTimeEdit(this); endEdit_->setDisplayFormat("HH:mm:ss");
    durLab_    = new QLabel("Duration: 00:00:00", this);
    exportBtn_ = new QPushButton("Export", this);
    busyBar_   = new QProgressBar(this);
    busyBar_->setRange(0,0);            // indeterminate
    busyBar_->setVisible(false);
    busyBar_->setFixedWidth(120);

    enableBox_->setStyleSheet(
        "QCheckBox { color: white; }"
        "QCheckBox::indicator { background-color: #222; border: 1px solid #aaa; }"
        "QCheckBox::indicator:checked { background-color: white; }"
    );

    h->addWidget(enableBox_);
    h->addSpacing(8);
    h->addWidget(new QLabel("Start:", this)); h->addWidget(startEdit_);
    h->addWidget(new QLabel("End:", this));   h->addWidget(endEdit_);
    h->addWidget(durLab_);
    h->addStretch(1);
    h->addWidget(busyBar_);
    h->addWidget(exportBtn_);

    setEnabledPanel(false);

    connect(enableBox_, &QCheckBox::toggled, this, &PlaybackTrimPanel::trimModeToggled);
    connect(startEdit_, &QTimeEdit::timeChanged, this, [this]{ emit startEditedNs(timeEditToNs(startEdit_)); });
    connect(endEdit_,   &QTimeEdit::timeChanged, this, [this]{ emit endEditedNs(timeEditToNs(endEdit_)); });
    connect(exportBtn_, &QPushButton::clicked, this, &PlaybackTrimPanel::exportRequested);
}

void PlaybackTrimPanel::setEnabledPanel(bool on){
    startEdit_->setEnabled(on);
    endEdit_->setEnabled(on);
    exportBtn_->setEnabled(on);
}

void PlaybackTrimPanel::setDayStartNs(qint64 ns){ dayStartNs_=ns; }

void PlaybackTrimPanel::setTimeEdit(QTimeEdit* w, qint64 ns){ w->setTime(nsToTime(ns)); }

qint64 PlaybackTrimPanel::timeEditToNs(const QTimeEdit* w) const{
    const QTime t = w->time();
    return ((t.hour()*3600LL + t.minute()*60LL + t.second())*1000000000LL);
}

void PlaybackTrimPanel::setRangeNs(qint64 start_ns, qint64 end_ns){
    setTimeEdit(startEdit_, start_ns);
    setTimeEdit(endEdit_,   end_ns);
    setDurationLabel(end_ns - start_ns);
}

void PlaybackTrimPanel::setDurationLabel(qint64 dur_ns){
    qint64 s = dur_ns/1000000000LL; int hh=s/3600, mm=(s%3600)/60, ss=s%60;
    durLab_->setText(QString("Duration: %1:%2:%3").arg(hh,2,10,QChar('0')).arg(mm,2,10,QChar('0')).arg(ss,2,10,QChar('0')));
}

void PlaybackTrimPanel::setBusy(bool on){
    busyBar_->setVisible(on);
    exportBtn_->setEnabled(!on);
    startEdit_->setEnabled(!on);
    endEdit_->setEnabled(!on);
    exportBtn_->setText(on ? "Exporting..." : "Export");
}

void PlaybackTrimPanel::onExportStarted(){ setBusy(true); }
void PlaybackTrimPanel::onExportProgress(double){ /* keep indeterminate by default */ }
void PlaybackTrimPanel::onExportFinished(const QString&){ setBusy(false); }
void PlaybackTrimPanel::onExportError(const QString&){ setBusy(false); }
