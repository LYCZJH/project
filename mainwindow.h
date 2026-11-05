#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <atomic>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QProgressBar>
#include <QMap>
#include <QListWidget>
#include <QVector>
#include <mutex>
#include <condition_variable>
#include <QSet>
#include <QThread>
#include <QTabWidget>
#include <QGroupBox>
#include <QDragEnterEvent>
#include <QDropEvent>

class Worker : public QObject {
    Q_OBJECT

public:
    struct Params
    {
        QVector<QString> src_files;
        //QString src_file;//源文件
        QString dst_file;//目标
        QStringList names;//查找
        bool subfolder = true;//子文件
        bool fuzzy = false;//模糊匹配
        int combobox = 0;//下拉框

        bool previewMode = false; //标记是否处于预览模式
        bool moveMode = false; // true移动false复制

        bool hidden = false;   //是否包含隐藏文件
        bool system = false;     //是否包含系统文件

        QSet<QString> processedFiles;// 记录已成功处理的文件路径
        QString ContinueFilePath;       // 断点记录文件json的保存路径
        QSet<QString> pendingDeletes;  //已复制但删除源失败的文件
    };
    enum Decision { Retry = 0, Skip = 1, Abort = 2 };
    Worker(const Params& p) : p(p) {}
    void requestStop() { stop = true; requestContinue(); }
    void setDecision(Decision d) { decision = d; }

    void requestPause() { pauseFlag = true; }
    void requestContinue()
    {
        pauseFlag = false;
        pauseCond.notify_all();
    }

    bool        illegalName(const QString& n, QString* why = nullptr) const;
    QStringList matchName(const QString& name,const QMap<QString, QStringList>& full, const QMap<QString, QStringList>& stem);
    QString     uniquePath(const QString& dir, const QString& fname);

    static QString computeTaskSignature(const Params& p);

public slots:
    void run();

signals:
    void logMsg(const QString&);
    void progress(int cur, int total);
    void finished(int found, int copied, QStringList missing, QStringList failed, bool aborted);
    void askSignal(const QString& text, const QString& target, int* result);

    void previewFinished(const QMap<QString, QStringList> &files);
    void Paused(int count);
    void Continue();

    void statsUpdated(int matched, int copied, int skipped, int missing, int failed);

private:

    Params p;
    std::atomic<bool> stop{false};
    int askUser(const QString& text, const QString& target);

    bool preflight();
    bool scanPhase(QMap<QString,QStringList>& fullmap,QMap<QString,QStringList>& stemmap,QStringList& quarantine);
    Decision decision = Retry;

    std::atomic<bool> pauseFlag{false};//是否处于暂停状态
    std::atomic<int> pauseCount{0}; //暂停次数
    std::mutex pauseMutex;//线程锁
    std::condition_variable pauseCond;
    void checkPause();
    void saveContinueRecord();   //保存断点记录到json
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    static QStringList parseNames(const QString& text);
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:

    void pickSrc();
    void pickDst();
    void pasteFromClipboard();
    void updateCount();
    void start();
    void onFinished(int found, int copied, QStringList missing, QStringList failed, bool aborted);

    void onPreviewClicked();
    void loadList();
    void saveList();

    void saveReport();   // 保存日志

    void onPauseClicked();
    void onContinueClicked();

private:

    QLineEdit*srcEdit = nullptr;
    QLineEdit*dstEdit = nullptr;

    QTextEdit*namesEdit = nullptr;
    QTextEdit*logEdit = nullptr;

    QLabel*countLabel = nullptr;
    QLabel*statusLabel = nullptr;

    QCheckBox*recurseChk = nullptr;
    QCheckBox*fuzzyChk = nullptr;

    QComboBox*conflictCombo = nullptr;

    QPushButton*startBtn = nullptr;
    QPushButton*stopBtn = nullptr;

    QProgressBar*progress = nullptr;
    Worker*worker = nullptr;
    QThread*thread = nullptr;
    QString lastDst;

    QListWidget *previewListWidget;

    QComboBox* modeCombo = nullptr;//复制移动模式
    QStringList lastMissing, lastFailed; //上次运行的未找到失败清单

    QCheckBox* hiddenChk = nullptr;
    QCheckBox* systemChk = nullptr;

    QPushButton*pauseBtn = nullptr;
    QPushButton*continueBtn = nullptr;
    QLabel*pauseStatusLabel = nullptr;

    QLabel *matchCountLabel  = nullptr;
    QLabel *copiedCountLabel = nullptr;
    QLabel *skippedCountLabel= nullptr;
    QLabel *missingCountLabel= nullptr;
    QLabel *failedCountLabel = nullptr;

    QTabWidget  *logTabWidget      = nullptr;//日志
    QListWidget *missingListWidget = nullptr;//未找到
    QListWidget *failedListWidget  = nullptr;//失败
};

#endif // MAINWINDOW_H