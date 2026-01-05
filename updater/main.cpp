#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QProgressBar>
#include <QSaveFile>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
struct CopyJob {
    QString src;
    QString dst;
};

static bool waitForPid(qint64 pid)
{
    if (pid <= 0) {
        return true;
    }
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        return true;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return true;
#else
    Q_UNUSED(pid);
    return true;
#endif
}
} // namespace

class UpdateWorker : public QObject
{
    Q_OBJECT

public:
    UpdateWorker(const QList<CopyJob> &jobs, const QString &launchPath, qint64 pid)
        : m_jobs(jobs)
        , m_launchPath(launchPath)
        , m_pid(pid)
    {}

signals:
    void statusChanged(const QString &text);
    void progressChanged(int value);
    void failed(const QString &error);
    void finished();

public slots:
    void run()
    {
        emit statusChanged(QStringLiteral("Waiting for app to close..."));
        if (!waitForPid(m_pid)) {
            emit failed(QStringLiteral("Failed to wait for app."));
            return;
        }

        emit statusChanged(QStringLiteral("Installing update..."));
        qint64 totalBytes = 0;
        for (const auto &job : m_jobs) {
            totalBytes += QFileInfo(job.src).size();
        }
        if (totalBytes <= 0) {
            totalBytes = 1;
        }

        qint64 doneBytes = 0;
        for (const auto &job : m_jobs) {
            if (!copyFile(job.src, job.dst, doneBytes, totalBytes)) {
                return;
            }
        }

        if (!m_launchPath.isEmpty()) {
            QProcess::startDetached(m_launchPath, QStringList());
        }
        emit finished();
    }

private:
    bool copyFile(const QString &src, const QString &dst, qint64 &doneBytes, qint64 totalBytes)
    {
        QFile in(src);
        if (!in.open(QIODevice::ReadOnly)) {
            emit failed(QStringLiteral("Cannot read %1").arg(src));
            return false;
        }

        QFileInfo dstInfo(dst);
        QDir().mkpath(dstInfo.dir().absolutePath());

        QSaveFile out(dst);
        out.setDirectWriteFallback(true);
        if (!out.open(QIODevice::WriteOnly)) {
            emit failed(QStringLiteral("Cannot write %1").arg(dst));
            return false;
        }

        constexpr qint64 kChunkSize = 1024 * 1024;
        while (!in.atEnd()) {
            const QByteArray chunk = in.read(kChunkSize);
            if (chunk.isEmpty()) {
                break;
            }
            if (out.write(chunk) != chunk.size()) {
                emit failed(QStringLiteral("Write failed for %1").arg(dst));
                return false;
            }
            doneBytes += chunk.size();
            const int pct = static_cast<int>((doneBytes * 100) / totalBytes);
            emit progressChanged(pct);
        }

        if (!out.commit()) {
            emit failed(QStringLiteral("Commit failed for %1").arg(dst));
            return false;
        }
        return true;
    }

    QList<CopyJob> m_jobs;
    QString m_launchPath;
    qint64 m_pid = 0;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FusionUpdater"));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {QStringLiteral("src"), QStringLiteral("Source file"), QStringLiteral("path")},
        {QStringLiteral("dst"), QStringLiteral("Destination file"), QStringLiteral("path")},
        {QStringLiteral("backend-src"), QStringLiteral("Backend source file"), QStringLiteral("path")},
        {QStringLiteral("backend-dst"), QStringLiteral("Backend destination file"), QStringLiteral("path")},
        {QStringLiteral("launch"), QStringLiteral("Executable to launch after update"), QStringLiteral("path")},
        {QStringLiteral("pid"), QStringLiteral("PID of the running app"), QStringLiteral("pid")}
    });
    parser.process(app);

    const QString src = parser.value(QStringLiteral("src"));
    const QString dst = parser.value(QStringLiteral("dst"));
    if (src.isEmpty() || dst.isEmpty()) {
        return 1;
    }

    QList<CopyJob> jobs;
    jobs.append({src, dst});

    const QString backendSrc = parser.value(QStringLiteral("backend-src"));
    const QString backendDst = parser.value(QStringLiteral("backend-dst"));
    if (!backendSrc.isEmpty() && !backendDst.isEmpty()) {
        jobs.append({backendSrc, backendDst});
    }

    const QString launchPath = parser.value(QStringLiteral("launch"));
    bool ok = false;
    const qint64 pid = parser.value(QStringLiteral("pid")).toLongLong(&ok);

    QWidget window;
    window.setWindowTitle(QStringLiteral("Updating Fusion Terminal"));
    window.setFixedSize(320, 130);

    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("Installing update..."), &window);
    layout->addWidget(title);

    auto *progress = new QProgressBar(&window);
    progress->setRange(0, 100);
    progress->setValue(0);
    layout->addWidget(progress);

    auto *status = new QLabel(QStringLiteral("Starting..."), &window);
    layout->addWidget(status);

    window.show();

    auto *worker = new UpdateWorker(jobs, launchPath, ok ? pid : 0);
    auto *thread = new QThread(&app);
    worker->moveToThread(thread);

    QObject::connect(thread, &QThread::started, worker, &UpdateWorker::run);
    QObject::connect(worker, &UpdateWorker::statusChanged, status, &QLabel::setText);
    QObject::connect(worker, &UpdateWorker::progressChanged, progress, &QProgressBar::setValue);
    QObject::connect(worker, &UpdateWorker::failed, [&window, status, thread](const QString &err) {
        status->setText(err);
        window.setWindowTitle(QStringLiteral("Update failed"));
        thread->quit();
    });
    QObject::connect(worker, &UpdateWorker::finished, [&app, thread]() {
        thread->quit();
        app.quit();
    });
    QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);

    thread->start();
    return app.exec();
}

#include "main.moc"
