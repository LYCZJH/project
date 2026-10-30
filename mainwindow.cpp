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
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <utility>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

//线程安全暂停和保存逻辑
void Worker::checkPause()
{
    std::unique_lock<std::mutex> lock(pauseMutex_);
    if (pauseFlag_ && !stop_.load())
    {
        pauseCount_++;
        emit Paused(pauseCount_);
        //阻塞等待，直到点击继续（pauseFlag变false）或点击停止（stop变true）
        pauseCond_.wait(lock, [this]() { return !pauseFlag_ || stop_.load(); });
        if (!stop_.load())
        {
            emit Continue();
        }
    }
}

void Worker::saveContinueRecord()
{
    if (p_.ContinueFilePath.isEmpty()) return;

    QJsonObject root;
    root["taskId"] = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

    QJsonArray srcArray;
    for (const auto& src : p_.src_files) srcArray.append(src);
    root["src_files"] = srcArray;

    root["dst_file"] = p_.dst_file;

    QJsonArray namesArray;
    for (const auto& name : p_.names) namesArray.append(name);
    root["names"] = namesArray;

    root["subfolder"] = p_.subfolder;
    root["fuzzy"] = p_.fuzzy;
    root["combobox"] = p_.combobox;
    root["moveMode"] = p_.moveMode;
    root["hidden"] = p_.hidden;
    root["system"] = p_.system;

    QJsonArray processedArray;
    for (const auto& file : p_.processedFiles) processedArray.append(file);
    root["processedFiles"] = processedArray;

    QFile file(p_.ContinueFilePath);
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.close();
    }
}

//源可读/目标可创建/探测文件
bool Worker::preflight()
{
// 源文件夹
    for (const QString& src : std::as_const(p_.src_files))
    {
        QFileInfo srcInfo(src);
        if (!srcInfo.isDir() || !srcInfo.isReadable())
        {
            emit logMsg(QStringLiteral(" [警告] 源文件夹不可读: %1").arg(src));
            int d = askUser(QStringLiteral("源文件夹不存在或无读权限"), src);
            if (d == Abort) return false;
            srcInfo.refresh();
            if (!srcInfo.isDir() || !srcInfo.isReadable()) return false;
        }
    }
//删除源文件需要写权限
    if (p_.moveMode)
    {
        for (const QString& src : std::as_const(p_.src_files))
        {
            QFileInfo srcInfo(src);
            if (!srcInfo.isWritable())
            {
                emit logMsg(QStringLiteral(" [警告] 移动模式要求源文件夹可写: %1").arg(src));
                int d = askUser(QStringLiteral("移动模式需要删除源文件，但源目录无写权限"), src);
                if (d == Abort) return false;
                srcInfo.refresh();
                if (!srcInfo.isWritable()) return false;
            }
        }
    }
//目标文件夹：创建
    QDir dstDir(p_.dst_file);
    if (!dstDir.exists() && !QDir().mkpath(p_.dst_file))
    {
        emit logMsg(QStringLiteral(" [警告] 目标文件夹创建失败: %1").arg(p_.dst_file));
        int d = askUser(QStringLiteral("目标文件夹创建失败（可能无写权限或路径非法）"), p_.dst_file);
        if (d == Abort) return false;
        dstDir = QDir(p_.dst_file);
        if (!dstDir.exists()) return false;
    }

//目标文件夹：写权限探测
    const QString probe = dstDir.filePath(QStringLiteral(".fc_probe_%1.tmp").arg(QDateTime::currentMSecsSinceEpoch()));
    {
        QFile f(probe);
        if (!f.open(QIODevice::WriteOnly))
        {
            emit logMsg(QStringLiteral(" [警告] 目标目录写入探测失败"));
            int d = askUser(QStringLiteral("目标目录无写权限（写入探测失败）"), p_.dst_file);
            if (d == Abort) return false;
            if (!f.open(QIODevice::WriteOnly)) return false;
        }
    }
    QFile::remove(probe);
    return true;
}

//扫描：手动栈式递归，支持符号链接环检测与逐目录权限隔离
bool Worker::scanPhase(QMap<QString, QStringList> &fullMap, QMap<QString, QStringList> &stemMap, QStringList &quarantine)
{
    QSet<QString> visitedDirs;
    QStringList stack;
    for (const QString& src : std::as_const(p_.src_files))//将所有源文件夹加入扫描队列
    {
        stack << src;
        visitedDirs << QFileInfo(src).canonicalFilePath();
    }
    int count = 0;
    while (!stack.isEmpty())
    {
         checkPause();
        if (stop_.load()) return false;
        const QString dirPath = stack.takeLast();
        QFileInfo di(dirPath);

        if (!di.isReadable())
        {
            quarantine << QStringLiteral("隔离目录(无权限): %1").arg(dirPath);
            emit logMsg(QStringLiteral(" [隔离] 目录无读权限，未扫描: %1").arg(dirPath));
            continue;
        }

        QDir::Filters filters = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot;
        if (p_.hidden) filters |= QDir::Hidden;
        if (p_.system) filters |= QDir::System;

        const auto entries = QDir(dirPath).entryInfoList(filters, QDir::Name);

        for (const QFileInfo &fi : entries)
        {
             checkPause();
            if (stop_.load()) return false;

            if (fi.isSymLink())
            {
                const QFileInfo target(fi.symLinkTarget());
                if (!target.exists())
                {
                    quarantine << QStringLiteral("隔离链接(断链): %1").arg(fi.absoluteFilePath());
                    continue;
                }
                if (target.isDir())
                {
                    if (!p_.subfolder) continue;
                    const QString canon = target.canonicalFilePath();
                    if (visitedDirs.contains(canon))
                    {
                        emit logMsg(QStringLiteral(" [警告] 符号链接循环: %1").arg(fi.absoluteFilePath()));
                        int d = askUser(QStringLiteral("检测到符号链接循环"), fi.absoluteFilePath());
                        if (d == Abort || stop_.load()) return false;
                        continue;
                    }
                    visitedDirs.insert(canon);
                    stack << fi.absoluteFilePath();
                    continue;
                }
//链接到文件
                if (!target.isReadable())
                {
                    quarantine << QStringLiteral("隔离文件(无权限): %1").arg(fi.absoluteFilePath());
                    continue;
                }
                fullMap[fi.fileName().toLower()].append(fi.absoluteFilePath());
                stemMap[fi.completeBaseName().toLower()].append(fi.absoluteFilePath());
                ++count;
                continue;
            }

            if (fi.isDir())
            {
                if (!p_.subfolder) continue;
                if (!fi.isReadable())
                {
                    quarantine << QStringLiteral("隔离目录(无权限): %1").arg(fi.absoluteFilePath());
                    continue;
                }
                stack << fi.absoluteFilePath();
            }
            else if (fi.isFile())
            {
                if (!fi.isReadable())
                {
                    quarantine << QStringLiteral("隔离文件(无权限): %1").arg(fi.absoluteFilePath());
                    continue;
                }
                fullMap[fi.fileName().toLower()].append(fi.absoluteFilePath());
                stemMap[fi.completeBaseName().toLower()].append(fi.absoluteFilePath());
                ++count;
            }
        }
    }
    emit logMsg(QStringLiteral("扫描完成，共 %1 个文件").arg(count));
    return true;
}

//非法文件名检查
bool Worker::illegalName(const QString &n, QString *why) const
{
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    for (QChar c : n)
    {
        if (c.unicode() < 32 || bad.contains(c))
        {
            if (why) *why = QStringLiteral("含非法字符 '%1'").arg(c);
            return true;
        }
    }
    if (n.endsWith(QLatin1Char('.')) || n.endsWith(QLatin1Char(' ')))
    {
        if (why) *why = QStringLiteral("Windows 文件名不能以点或空格结尾");
        return true;
    }
    static const QSet<QString> reserved =
        {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")
    };
    const QString base = n.left(n.lastIndexOf(QLatin1Char('.'))).toUpper();
    if (reserved.contains(base))
    {
        if (why) *why = QStringLiteral("Windows 保留设备名");
        return true;
    }
    if (n.size() > 255)
    {
        if (why) *why = QStringLiteral("文件名超过 255 字符");
        return true;
    }
    return false;
}
//Worker线程实现
void Worker::run()
{
    if (p_.previewMode)
    {
        QStringList quarantine;
        QMap<QString, QStringList> fullmap, stemmap;

        emit logMsg("开始预览扫描（不会复制文件）...");

        //只执行扫描，不执行preflight
        if (!scanPhase(fullmap, stemmap, quarantine))
        {
            emit finished(0, 0, QStringList(), QStringList(), true);
            return;
        }

        QMap<QString, QStringList> matchedFiles;
        int found = 0;

    //遍历用户输入的名称进行匹配
        for (const QString &name : std::as_const(p_.names))
        {
            if (stop_.load()) break;

            QString why;
            if (illegalName(name, &why)) {
                emit logMsg(QStringLiteral("  [格式错误，已跳过] [%1]：%2").arg(name, why));
                continue;
            }

            QStringList paths = matchName(name, fullmap, stemmap);
            if (!paths.isEmpty())
            {
                matchedFiles.insert(name, paths);
                ++found;
            }
        }

    //发出预览完成的信号，把匹配到的结果传给主界面
        emit previewFinished(matchedFiles);

        // 结束线程运行
        emit finished(found, 0, QStringList(), QStringList(), false);
        return;
    }

    emit logMsg("开始预检...");
    QStringList quarantine, missing, failed, skipped;
    QSet<QString> copied;
    int found = 0, copiedN = 0;
    int skippedN = 0;//跳过计数
    emit logMsg(p_.moveMode ? "操作模式：移动" : "操作模式：复制");
//管道级预检
    if (!preflight())
    {
        emit finished(found, copiedN, missing, failed, true);
        return;
    }

//扫描
    emit logMsg("开始扫描文件...");
    QMap<QString, QStringList> fullmap, stemmap;
    if (!scanPhase(fullmap, stemmap, quarantine))
    {
        emit finished(found, copiedN, missing, failed, true);
        return;
    }

//匹配与复制
    const int total = p_.names.size();
    for (int i = 0; i < total; ++i)
    {
         checkPause();//处理每个文件名前响应暂停
        if (stop_.load())
        {
            emit finished(found, copiedN, missing, failed, true);
            return;
        }
        const QString &name = p_.names.at(i);
        QString why;

//拦截非法文件名
        if (illegalName(name, &why))
        {
            skipped << QStringLiteral("[%1]：%2").arg(name, why);
            emit logMsg(QStringLiteral("  [格式错误，已跳过] [%1]：%2").arg(name, why));
            ++skippedN;//累加跳过计数并实时刷新统计
            emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
            emit progress(i + 1, total);
            continue;
        }

//匹配文件
        const QStringList paths = matchName(name, fullmap, stemmap);
        if (paths.isEmpty())
        {
            missing << name;
            emit logMsg(QStringLiteral("未找到: %1").arg(name));
            emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());//实时刷新统计
            emit progress(i + 1, total);
            continue;
        }

        ++found;
        for (const QString &path : std::as_const(paths))
        {
            checkPause();
            if (stop_.load()) break;
            QFileInfo fi(path);

            if (!fi.exists() || !fi.isFile())
            {
                emit logMsg(QStringLiteral("  [警告] 扫描后文件已移动或删除: %1").arg(path));
                int d = askUser(QStringLiteral("文件在扫描后被移动/删除"), path);
                if (d == Abort || stop_.load())
                {
                    emit finished(found, copiedN, missing, failed, true);
                    return;
                }
                fi.refresh();
                if (d != Retry || !fi.exists() || !fi.isFile()) continue;
            }

//跳过已经处理过的文件（断点继续跑）
            if (p_.processedFiles.contains(fi.absoluteFilePath()))
            {
                emit logMsg(QStringLiteral("  [跳过已处理] %1").arg(path));
                continue;
            }

//路径拦截
            const QString target = QDir(p_.dst_file).filePath(fi.fileName());
            if (path.size() >= 260 || QDir::cleanPath(target).size() >= 260)
            {
                quarantine << QStringLiteral("路径超长: %1").arg(path);
                emit logMsg(QStringLiteral("  [隔离] 路径超过 260 字符限制: %1").arg(path));
                continue;
            }

            const QString absSrc = fi.absoluteFilePath();
            if (copied.contains(absSrc)) continue;
            if (QFileInfo(target).absoluteFilePath() == absSrc) continue;

            QString realTarget = target;
            if (QFile::exists(realTarget))
            {
                if (p_.combobox == 2)
                {
                    emit logMsg(QStringLiteral("  [跳过] %1").arg(fi.fileName()));
                    ++skippedN;//冲突跳过计入跳过数
                    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
                    continue;
                }
                else if (p_.combobox == 1)
                {
                    //覆盖模式下删除失败，支持"重试/跳过/中止"
                    bool removed = QFile::remove(realTarget);
                    while (!removed && !stop_.load())
                    {
                        emit logMsg(QStringLiteral("  [覆盖失败] 目标文件可能被占用: %1").arg(realTarget));
                        int d = askUser(QStringLiteral("覆盖失败，目标文件可能被占用或权限临时不足"), realTarget);
                        if (d == Abort || stop_.load())
                        {
                            failed << path;
                            emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
                            emit finished(found, copiedN, missing, failed, true);
                            return;
                        }
                        if (d == Skip) break;
                        QThread::msleep(200);
                        removed = QFile::remove(realTarget);
                    }

                    if (!removed)
                    {
                        quarantine << QStringLiteral("目标被占用: %1").arg(realTarget);
                        continue;
                    }
                }                
                else
                {
                    realTarget = uniquePath(p_.dst_file, fi.fileName());
                }
            }
//判断磁盘空间
                qint64 fileSize = QFileInfo(path).size();//获取准备复制的文件大小
                QStorageInfo si(p_.dst_file);//获取目标文件夹所在的磁盘信息

                if (si.isValid() && si.bytesAvailable() >= 0 && (quint64)si.bytesAvailable() < (quint64)fileSize)
                {
                    emit logMsg(QStringLiteral("  [警告] 磁盘空间不足: %1（需 %2 字节）").arg(target).arg(fileSize));
                    int d = askUser(QStringLiteral("目标磁盘剩余空间不足（本文件需要 %1 字节）").arg(fileSize), target);

                    if (d == Abort || stop_.load())
                    {
                        emit finished(found, copiedN, missing, failed, true);
                        return;
                    }
                    si.refresh();
                    if (d != Retry || si.bytesAvailable() < fileSize)
                    {
                        ++skippedN;
                    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());//磁盘不足导致跳过也计入
                        continue;
                    }                    // 跳过该文件
                }

//复制文件，失败重试
                bool copyOk   = false;
                bool giveUp   = false;
                bool aborted  = false;
                while (!copyOk && !giveUp && !stop_.load())
                {
                    if (QFile::copy(path, realTarget))
                    {
                        copyOk = true;
                    }
                    else
                    {
                        emit logMsg(QStringLiteral("  [复制失败] %1（可能被占用或权限临时不足）").arg(path));
                        int d = askUser(QStringLiteral("复制失败，文件可能被占用或权限临时不足"), path);
                        if (d == Abort || stop_.load())
                        {
                            failed << path;
                            aborted = true;
                            break;
                        }
                        if (d == Skip)
                        {
                            giveUp = true;    //跳过该文件
                        }
                        else
                        {
                            QThread::msleep(200);   // Retry前等待
                        }
                    }
                }

                if (aborted)
                {
                    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
                    emit finished(found, copiedN, missing, failed, true);
                    return;
                }

                if (copyOk)
                {
                    copied.insert(absSrc);
                    ++copiedN;

                    p_.processedFiles.insert(absSrc);//处理成功后，记录并实时保存断点
                    saveContinueRecord();

                    if (p_.moveMode && !QFile::remove(absSrc))
                        emit logMsg(QStringLiteral("  [警告] 已复制到目标，但删除源文件失败: %1").arg(absSrc));
                    emit logMsg(QString("  [成功] %1 -> %2").arg(path, realTarget));
                }
                else if (giveUp)
                {
                    failed << path;
                    emit logMsg(QString("  [已跳过失败] %1").arg(path));
                }
        }

        emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());//每个文件名处理完刷新统计
        emit progress(i + 1, total);//进度条
    }

    if (!quarantine.isEmpty())
    {
        emit logMsg(QStringLiteral("共有 %1 个文件/目录被隔离忽略。").arg(quarantine.size()));
    }

    if (!p_.ContinueFilePath.isEmpty() && QFile::exists(p_.ContinueFilePath))
    {
        QFile::remove(p_.ContinueFilePath);
        emit logMsg("任务已全部完成，已清理断点记录。");
    }

    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size()); //结束时刷新一次最终统计
    emit finished(found, copiedN, missing, failed, false);

}

int Worker::askUser(const QString& text, const QString& target)
{
    int result = 0;  //返回值0跳过 1重试 2中止
    emit askSignal(text, target, &result); //使用 BlockingQueuedConnection，信号发出后会阻塞在子线程
    return result;   //直到主线程弹框返回把结果写入 result
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
    resize(1000,900);//修改大小
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

    auto* saveListBtn = new QPushButton("保存列表");
    connect(saveListBtn, &QPushButton::clicked, this, &MainWindow::saveList);
    row2->addWidget(saveListBtn);
    auto* loadListBtn = new QPushButton("加载列表");
    connect(loadListBtn, &QPushButton::clicked, this, &MainWindow::loadList);
    row2->addWidget(loadListBtn);


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

    // 预览区域容器
    auto* rowPreview = new QHBoxLayout;
    auto* previewBtn = new QPushButton("预览匹配结果");
    connect(previewBtn, &QPushButton::clicked, this, &MainWindow::onPreviewClicked);
    rowPreview->addWidget(previewBtn);
    rowPreview->addStretch();
    main->addLayout(rowPreview);

    // 预览结果显示控件
    previewListWidget = new QListWidget(this);
    previewListWidget->setMaximumHeight(150); // 限制高度
    previewListWidget->setVisible(false);     // 初始隐藏
    main->addWidget(previewListWidget);

    //目标文件夹
    auto* row3 = new QHBoxLayout;
    row3->addWidget(new QLabel("目标文件夹:"));
    dstEdit = new QLineEdit("D:\\");
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


//复制/移动模式选择
    row4->addWidget(new QLabel("操作模式:"));
    modeCombo = new QComboBox;
    modeCombo->addItems({"复制", "移动"});
    modeCombo->setToolTip("移动");
    row4->addWidget(modeCombo);

    hiddenChk = new QCheckBox("包含隐藏文件");
    hiddenChk->setToolTip("包含带隐藏属性的文件");
    row4->addWidget(hiddenChk);
    systemChk = new QCheckBox("包含系统文件");
    systemChk->setToolTip("包含带系统属性的文件");
    row4->addWidget(systemChk);

    row4->addStretch();
    main->addLayout(row4);


    //开始按钮和进度
    auto* row5 = new QHBoxLayout;
    startBtn = new QPushButton("开始");
    connect(startBtn, &QPushButton::clicked, this, &MainWindow::start);
    row5->addWidget(startBtn);

    pauseBtn = new QPushButton("暂停");
    pauseBtn->setEnabled(false);
    connect(pauseBtn, &QPushButton::clicked, this, &MainWindow::onPauseClicked);
    row5->addWidget(pauseBtn);

    continueBtn = new QPushButton("继续");
    continueBtn->setEnabled(false);
    connect(continueBtn, &QPushButton::clicked, this, &MainWindow::onContinueClicked);
    row5->addWidget(continueBtn);

    stopBtn = new QPushButton("停止");
    stopBtn->setEnabled(false);
    connect(stopBtn, &QPushButton::clicked, [this]
            {
                if (worker_) worker_->requestStop();
            });
    row5->addWidget(stopBtn);

    // 把状态标签移到按钮组后面，防止它撑开按钮之间的间距
    pauseStatusLabel = new QLabel("");
    // 给标签设定一个固定的最小宽度，防止出现文字时导致界面抖动
    pauseStatusLabel->setMinimumWidth(100);
    row5->addWidget(pauseStatusLabel);



    // //暂停按钮
    // pauseBtn = new QPushButton("暂停");
    // pauseBtn->setEnabled(false);
    // connect(pauseBtn, &QPushButton::clicked, this, &MainWindow::onPauseClicked);
    // row5->addWidget(pauseBtn);
    // //继续按钮
    // continueBtn = new QPushButton("继续");
    // continueBtn->setEnabled(false);
    // connect(continueBtn, &QPushButton::clicked, this, &MainWindow::onContinueClicked);
    // row5->addWidget(continueBtn);

    // pauseStatusLabel = new QLabel("");
    // row5->addWidget(pauseStatusLabel);
    // //停止按钮
    // stopBtn = new QPushButton("停止");
    // stopBtn->setEnabled(false);
    // connect(stopBtn, &QPushButton::clicked, [this]
    // {
    //     if (worker_) worker_->requestStop();
    // });
    // row5->addWidget(stopBtn);

    progress = new QProgressBar;
    row5->addWidget(progress, 1);
    statusLabel = new QLabel("就绪");
    row5->addWidget(statusLabel);
    main->addLayout(row5);

    //结果统计面板
    auto* statBox = new QGroupBox("结果统计");
    auto* statLayout = new QHBoxLayout(statBox);
    statLayout->setSpacing(10);
    auto makeStatItem = [&](const QString& title, QLabel*& label) -> QWidget*
    {
        QWidget* w = new QWidget;
        QVBoxLayout* v = new QVBoxLayout(w);
        v->setContentsMargins(6, 2, 6, 2);
        v->setSpacing(0);
        QLabel* t = new QLabel(title);
        t->setAlignment(Qt::AlignCenter);
        QFont tf = t->font(); tf.setPointSize(9); t->setFont(tf);
        t->setStyleSheet("color:#666;");
        label = new QLabel("0");
        label->setAlignment(Qt::AlignCenter);
        QFont lf = label->font(); lf.setBold(true); lf.setPointSize(14); label->setFont(lf);
        label->setMinimumWidth(60);
        v->addWidget(t);
        v->addWidget(label);
        return w;
    };
    statLayout->addWidget(makeStatItem("匹配文件数", matchCountLabel));
    statLayout->addWidget(makeStatItem("复制成功",   copiedCountLabel));
    statLayout->addWidget(makeStatItem("跳过",       skippedCountLabel));
    statLayout->addWidget(makeStatItem("未找到",     missingCountLabel));
    statLayout->addWidget(makeStatItem("失败",       failedCountLabel));
    statLayout->addStretch();

    //成功绿，跳过橙，未找到/失败红
    copiedCountLabel->setStyleSheet("color:#2e7d32;");
    skippedCountLabel->setStyleSheet("color:#ef6c00;");
    missingCountLabel->setStyleSheet("color:#c62828;");
    failedCountLabel->setStyleSheet("color:#c62828;");
    main->addWidget(statBox);

    //日志
    auto* rowLog = new QHBoxLayout;
    rowLog->addWidget(new QLabel("日志:"));
    rowLog->addStretch();

    auto* saveReportBtn = new QPushButton("保存日志");
    connect(saveReportBtn, &QPushButton::clicked, this, &MainWindow::saveReport);
    rowLog->addWidget(saveReportBtn);
    main->addLayout(rowLog);

    //日志区域改为 TabWidget（运行日志 / 未找到 / 失败）
    logTabWidget = new QTabWidget(this);

    logEdit = new QTextEdit;
    logEdit->setReadOnly(true);
    logEdit->setFont(QFont("Consolas", 9));

    logTabWidget->addTab(logEdit, "运行日志");

    missingListWidget = new QListWidget;
    missingListWidget->setFont(QFont("Consolas", 9));
    logTabWidget->addTab(missingListWidget, "未找到 (0)");

    failedListWidget = new QListWidget;
    failedListWidget->setFont(QFont("Consolas", 9));
    logTabWidget->addTab(failedListWidget, "失败 (0)");

    logEdit->setMinimumHeight(180);
    main->addWidget(logTabWidget, 2);
}

MainWindow::~MainWindow()
{
    if (thread_)
    {
        if (worker_)
            worker_->requestStop();
        thread_->quit();
        thread_->wait();
        delete worker_;
        worker_ = nullptr;
        delete thread_;
        thread_ = nullptr;
    }
}

void MainWindow::onPauseClicked()
{
    if (worker_)
    {
        worker_->requestPause();
        pauseBtn->setEnabled(false);
        continueBtn->setEnabled(true);
        statusLabel->setText("已暂停");
    }
}

void MainWindow::onContinueClicked()
{
    if (worker_)
    {
        worker_->requestContinue();
        pauseBtn->setEnabled(true);
        continueBtn->setEnabled(false);
        statusLabel->setText("运行中...");
        pauseStatusLabel->setText("");
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
    p.src_files.append(src);//单源路径放入多源数组非预览模式
    p.dst_file = dst;
    p.names = names;
    p.subfolder = recurseChk->isChecked();
    p.fuzzy = fuzzyChk->isChecked();
    p.combobox = conflictCombo->currentIndex();
    p.moveMode = (modeCombo->currentIndex() == 1);

    p.hidden = hiddenChk->isChecked();
    p.system = systemChk->isChecked();

    p.previewMode = false; //不是预览模式

    //断点继续检查是否存在记录
    p.ContinueFilePath = QDir(dst).filePath(".fc_resume.json");
    if (QFile::exists(p.ContinueFilePath))
    {
        if (QMessageBox::question(this, "发现断点", "检测到目标文件夹有未完成的运行记录，是否继续上次任务？", QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
        {
            QFile file(p.ContinueFilePath);
            if (file.open(QIODevice::ReadOnly))
            {
                QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                QJsonObject root = doc.object();
                QJsonArray arr = root["processedFiles"].toArray();
                for (int i = 0; i < arr.size(); ++i)
                {
                    p.processedFiles.insert(arr[i].toString());
                }
                logEdit->append(QString("已加载断点记录，将跳过 %1 个已处理文件。").arg(p.processedFiles.size()));
            }
        }
        else
        {
            QFile::remove(p.ContinueFilePath);//如果不继续，就覆盖当前参数，删除旧记录
        }
    }

    lastDst_ = dst;

    logEdit->clear();
    progress->setValue(0);
    statusLabel->setText("运行中...");
    startBtn->setEnabled(false);
    stopBtn->setEnabled(true);

    pauseBtn->setEnabled(true);//启用暂停按钮
    continueBtn->setEnabled(false);
    pauseStatusLabel->setText("");

    //重置统计与列表
    matchCountLabel->setText("0");
    copiedCountLabel->setText("0");
    skippedCountLabel->setText("0");
    missingCountLabel->setText("0");
    failedCountLabel->setText("0");
    missingListWidget->clear();
    failedListWidget->clear();
    logTabWidget->setTabText(1, "未找到 (0)");
    logTabWidget->setTabText(2, "失败 (0)");
    logTabWidget->setCurrentIndex(0); // 切回运行日志页
    previewListWidget->clear();  //清空上次的预览列表
    previewListWidget->setVisible(false);

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

    //连接统计面板更新信号
    connect(worker_, &Worker::statsUpdated, this, [this](int matched, int copied, int skipped, int missing, int failed)
            {
        matchCountLabel->setText(QString::number(matched));
        copiedCountLabel->setText(QString::number(copied));
        skippedCountLabel->setText(QString::number(skipped));
        missingCountLabel->setText(QString::number(missing));
        failedCountLabel->setText(QString::number(failed));

        logTabWidget->setTabText(1, QString("未找到 (%1)").arg(missing));
        logTabWidget->setTabText(2, QString("失败 (%1)").arg(failed));
    });

    connect(worker_, &Worker::finished, this, &MainWindow::onFinished);

    //接收暂停状态更新，显示暂停次数
    connect(worker_, &Worker::Paused, this, [this](int count)
    {
        pauseStatusLabel->setText(QString("(已暂停 %1 次)").arg(count));
    });
    connect(worker_, &Worker::Continue, this, [this]()
    {
        pauseStatusLabel->setText("");
    });

// 跨线程询问：BlockingQueuedConnection 让子线程等待主线程结果
    connect(worker_, &Worker::askSignal, this,[this](const QString& text, const QString& target, int* result)
            {
                QMessageBox box(QMessageBox::Warning, "处理异常",text + "\n\n文件: " + QFileInfo(target).fileName(),QMessageBox::NoButton, this);
                QPushButton* btnRetry = box.addButton("重试",    QMessageBox::AcceptRole);
                QPushButton* btnSkip  = box.addButton("跳过此文件", QMessageBox::RejectRole);
                QPushButton* btnAbort = box.addButton("中止全部", QMessageBox::DestructiveRole);
                box.setDefaultButton(btnRetry);
                box.exec();

        if(box.clickedButton() == btnRetry)*result = Worker::Retry;
        else if(box.clickedButton() == btnSkip)*result = Worker::Skip;
        else*result = Worker::Abort;
    },
            Qt::BlockingQueuedConnection);

    thread_->start();
}

void MainWindow::onFinished(int found, int copied, QStringList missing, QStringList failed, bool aborted)
{
    lastMissing_ = missing;
    lastFailed_  = failed;


    missingListWidget->clear(); //把数据填充到下方的 Tab 列表中
    missingListWidget->addItems(missing);
    failedListWidget->clear();
    failedListWidget->addItems(failed);


    startBtn->setEnabled(true);
    stopBtn->setEnabled(false);

    pauseBtn->setEnabled(false); //运行结束，禁用暂停按钮
    continueBtn->setEnabled(false);
    pauseStatusLabel->setText("");

    statusLabel->setText(aborted ? "已停止" : "完成");
    progress->setValue(0);
    if(thread_)
    {
        thread_->quit();
        thread_->wait();
        delete worker_;
        worker_ = nullptr;
        delete thread_;
        thread_ = nullptr;
    }


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

void MainWindow::saveReport()
{
    if (logEdit->toPlainText().isEmpty() && lastMissing_.isEmpty() && lastFailed_.isEmpty())
    {
        QMessageBox::information(this, "提示", "当前没有日志和清单可保存。\n请先运行一次任务。");
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString defPath = lastDst_.isEmpty() ? QString("文件筛选报告_%1.txt").arg(stamp): QDir(lastDst_).filePath(QString("文件筛选报告_%1.txt").arg(stamp));

    QString path = QFileDialog::getSaveFileName(this, "保存日志与清单", defPath, "文本文件 (*.txt);;所有文件 (*)");
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "错误", "无法写入文件。");
        return;
    }

    QTextStream ts(&f);
    ts << "运行日志\n"
    << logEdit->toPlainText() << "\n\n";

    ts << "未找到清单 (共 " << lastMissing_.size() << " 项)\n";
    for (const QString &s : lastMissing_)
    ts << s << "\n";
    ts << "\n";

    ts << "复制失败清单 (共 " << lastFailed_.size() << " 项)\n";
    for (const QString &s : lastFailed_)
    ts << s << "\n";

    f.close();
    QMessageBox::information(this, "成功", "日志与清单已保存:\n" + path);
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

//保存列表
void MainWindow::saveList()
{
    QString path = QFileDialog::getSaveFileName(this, "保存文件名列表", "", "文本文件 (*.txt);;所有文件 (*)");
    if (!path.isEmpty())
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(namesEdit->toPlainText().toUtf8());
            QMessageBox::information(this, "成功", "文件列表已保存。");
        } else {
            QMessageBox::warning(this, "错误", "无法写入文件。");
        }
    }
}

//加载列表
void MainWindow::loadList()
{
    QString path = QFileDialog::getOpenFileName(this, "打开文件名列表", "", "文本文件 (*.txt);;所有文件 (*)");
    if (!path.isEmpty())
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            namesEdit->setPlainText(file.readAll());
            updateCount(); // 刷新计数
        }
        else
        {
            QMessageBox::warning(this, "错误", "无法读取文件。");
        }
    }
}

//预览匹配结果点击事件
void MainWindow::onPreviewClicked()
{
//清空旧数据
    previewListWidget->clear();
    previewListWidget->setVisible(false);

    // 验证输入
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

    logEdit->append("正在预览匹配结果...");
    startBtn->setEnabled(false);
    stopBtn->setEnabled(false);

//构建预览参数
    Worker::Params p;
    p.src_files.append(src);
    p.names = names;
    p.subfolder = recurseChk->isChecked();
    p.fuzzy = fuzzyChk->isChecked();
    p.combobox = conflictCombo->currentIndex();
    p.previewMode = true;

//使用事件循环等待预览完成
    QEventLoop loop;
    Worker *previewWorker = new Worker(p);
    QThread *previewThread = new QThread;

    previewWorker->moveToThread(previewThread);

    connect(previewThread, &QThread::started, previewWorker, &Worker::run);

// 连接信号：当收到预览结果时，弹出提示并恢复UI
    connect(previewWorker, &Worker::previewFinished, this, [this](const QMap<QString, QStringList>& files)
            {
        QStringList matchedNames;
        for (const QString& name : files.keys())
        {
            matchedNames << name;
        }

//延迟一点让日志先刷出来
        QTimer::singleShot(100, [this, matchedNames]()
        {
            this->previewListWidget->setVisible(true);
            this->previewListWidget->addItems(matchedNames);
            QMessageBox::information(this, "预览完成", QString("找到 %1 个匹配的文件名。\n请查看下方列表。").arg(matchedNames.size()));
            this->startBtn->setEnabled(true);
        });
    });

//补充连接askSignal防止预览时遇到异常
    connect(previewWorker, &Worker::askSignal, this,[this](const QString& text, const QString& target, int* result)
            {
        QMessageBox box(QMessageBox::Warning, "处理异常",text + "\n\n文件: " + QFileInfo(target).fileName(),QMessageBox::NoButton, this);
        QPushButton* btnRetry = box.addButton("重试",    QMessageBox::AcceptRole);
        QPushButton* btnSkip  = box.addButton("跳过此文件", QMessageBox::RejectRole);
        QPushButton* btnAbort = box.addButton("中止全部", QMessageBox::DestructiveRole);
        box.setDefaultButton(btnRetry);
        box.exec();
                if (box.clickedButton() == btnRetry) *result = Worker::Retry;
                else if (box.clickedButton() == btnSkip) *result = Worker::Skip;
                else *result = Worker::Abort;
            }, Qt::BlockingQueuedConnection);

//连接信号：当线程结束时结束事件循环
    connect(previewWorker, &Worker::finished, &loop, &QEventLoop::quit);
    connect(previewWorker, &Worker::logMsg, logEdit, &QTextEdit::append);

    previewThread->start();
    loop.exec(); //阻塞在这里，直到预览完成

    previewThread->quit();
    previewThread->wait();
    delete previewWorker;
    delete previewThread;
}