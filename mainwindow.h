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

//后台工作线程
class Worker : public QObject {
    Q_OBJECT

public:
    struct Params
    {
        QString src_file;//源文件
        QString dst_file;//目标
        QStringList names;//查找
        bool subfolder = true;//子文件
        bool fuzzy = false;//模糊匹配
        int combobox = 0;//下拉框

        bool previewMode = false; //标记是否处于预览模式
    };

    enum Decision { Retry = 0, Skip = 1, Abort = 2 };
    Worker(const Params& p) : p_(p) {}
    void requestStop() { stop_ = true; }
    void setDecision(Decision d) { decision_ = d; }



public slots:
    void run();

signals:
    void logMsg(const QString&);
    void progress(int cur, int total);
    void finished(int found, int copied, QStringList missing, QStringList failed, bool aborted);
    void askSignal(const QString& text, const QString& target, int* result);

    void previewFinished(const QMap<QString, QStringList> &files);

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

    QListWidget *previewListWidget;
};

#endif // MAINWINDOW_H