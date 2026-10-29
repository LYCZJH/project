#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <atomic>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QProgressBar>
#include <QMap>
#include <QStringList>
#include <QListWidget>
#include <QTextStream>
#include <QVector>
#include <mutex>              //暂停互斥锁
#include <condition_variable> //暂停条件变量
#include <QSet>               //用于记录已处理文件

//后台工作线程
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
        bool moveMode = false; // true移动(复制后删除源)false复制

        bool hidden = false;   //是否包含隐藏文件
        bool system = false;     //是否包含系统文件

        QSet<QString> processedFiles;// 记录已成功处理的文件路径
        QString ContinueFilePath;       // 断点记录文件(.json)的保存路径
    };

    enum Decision { Retry = 0, Skip = 1, Abort = 2 };
    Worker(const Params& p) : p_(p) {}
    void requestStop() { stop_ = true; requestContinue(); }
    void setDecision(Decision d) { decision_ = d; }

    void requestPause() { pauseFlag_ = true; }
    void requestContinue()
    {
        pauseFlag_ = false;
        pauseCond_.notify_all();
    }

public slots:
    void run();

signals:
    void logMsg(const QString&);
    void progress(int cur, int total);
    void finished(int found, int copied, QStringList missing, QStringList failed, bool aborted);
    void askSignal(const QString& text, const QString& target, int* result);

    void previewFinished(const QMap<QString, QStringList> &files);
    //暂停状态信号
    void Paused(int count);
    void Continue();

private:
    QStringList matchName(const QString& name,const QMap<QString, QStringList>& full,const QMap<QString, QStringList>& stem);
    QString uniquePath(const QString& dir, const QString& fname);
    Params p_;
    std::atomic<bool> stop_{false};
    int askUser(const QString& text, const QString& target);

    bool preflight();
    bool scanPhase(QMap<QString,QStringList>& fullmap,QMap<QString,QStringList>& stemmap,QStringList& quarantine);
    bool illegalName(const QString& n, QString* why = nullptr) const;
    Decision decision_ = Retry;

    std::atomic<bool> pauseFlag_{false};//是否处于暂停状态
    std::atomic<int> pauseCount_{0}; //暂停次数
    std::mutex pauseMutex_;//线程锁
    std::condition_variable pauseCond_;//条件变量
    void checkPause();//检查暂停状态并阻塞
    void saveContinueRecord();   //保存断点记录到 JSON
};

//界面
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

    void onPreviewClicked();
    void loadList();
    void saveList();

    void saveReport();   // 保存日志+未找到/失败清单

    void onPauseClicked();//暂停/继续界面槽
    void onContinueClicked();

private:
    static QStringList parseNames(const QString& text);

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
    Worker*worker_ = nullptr;
    QThread*thread_ = nullptr;
    QString lastDst_;

    QListWidget *previewListWidget;

    QComboBox* modeCombo = nullptr;//复制/移动模式
    QStringList lastMissing_, lastFailed_; //上次运行的未找到/失败清单

    QCheckBox* hiddenChk = nullptr;
    QCheckBox* systemChk = nullptr;

    QPushButton*pauseBtn = nullptr;
    QPushButton*continueBtn = nullptr;
    QLabel*pauseStatusLabel = nullptr;
};

#endif // MAINWINDOW_H