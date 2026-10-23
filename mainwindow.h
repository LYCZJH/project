#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <atomic>

class QLineEdit;
class QPushButton;
class QTextEdit;
class QLabel;
class QCheckBox;
class QComboBox;
class QProgressBar;

//后台工作线程
class Worker : public QObject {
    Q_OBJECT
public:
    struct Params {
        QString src, dst;
        QStringList names;
        bool recursive = true;
        bool fuzzy = false;
        int conflict = 0;
    };
    Worker(const Params& p) : p_(p) {}
    void requestStop() { stop_.store(true); }

public slots:
    void run();

signals:
    void logMsg(const QString&);
    void progress(int cur, int total);
    void finished(int found, int copied, QStringList missing, QStringList failed, bool aborted);

private:
    QStringList matchName(const QString& name,
                          const QMap<QString, QStringList>& full,
                          const QMap<QString, QStringList>& stem);
    QString uniquePath(const QString& dir, const QString& fname);
    Params p_;
    std::atomic<bool> stop_{false};
};

//主窗口
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
private slots:

    void pickSrc();
    void pickDst();
    void pasteFromClipboard();
    void updateCount();
    void start();
    void onFinished(int found, int copied, QStringList missing, QStringList failed, bool aborted);

private:
    static QStringList parseNames(const QString& text);

    QLineEdit   *srcEdit = nullptr, *dstEdit = nullptr;
    QTextEdit   *namesEdit = nullptr, *logEdit = nullptr;
    QLabel      *countLabel = nullptr, *statusLabel = nullptr;
    QCheckBox   *recurseChk = nullptr, *fuzzyChk = nullptr;
    QComboBox   *conflictCombo = nullptr;
    QPushButton *startBtn = nullptr, *stopBtn = nullptr;
    QProgressBar *progress = nullptr;
    Worker*  worker_ = nullptr;
    QThread* thread_ = nullptr;
    QString  lastDst_;
};

#endif // MAINWINDOW_H