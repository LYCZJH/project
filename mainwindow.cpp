#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDirIterator>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QMap>
#include <QMessageBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QFont>
#include <QApplication>
#include <QStorageInfo>

// Worker 线程实现
void Worker::run()
{
    emit logMsg("开始扫描文件...");
    QMap<QString, QStringList> fullMap, stemMap;
    int count = 0;
    auto flags = p_.subfolder ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator it(p_.src_file, QDir::Files | QDir::NoDotAndDotDot, flags);
    while (it.hasNext())
    {
        if (stop_.load())
        {
            emit finished(0, 0, QStringList(), QStringList(), true);
            return;
        }
        QString path = it.next();
        QFileInfo fi(path);
        fullMap[fi.fileName().toLower()].append(path);
        stemMap[fi.completeBaseName().toLower()].append(path);
        ++count;
    }
    emit logMsg(QString("扫描完成，共 %1 个文件").arg(count));

    QDir().mkpath(p_.dst_file);
    QSet<QString> copied;
    int found = 0, copiedN = 0;
    QStringList missing, failed;
    const int total = p_.names.size();

    for (int i = 0; i < total; ++i)
    {
        if (stop_.load())
        {
            emit finished(found, copiedN, missing, failed, true);
            return;
        }
        const QString& name = p_.names[i];
        QStringList paths = matchName(name, fullMap, stemMap);

        if (paths.isEmpty())
        {
            missing << name;
            emit logMsg(QString("  × 未找到: %1").arg(name));
        }
        else
        {
            ++found;
            for (const QString& path : std::as_const(paths))
            {
                if (stop_.load())
                break;
                QString fname  = QFileInfo(path).fileName();
                QString target = QDir(p_.dst_file).filePath(fname);
                QString absSrc = QFileInfo(path).absoluteFilePath();

                if (copied.contains(absSrc))
                continue;
                if (QFileInfo(target).absoluteFilePath() == absSrc)
                 continue;

                if (QFile::exists(target))
                {
                    if (p_.combobox == 1)
                    {
                        QFile::remove(target);
                    }
                    else if (p_.combobox == 2)
                    {
                        emit logMsg(QString("  [跳过] %1").arg(fname));
                        continue;
                    }
                    else
                    {
                        target = uniquePath(p_.dst_file, fname);
                    }
                }
//判断磁盘空间
                qint64 fileSize = QFileInfo(path).size();//获取准备复制的文件大小
                QStorageInfo si(p_.dst_file);//获取目标文件夹所在的磁盘信息

                if (si.isValid() && si.bytesAvailable() >= 0 && (quint64)si.bytesAvailable() < (quint64)fileSize)
                {
                    emit logMsg(QStringLiteral("  [告警] 磁盘空间不足: %1（需 %2 字节）").arg(target).arg(fileSize));
                    int d = askUser(QStringLiteral("目标磁盘剩余空间不足（本文件需要 %1 字节）").arg(fileSize), target);
                    if (d == 2 || stop_.load())
                    {
                        emit finished(found, copiedN, missing, failed, true);
                        return;
                    }
                    si.refresh();//刷新磁盘
                    if (d != 1 || si.bytesAvailable() < fileSize)
                        continue;                          // 跳过该文件
                }
//复制文件
                if (QFile::copy(path, target))
                {
                    copied.insert(absSrc);
                    ++copiedN;
                    emit logMsg(QString("  [成功] %1 -> %2").arg(path, target));
                }
                else
                {
                    failed << path;
                    emit logMsg(QString("  [失败] %1").arg(path));
                }
            }
        }
        emit progress(i + 1, total);//进度条
    }
    emit finished(found, copiedN, missing, failed, false);
}

int Worker::askUser(const QString& text, const QString& target)
{
    // 返回值0跳过 1重试 2中止
    // 使用 BlockingQueuedConnection，信号发出后会阻塞在子线程
    // 直到主线程弹框返回把结果写入 result
    int result = 0;
    emit askSignal(text, target, &result);
    return result;
}

QStringList Worker::matchName(const QString& name, const QMap<QString, QStringList>& full,const QMap<QString, QStringList>& stem)
{
    QString key = name.toLower();
    if (full.contains(key))
        return full.value(key);
    if (stem.contains(key))
        return stem.value(key);

    int dot = name.lastIndexOf('.');
    if (dot > 0)
    {
        QString s = name.left(dot).toLower();
        QString e = name.mid(dot).toLower();
        if (stem.contains(s))
        {
            QStringList res;
            for (const QString& p : stem.value(s))
                if (p.toLower().endsWith(e))
                    res << p;
            if (!res.isEmpty())
                return res;
        }
    }
    if (p_.fuzzy)
    {
        QStringList res;
        for (auto it = full.constBegin(); it != full.constEnd(); ++it)
            if (it.key().contains(key))
                res << it.value();
        if (res.isEmpty())
            for (auto it = stem.constBegin(); it != stem.constEnd(); ++it)
                if (it.key().contains(key))
                    res << it.value();
        return res;
    }
    return QStringList();
}

QString Worker::uniquePath(const QString& dir, const QString& fname)
{
    QFileInfo fi(fname);
    QString base = fi.completeBaseName(), ext = fi.suffix();
    for (int i = 1; i < 100000; ++i)
    {
        QString nm = ext.isEmpty()? QString("%1(%2)").arg(base).arg(i) : QString("%1(%2).%3").arg(base).arg(i).arg(ext);
        QString p = QDir(dir).filePath(nm);
        if (!QFile::exists(p))
            return p;
    }
    return QDir(dir).filePath(fname);
}


// MainWindow 实现
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    setWindowTitle("文件批量查找复制工具");
    resize(880, 720);
    auto* main = new QVBoxLayout(centralWidget); // 布局挂载到中心部件上

    //源文件夹
    auto* row1 = new QHBoxLayout;
    row1->addWidget(new QLabel("源文件夹:"));
    srcEdit = new QLineEdit("D:\\");
    row1->addWidget(srcEdit, 1);
    auto* b1 = new QPushButton("浏览...");
    connect(b1, &QPushButton::clicked, this, &MainWindow::pickSrc);
    row1->addWidget(b1);
    main->addLayout(row1);

    //文件名粘贴
    main->addWidget(new QLabel("粘贴文件名（每行一个，或用逗号/分号分隔）:"));
    auto* row2 = new QHBoxLayout;
    auto* pasteBtn = new QPushButton("从剪贴板粘贴");
    connect(pasteBtn, &QPushButton::clicked, this, &MainWindow::pasteFromClipboard);
    row2->addWidget(pasteBtn);
    auto* clearBtn = new QPushButton("清空");
    connect(clearBtn, &QPushButton::clicked, [this]
            { namesEdit->clear(); updateCount();});
    row2->addWidget(clearBtn);
    countLabel = new QLabel("共 0 个");
    row2->addWidget(countLabel);
    row2->addStretch();
    main->addLayout(row2);

    namesEdit = new QTextEdit;
    namesEdit->setAcceptRichText(false);
    namesEdit->setFont(QFont("Consolas", 10));
    connect(namesEdit, &QTextEdit::textChanged, this, &MainWindow::updateCount);
    main->addWidget(namesEdit, 2);

    //目标文件夹
    auto* row3 = new QHBoxLayout;
    row3->addWidget(new QLabel("目标文件夹:"));
    dstEdit = new QLineEdit("D:\\筛选结果");
    row3->addWidget(dstEdit, 1);
    auto* b2 = new QPushButton("浏览...");
    connect(b2, &QPushButton::clicked, this, &MainWindow::pickDst);
    row3->addWidget(b2);
    main->addLayout(row3);

    //选项
    auto* row4 = new QHBoxLayout;
    recurseChk = new QCheckBox("包含子文件夹");
    recurseChk->setChecked(true);
    row4->addWidget(recurseChk);
    fuzzyChk = new QCheckBox("模糊匹配");
    row4->addWidget(fuzzyChk);
    row4->addWidget(new QLabel("同名文件:"));
    conflictCombo = new QComboBox;
    conflictCombo->addItems({"自动重命名", "覆盖", "跳过"});
    row4->addWidget(conflictCombo);
    row4->addStretch();
    main->addLayout(row4);

    //按钮和进度
    auto* row5 = new QHBoxLayout;
    startBtn = new QPushButton("▶ 开始");
    connect(startBtn, &QPushButton::clicked, this, &MainWindow::start);
    row5->addWidget(startBtn);
    stopBtn = new QPushButton("停止");
    stopBtn->setEnabled(false);
    connect(stopBtn, &QPushButton::clicked, [this]
    {
        if (worker_) worker_->requestStop();
    });
    row5->addWidget(stopBtn);
    progress = new QProgressBar;
    row5->addWidget(progress, 1);
    statusLabel = new QLabel("就绪");
    row5->addWidget(statusLabel);
    main->addLayout(row5);

    //日志
    main->addWidget(new QLabel("日志:"));
    logEdit = new QTextEdit;
    logEdit->setReadOnly(true);
    logEdit->setFont(QFont("Consolas", 9));
    main->addWidget(logEdit, 2);
}

MainWindow::~MainWindow()
{
    if (thread_)
    {
        if (worker_)
        worker_->requestStop();
        thread_->quit();
        thread_->wait();
    }
}

void MainWindow::pickSrc()
{
    QString d = QFileDialog::getExistingDirectory(this, "选择源文件夹", srcEdit->text());
    if (!d.isEmpty())
        srcEdit->setText(QDir::toNativeSeparators(d));
}

void MainWindow::pickDst()
{
    QString d = QFileDialog::getExistingDirectory(this, "选择目标文件夹", dstEdit->text());
    if (!d.isEmpty())
        dstEdit->setText(QDir::toNativeSeparators(d));
}

void MainWindow::pasteFromClipboard()
{
    namesEdit->insertPlainText(QApplication::clipboard()->text());
    updateCount();
}

void MainWindow::updateCount()
{
    countLabel->setText(QString("共 %1 个").arg(parseNames(namesEdit->toPlainText()).size()));
}

void MainWindow::start()
{
    if (thread_)
        return;
    QString src = srcEdit->text().trimmed();
    if (!QDir(src).exists())
    {
        QMessageBox::warning(this, "提示", "源文件夹不存在");
        return;
    }
    QStringList names = parseNames(namesEdit->toPlainText());
    if (names.isEmpty())
    {
        QMessageBox::warning(this, "提示", "请先粘贴文件名");
        return;
    }
    QString dst = dstEdit->text().trimmed();
    if (dst.isEmpty())
    {
        QMessageBox::warning(this, "提示", "请填写目标文件夹");
        return;
    }
    QFileInfo dfi(dst);
    if (!dfi.isAbsolute())
        dst = QDir(QDir(src).absolutePath() + "/..").absoluteFilePath(dst);
    dst = QDir::cleanPath(dst);

    if (QDir::cleanPath(QFileInfo(src).absoluteFilePath()).toLower() == dst.toLower())
    {
        QMessageBox::warning(this, "提示", "目标文件夹不能和源文件夹相同");
        return;
    }

    Worker::Params p;
    p.src_file = src;
    p.dst_file = dst;
    p.names = names;
    p.subfolder = recurseChk->isChecked();
    p.fuzzy = fuzzyChk->isChecked();
    p.combobox = conflictCombo->currentIndex();
    lastDst_ = dst;

    logEdit->clear();
    progress->setValue(0);
    statusLabel->setText("运行中...");
    startBtn->setEnabled(false);
    stopBtn->setEnabled(true);

    worker_ = new Worker(p);
    thread_ = new QThread;
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::started, worker_, &Worker::run);
    connect(worker_, &Worker::logMsg, logEdit, &QTextEdit::append);
    connect(worker_, &Worker::progress, this, [this](int c, int t)
        {
        progress->setMaximum(t);
        progress->setValue(c);
        statusLabel->setText(QString("%1 / %2").arg(c).arg(t));
    });
    connect(worker_, &Worker::finished, this, &MainWindow::onFinished);

// 跨线程询问：BlockingQueuedConnection 让子线程等待主线程结果
    connect(worker_, &Worker::askSignal, this,[this](const QString& text, const QString& target, int* result)
            {
                QMessageBox box(QMessageBox::Warning, "磁盘空间不足",text + "\n\n文件: " + QFileInfo(target).fileName(),QMessageBox::NoButton, this);
                QPushButton* btnRetry = box.addButton("重试",    QMessageBox::AcceptRole);
                QPushButton* btnSkip  = box.addButton("跳过此文件", QMessageBox::RejectRole);
                QPushButton* btnAbort = box.addButton("中止全部", QMessageBox::DestructiveRole);
                box.setDefaultButton(btnRetry);
                box.exec();

                if (box.clickedButton() == btnRetry)*result = 1;
                else if (box.clickedButton() == btnAbort)*result = 2;
                else *result = 0;
            },
            Qt::BlockingQueuedConnection);

    thread_->start();
}

void MainWindow::onFinished(int found, int copied, QStringList missing, QStringList failed, bool aborted)
{
    startBtn->setEnabled(true);
    stopBtn->setEnabled(false);
    statusLabel->setText(aborted ? "已停止" : "完成");
    progress->setValue(0);
    thread_->quit();
    thread_->wait();
    delete worker_;
    worker_ = nullptr;
    delete thread_;
    thread_ = nullptr;

    QString msg = QString("匹配到 %1 个文件名\n复制成功 %2 个\n未找到 %3 个").arg(found).arg(copied).arg(missing.size());
    if (!failed.isEmpty())
    {
         msg += QString("\n复制失败 %1 个").arg(failed.size());
    }

    if (aborted)
    {
        msg += "\n\n（已被用户停止）";
        msg += "\n\n保存位置: " + lastDst_;
    }

    if (QMessageBox::question(this, "完成", msg + "\n\n是否打开目标文件夹？") == QMessageBox::Yes)
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(lastDst_));
    }
}

QStringList MainWindow::parseNames(const QString& text)
{
    QStringList out;
    QSet<QString> seen;
    QString s = text;
    s.replace(',', '\n').replace(';', '\n').replace('\t', '\n').replace('|', '\n');
    s.replace(QChar(0xFF0C), '\n').replace(QChar(0xFF1B), '\n');

    for (QString line : s.split('\n'))
    {
        line = line.trimmed();
        while (!line.isEmpty() && (line.front() == '"' || line.front() == '\'')) line.remove(0, 1);
        while (!line.isEmpty() && (line.back()  == '"' || line.back()  == '\'')) line.chop(1);
        line = line.trimmed();
        if (line.isEmpty()) continue;

        int lastSep = -1;
        for (int i = 0; i < line.size(); ++i)
        if (line[i] == '\\' || line[i] == '/') lastSep = i;
        if (lastSep >= 0) line = line.mid(lastSep + 1).trimmed();
        if (line.isEmpty()) continue;

        QString key = line.toLower();
        if (!seen.contains(key))
        {
            seen.insert(key); out << line;
        }
    }
    return out;
}