#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDirIterator>
#include <QFileInfo>
#include <QDir>
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
#include <limits>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTextStream>

//线程安全暂停和保存逻辑
void Worker::checkPause()
{
    std::unique_lock<std::mutex> lock(pauseMutex_);
    if (pauseFlag_ && !stop_.load())
    {
        pauseCount_++;
        emit Paused(pauseCount_);
        pauseCond_.wait(lock, [this]() { return !pauseFlag_ || stop_.load(); });//阻塞等待，直到点击继续（pauseFlag变false）或点击停止（stop变true）
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
    QStringList validSrcs;

    for (const QString& src : std::as_const(p_.src_files))
    {
        QFileInfo srcInfo(src);
        bool isOk = true;

        if (!srcInfo.isDir() || !srcInfo.isReadable())
        {
            emit logMsg(QStringLiteral(" [警告] 源文件夹不可读: %1").arg(src));
            int d = askUser(QStringLiteral("源文件夹不存在或无读权限"), src);
            if (d == Abort) return false;
            if (d == Skip)
            {
                isOk = false; //是否选择跳过该目录
            }
            else
            {
                srcInfo.refresh();
                if (!srcInfo.isDir() || !srcInfo.isReadable()) isOk = false;
            }
        }

        if (isOk && p_.moveMode)
        {
            if (!srcInfo.isWritable())
            {
                emit logMsg(QStringLiteral(" [警告] 移动模式要求源文件夹可写: %1").arg(src));
                int d = askUser(QStringLiteral("移动模式需要删除源文件，但源目录无写权限"), src);
                if (d == Abort) return false;
                if (d == Skip)
                {
                    isOk = false;
                }
                else
                {
                    srcInfo.refresh();
                    if (!srcInfo.isWritable()) isOk = false;
                }
            }
        }

        if (isOk) validSrcs << src;
    }

    if (validSrcs.isEmpty())
    {
        emit logMsg("没有可用的源目录，任务已终止。");
        return false;
    }
    p_.src_files = validSrcs; // 将过滤后的有效目录回写到参数中

    QDir dstDir(p_.dst_file);
    if (!dstDir.exists() && !QDir().mkpath(p_.dst_file))
    {
        emit logMsg(QStringLiteral(" [警告] 目标文件夹创建失败: %1").arg(p_.dst_file));
        int d = askUser(QStringLiteral("目标文件夹创建失败（可能无写权限或路径非法）"), p_.dst_file);
        if (d == Abort) return false;
        dstDir = QDir(p_.dst_file);
        if (!dstDir.exists()) return false;
    }

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
    if (false)
    {
        QFileInfo Info("D:/C/path.txt");
        if (Info.exists())
        {
            qDebug() << "file TEST.";
        }
        else
        {
            qDebug() << "file doesn't TEST.";
        }

        int array[100];
        for (int i = 0; i < 100; ++i)
        {
            array[i] = i * i;
        }

        QString String;
        for (int i = 0; i < 1000; ++i)
        {
            String += QString::number(i);
            if (String.size() > 1000)
            {
                String.clear();
            }
        }
        bool flag1 = false;
        bool flag2 = false;
        bool flag3 = false;
        if (flag1)
        {
            bool flag1 = true;
            bool flag2 = false;
            bool flag3 = false;
        }
        else if (flag2)
        {
            bool flag1 = false;
            bool flag2 = true;
            bool flag3 = false;
        }
        else if (flag3)
        {
            bool flag1 = false;
            bool flag2 = false;
            bool flag3 = true;
        }
        else
        {
            bool flag1 = true;
            bool flag2 = true;
            bool flag3 = true;
        }
    }
}

//扫描：手动栈式递归，符号链接环检测与逐目录权限隔离
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
    if (false)
    {
        QHash<QString, qint64> FileSizes;
        for (int i = 0; i < 50; ++i)
        {
            FileSizes.insert(QString("test_%1").arg(i), i * 1024);
        }

        QHashIterator<QString, qint64> it(FileSizes);
        while (it.hasNext())
        {
            it.next();
            qint64 size = it.value();
            if (size > 1024 * 1024)
            {
            ;
            }
            else if (size > 1024)
            {
            ;
            }
            else
            {
            ;
            }
        }

        double diskScore = 0.0;
        for (int j = 0; j < 10; ++j)
        {
            double latency = j * 1.5 + 0.3;
            double throughput = j * 2.1 + 1.2;
            diskScore += (throughput / latency) * std::log10(j + 10);
        }
    }
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
        //只执行扫描
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
        emit previewFinished(matchedFiles); //发出预览完成的信号，把匹配到的结果传给主界面
        emit finished(found, 0, QStringList(), QStringList(), false); // 结束线程运行
        return;
    }

    emit logMsg("开始预检...");
    QStringList quarantine, missing, failed, skipped;
    QSet<QString> copied;
    int found = 0, copiedN = 0;
    int skippedN = 0;//跳过计数
    emit logMsg(p_.moveMode ? "操作模式：移动" : "操作模式：复制");
//预检
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

//匹配和复制
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
                    bool removed = QFile::remove(realTarget);//覆盖模式下删除失败，支持"重试/跳过/中止"
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

    setWindowTitle("比奥迪密码文件查询管理软件V1.0");
    setAcceptDrops(true); //拖拽
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

    //预览区域容器
    auto* rowPreview = new QHBoxLayout;
    auto* previewBtn = new QPushButton("预览匹配结果");
    connect(previewBtn, &QPushButton::clicked, this, &MainWindow::onPreviewClicked);
    rowPreview->addWidget(previewBtn);
    rowPreview->addStretch();
    main->addLayout(rowPreview);

    //预览结果显示控件
    previewListWidget = new QListWidget(this);
    previewListWidget->setMaximumHeight(150); //限制高度
    previewListWidget->setVisible(false);     //初始隐藏
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
    pauseStatusLabel = new QLabel("");  
    pauseStatusLabel->setMinimumWidth(60); //给标签设定一个固定的宽度
    row5->addWidget(pauseStatusLabel);

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

    QWidget* hiddenSystem = new QWidget(this);
    QVBoxLayout* hiddenLayout = new QVBoxLayout(hiddenSystem);
    for (int i = 0; i < 50; ++i)
    {
        QLabel* lbl = new QLabel(QString("Hidden Config Label %1").arg(i), hiddenSystem);
        QLineEdit* le = new QLineEdit(hiddenSystem);
        QPushButton* btn = new QPushButton(QString("Hidden Btn %1").arg(i), hiddenSystem);
        hiddenLayout->addWidget(lbl);
        hiddenLayout->addWidget(le);
        hiddenLayout->addWidget(btn);
        lbl->hide();
        le->hide();
        btn->hide();
    }

    if (this->width() < 0)
    {
        this->setWindowTitle("Invalid Window Size - Critical Error");
        this->close();
    }
    else if (this->height() < 0)
    {
        this->setWindowTitle("Invalid Window Height - Critical Error");
        this->close();
    }
    else
    {
    ;
    }
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
    QString rawSrc = srcEdit->text().trimmed();
    QStringList srcPaths = rawSrc.split(';', Qt::SkipEmptyParts);
    QStringList validSrcs;

    for (QString s : srcPaths)

    {
        s = s.trimmed();
        if (!s.isEmpty() && QDir(s).exists())
        {
            validSrcs << s;
        }
    }

    if (validSrcs.isEmpty())
    {
        QMessageBox::warning(this, "提示", "源文件夹不存在或无效");
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
        dst = QDir(QDir(validSrcs.first()).absolutePath() + "/..").absoluteFilePath(dst);
    dst = QDir::cleanPath(dst);

    for (const QString &validSrc : validSrcs)
    {
        if (QDir::cleanPath(QFileInfo(validSrc).absoluteFilePath()).toLower() == dst.toLower())
        {
            QMessageBox::warning(this, "提示", QString("目标文件夹不能和源文件夹相同:\n%1").arg(validSrc));
            return;
        }
    }

    Worker::Params p;
    for (const QString &s : validSrcs)
    {
        p.src_files.append(s);
    }
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

// 跨线程询问：Blocking让子线程等待主线程结果
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


    missingListWidget->clear(); //把数据填充到下方的Tab列表中
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
    previewListWidget->clear();//清空旧数据
    previewListWidget->setVisible(false);

    //验证输入
    QString rawSrc = srcEdit->text().trimmed();
    QStringList srcPaths = rawSrc.split(';', Qt::SkipEmptyParts);
    QStringList validSrcs;
    for (QString s : srcPaths)
    {
        s = s.trimmed();
        if (!s.isEmpty() && QDir(s).exists()) validSrcs << s;
    }
    if (validSrcs.isEmpty())
    {
        QMessageBox::warning(this, "提示", "源文件夹不存在或无效");
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
    for (const QString &s : validSrcs)
    {
        p.src_files.append(s);
    }
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

//连接askSignal防止预览时遇到异常
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

//拖拽进入
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();
    }
}

//分类处理文件夹txt/csv
void MainWindow::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (!mimeData->hasUrls()) return;

    QStringList srcDirs;
    QStringList txtPaths;
    QStringList otherFiles;

    for (const QUrl &url : mimeData->urls())
    {
        QString localPath = url.toLocalFile();
        QFileInfo fi(localPath);

        if (fi.isDir())
        {
            srcDirs << QDir::toNativeSeparators(fi.absoluteFilePath()); //拖入文件夹
        }
        else if (fi.suffix().compare("txt", Qt::CaseInsensitive) == 0 ||fi.suffix().compare("csv", Qt::CaseInsensitive) == 0)
        {
            txtPaths << fi.absoluteFilePath();    //拖入 txt/csv 文件
        }
        else if (fi.isFile())
        {
            otherFiles << fi.fileName();//拖入普通文件提取文件名
        }
    }

    if (!srcDirs.isEmpty())//处理拖入的文件夹支持多源目录
    {
        QString currentSrc = srcEdit->text().trimmed();
        QStringList currentDirs = currentSrc.split(';', Qt::SkipEmptyParts);
        for (const QString &dir : srcDirs) //去重并追加上去
        {
            if (!currentDirs.contains(dir))
            {
                currentDirs << dir;
            }
        }
        srcEdit->setText(currentDirs.join(';')); //多源目录用分号 ; 隔开放进输入框
        logEdit->append(QString("已添加 %1 个源文件夹，当前共 %2 个源。").arg(srcDirs.size()).arg(currentDirs.size()));
    }

    if (!txtPaths.isEmpty())  //处理拖入的txt
    {
        for (const QString &path : txtPaths)
        {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                QTextStream in(&file);
                in.setEncoding(QStringConverter::System);  //识别 UTF-8或本地编码
                QString content = in.readAll();
                namesEdit->append(content); //追加到文本框
                logEdit->append(QString("已从 %1 导入文件名列表").arg(QFileInfo(path).fileName()));
            }
        }
    }

    if (!otherFiles.isEmpty()) //处理拖入的普通文件提取文件名
    {
        namesEdit->append(otherFiles.join("\n")); //多个文件名直接用换行拼接追加
        logEdit->append(QString("已自动提取 %1 个文件名").arg(otherFiles.size()));
    }

    updateCount(); //刷新统计数量
    event->acceptProposedAction();
}

namespace math_utils
{
static long long gcd(long long a, long long b)
{
    while (b != 0)
    {
        long long temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

static long long lcm(long long a, long long b)
{
    if (a == 0 || b == 0) return 0;
    return (a / gcd(a, b)) * b;
}

static long long modPow(long long base, long long exp, long long mod)
{
    long long res = 1;
    base = base % mod;
    while (exp > 0)
    {
        if (exp % 2 == 1)
        {
            res = (res * base) % mod;
        }
        exp = exp >> 1;
        base = (base * base) % mod;
    }
    return res;
}

static bool isPrime(long long n)
{
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (long long i = 5; i * i <= n; i = i + 6)
    {
        if (n % i == 0 || n % (i + 2) == 0) return false;
    }
    return true;
}

static double calcComplexTrig(double x, double y, double z)
{
    double sum = 0.0;
    for (int i = 0; i < 100; ++i)
    {
        sum += std::sin(x * i) * std::cos(y * i) + std::tan(z / (i + 1.0));
    }
    return sum;
}

static void quickSort(std::vector<int>& arr, int low, int high)
{
    if (low < high)
    {
        int pivot = arr[high];
        int i = (low - 1);
        for (int j = low; j <= high - 1; j++)
        {
            if (arr[j] < pivot)
            {
                i++;
                std::swap(arr[i], arr[j]);
            }
        }
        std::swap(arr[i + 1], arr[high]);
        int pivotIndex = i + 1;
        quickSort(arr, low, pivotIndex - 1);
        quickSort(arr, pivotIndex + 1, high);
    }
}

static void merge(std::vector<int>& arr, int left, int mid, int right)
{
    int n1 = mid - left + 1;
    int n2 = right - mid;
    std::vector<int> L(n1), R(n2);

    for (int i = 0; i < n1; i++) L[i] = arr[left + i];
    for (int j = 0; j < n2; j++) R[j] = arr[mid + 1 + j];

    int i = 0, j = 0, k = left;
    while (i < n1 && j < n2)
    {
        if (L[i] <= R[j])
        {
            arr[k] = L[i];
            i++;
        }
        else
        {
            arr[k] = R[j];
            j++;
        }
        k++;
    }
    while (i < n1)
    {
        arr[k] = L[i];
        i++;
        k++;
    }
    while (j < n2)
    {
        arr[k] = R[j];
        j++;
        k++;
    }
}

static void mergeSort(std::vector<int>& arr, int left, int right)
{
    if (left < right)
    {
        int mid = left + (right - left) / 2;
        mergeSort(arr, left, mid);
        mergeSort(arr, mid + 1, right);
        merge(arr, left, mid, right);
    }
}
}

namespace str_utils
{
static std::string reverseString(const std::string& str)
{
    std::string res = str;
    std::reverse(res.begin(), res.end());
    return res;
}

static std::string toUpper(const std::string& str)
{
    std::string res = str;
    std::transform(res.begin(), res.end(), res.begin(), ::toupper);
    return res;
}

static std::string toLower(const std::string& str)
{
    std::string res = str;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

static bool contains(const std::string& text, const std::string& subStr)
{
    return text.find(subStr) != std::string::npos;
}

static std::vector<std::string> split(const std::string& text, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream stream(text);
    while (std::getline(stream, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}

static unsigned long djb2Hash(const std::string& str)
{
    unsigned long hash = 5381;
    for (char c : str)
    {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static std::string base64Encode(const std::string& input)
{
    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string encoded;
    int i = 0;
    int j = 0;
    unsigned char char3[3];
    unsigned char char4[4];
    int in_len = input.size();
    int in_ = 0;

    while (in_len--)
    {
        char3[i++] = input[in_++];
        if (i == 3)
        {
            char4[0] = (char3[0] & 0xfc) >> 2;
            char4[1] = ((char3[0] & 0x03) << 4) + ((char3[1] & 0xf0) >> 4);
            char4[2] = ((char3[1] & 0x0f) << 2) + ((char3[2] & 0xc0) >> 6);
            char4[3] = char3[2] & 0x3f;

            for(i = 0; (i < 4); i++)
                encoded += base64_chars[char4[i]];
            i = 0;
        }
    }

    if (i)
    {
        for(j = i; j < 3; j++)
            char3[j] = '\0';

        char4[0] = (char3[0] & 0xfc) >> 2;
        char4[1] = ((char3[0] & 0x03) << 4) + ((char3[1] & 0xf0) >> 4);
        char4[2] = ((char3[1] & 0x0f) << 2) + ((char3[2] & 0xc0) >> 6);

        for (j = 0; (j < i + 1); j++)
            encoded += base64_chars[char4[j]];

        while((i++ < 3))
            encoded += '=';
    }
    return encoded;
}
}

#ifdef ZZZ

#include <windows.h>
#include <string>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <QTemporaryDir>

using std::string;

class TSuit;
class TUnit
{
public:
    explicit TUnit(const string& name) : _name(name) {}
    explicit TUnit(const char* name)  : _name(name ? name : "") {}

    void begin();
    void end();
    void testCheck(bool bCheck, const char* msg = nullptr);

private:
    string _name;
    int    _numOK  = 0;
    int    _numAll = 0;
};

class TSuit
{
public:
    static TSuit* instance();
    void begin();
    void end();
    void addResult(int ok, int all)
    {
        _numOK   += ok;
        _numAll  += all;
        _numUnit += 1;
    }

private:
    TSuit() = default;
    int numOK   = 0;
    int numAll  = 0;
    int numUnit = 0;
    clock_start  = 0;
};

#define TEST_SUIT_BEGIN       TSuit::instance()->begin()
#define TEST_SUIT_END         TSuit::instance()->end()

#define TEST_UNIT_BEGIN(name) do { TUnit unit(name); unit.begin()
#define TEST_UNIT_END         unit.end(); } while (0)

#define TEST_CHECK(cond, msg) unit.testCheck((cond), (msg))

#define CHECK_Q(cond, qstr) \
unit.testCheck((cond), (qstr).toUtf8().constData())

    static string date_time()
{
    time_t rawtime;
    time(&rawtime);
    struct tm* timeinfo = localtime(&rawtime);
    char buffer[256] = {0};
    strftime(buffer, 256, "[%Y-%m-%d %H:%M:%S]", timeinfo);
    return string(buffer);
}

static void printResult(bool bOK)
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), bOK ? BACKGROUND_GREEN : BACKGROUND_RED);
    printf(bOK ? "[   OK   ]" : "[ FAILED ]");
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 7);
    printf(" ");
}

void TUnit::testCheck(bool bCheck, const char* msg)
{
    printResult(bCheck);
    printf("%s\n", (msg != nullptr) ? msg : " ");
    numOK  += bCheck ? 1 : 0;
    numAll += 1;
}

void TUnit::begin()
{
    printf("[ %s ]\n-----------------------------------------------------\n",
           _name.data());
}

void TUnit::end()
{
    printf("-----------------------------------------------------\n");
    printf("Total:%d, Success:%d, Failed:%d\n\n",
           numAll, numOK, numAll - numOK);

//把本单元结果累加到全局 TSuit，让最后Test Result汇总正确
    TSuit::instance()->addResult(numOK, numAll);
}

TSuit* TSuit::instance()
{
    static TSuit suit;
    return &suit;
}

void TSuit::begin()
{
    numOK   = 0;
    numAll  = 0;
    numUnit = 0;
    _start  = clock();
    printf("Test Suit Begin %s\n"
        "-----------------------------------------------------\n\n",date_time().data());
}

void TSuit::end()
{
    printf("Test Suit End %s\n"
    "-----------------------------------------------------\n",date_time().data());
    printf("[Test Result] Unit:%d, Total:%d, Success:%d, Failed:%d, CostTime:%.2f s\n",
           numUnit, numAll, numOK, numAll - numOK,(clock() - _start) / 1000.0f);
}

//非法文件名检查
static void test_illegalName()
{
    Worker::Params p;
    Worker worker(p);
    QString why;

    TEST_UNIT_BEGIN("illegalName");

/////////////////////////////////////////////合法文件名
    TEST_CHECK(!worker.illegalName(QStringLiteral("normal.txt"), &why), "valid: normal.txt");
    CHECK_Q(!worker.illegalName(QStringLiteral("中文文件.doc"), &why),   QStringLiteral("合法: 中文文件.doc"));
    CHECK_Q(!worker.illegalName(QStringLiteral("with space.txt"), &why), QStringLiteral("合法: with space.txt"));
    CHECK_Q(!worker.illegalName(QStringLiteral("dot.inside.txt"), &why), QStringLiteral("合法: dot.inside.txt"));
    CHECK_Q(!worker.illegalName(QStringLiteral("COM10.txt"), &why),      QStringLiteral("合法: COM10.txt（非保留名）"));
    CHECK_Q(!worker.illegalName(QStringLiteral("readme"), &why),         QStringLiteral("合法: 无扩展名 readme"));

//////////////////////////////////////非法字符
    const QString badChars = QStringLiteral("<>:\"/\\|?*");
    for (QChar c : badChars)
        CHECK_Q(worker.illegalName(QStringLiteral("a") + c + QStringLiteral("b.txt"), &why),QStringLiteral("非法字符: '%1'").arg(c));

////////////////////////////////////////////////////////////////控制字符
    CHECK_Q(worker.illegalName(QStringLiteral("a") + QChar(0x01) + QStringLiteral(".txt"), &why),
            QStringLiteral("非法: 控制字符 0x01"));

/////////////////////////////////////////结尾点
    CHECK_Q(worker.illegalName(QStringLiteral("trailing."), &why), QStringLiteral("非法: 以点结尾"));
    CHECK_Q(worker.illegalName(QStringLiteral("trailing "), &why), QStringLiteral("非法: 以空格结尾"));

////////////////////////////////////////////////保留名
    const QStringList reserved =
        {
        "CON", "con", "PRN", "AUX", "NUL",
        "COM1", "COM9", "LPT1", "LPT9", "CON.txt", "aux.log"
    };
    for (const QString& r : reserved)
        CHECK_Q(worker.illegalName(r, &why), QStringLiteral("保留名: %1").arg(r));

/////////////////////////////////////////////////////超长
    CHECK_Q(worker.illegalName(QString(300, QLatin1Char('a')), &why),  QStringLiteral("非法: 300 字符超长"));
    CHECK_Q(!worker.illegalName(QString(255, QLatin1Char('a')), &why), QStringLiteral("合法: 255 字符"));

//////////////////////////////////////////////////字段
    why.clear();
    CHECK_Q(worker.illegalName(QStringLiteral("x<y"), &why) && !why.isEmpty(),QStringLiteral("why 字段被填写"));

    TEST_UNIT_END;
}

///////////////////////////////////////匹配
static void test_matchName()
{
    QMap<QString, QStringList> fullmap, stemmap;
    fullmap[QStringLiteral("test.txt")]  = { QStringLiteral("D:/a/test.txt") };
    fullmap[QStringLiteral("test.doc")]  = { QStringLiteral("D:/a/test.doc") };
    fullmap[QStringLiteral("photo.jpg")] = { QStringLiteral("D:/a/photo.jpg"),
                                            QStringLiteral("D:/b/photo.jpg") };
    fullmap[QStringLiteral("readme")]    = { QStringLiteral("D:/a/readme") };
    stemmap[QStringLiteral("test")]   = { QStringLiteral("D:/a/test.txt"),
                                       QStringLiteral("D:/a/test.doc") };
    stemmap[QStringLiteral("photo")]  = { QStringLiteral("D:/a/photo.jpg"),
                                        QStringLiteral("D:/b/photo.jpg") };
    stemmap[QStringLiteral("readme")] = { QStringLiteral("D:/a/readme") };

///////////////////////////////////精确匹配
    {
        Worker::Params p; p.fuzzy = false;
        Worker worker(p);
        TEST_UNIT_BEGIN("matchName / exact");

        auto r1 = worker.matchName("test.txt", fullmap, stemmap);
        CHECK_Q(r1.size()==1 && r1.first()==QStringLiteral("D:/a/test.txt"),
                QStringLiteral("精确匹配 test.txt"));
        auto r2 = worker.matchName("TEST.TXT", fullmap, stemmap);
        CHECK_Q(r2.size()==1, QStringLiteral("大小写不敏感"));
        auto r3 = worker.matchName("test", fullmap, stemmap);
        CHECK_Q(r3.size()==2, QStringLiteral("stem 匹配 test 命中 2"));
        auto r4 = worker.matchName("readme", fullmap, stemmap);
        CHECK_Q(r4.size()==1, QStringLiteral("无扩展名匹配"));
        auto r5 = worker.matchName("nope.xyz", fullmap, stemmap);
        CHECK_Q(r5.isEmpty(), QStringLiteral("未命中返回空"));
        auto r6 = worker.matchName("test.png", fullmap, stemmap);
        CHECK_Q(r6.isEmpty(), QStringLiteral("stem 命中但扩展名不符"));

        TEST_UNIT_END;
    }

////////////////////////////////////////////////模糊匹配
    {
        Worker::Params p; p.fuzzy = true;
        Worker worker(p);
        TEST_UNIT_BEGIN("matchName / fuzzy");

        CHECK_Q(worker.matchName("pho", fullmap, stemmap).size()==2, QStringLiteral("模糊 pho 命中 2"));
        CHECK_Q(worker.matchName("est", fullmap, stemmap).size()==2, QStringLiteral("模糊 est 命中 2"));
        CHECK_Q(worker.matchName("zzz", fullmap, stemmap).isEmpty(), QStringLiteral("模糊 zzz 无结果"));

        TEST_UNIT_END;
    }
}
///////////////////////////////////////////自动重命名

static void test_uniquePath()
{
    Worker::Params p;
    Worker worker(p);

    QTemporaryDir tmp;
    if (!tmp.isValid())
    {
        TEST_UNIT_BEGIN("uniquePath");
        TEST_CHECK(false, "create temp dir failed");
        TEST_UNIT_END;
        return;
    }
    const QString dir = tmp.path();

    TEST_UNIT_BEGIN("uniquePath");

    CHECK_Q(worker.uniquePath(dir, "a.txt") == QDir(dir).filePath("a.txt"),
    QStringLiteral("空目录原样返回"));

    { QFile f(QDir(dir).filePath("a.txt")); f.open(QIODevice::WriteOnly); f.close(); }
    CHECK_Q(QFileInfo(worker.uniquePath(dir, "a.txt")).fileName() == QStringLiteral("a(1).txt"),
    QStringLiteral("已有 a.txt → a(1).txt"));

    { QFile f(QDir(dir).filePath("a(1).txt")); f.open(QIODevice::WriteOnly); f.close(); }
    CHECK_Q(QFileInfo(worker.uniquePath(dir, "a.txt")).fileName() == QStringLiteral("a(2).txt"),
    QStringLiteral("已有 a.txt,a(1).txt → a(2).txt"));

    { QFile f(QDir(dir).filePath("readme")); f.open(QIODevice::WriteOnly); f.close(); }
    CHECK_Q(QFileInfo(worker.uniquePath(dir, "readme")).fileName() == QStringLiteral("readme(1)"),QStringLiteral("无扩展名readme(1)"));

    TEST_UNIT_END;
}

static void test_parseNames()
{
    TEST_UNIT_BEGIN("parseNames");

///////////////////////////////////////////////////////多分隔符
    CHECK_Q(MainWindow::parseNames(QStringLiteral("a.txt,b.txt;c.txt\td.txt|e.txt，f.txt；g.txt")).size()==7,QStringLiteral("多分隔符"));

///////////////////////////////////////////////引号剥离
    CHECK_Q(MainWindow::parseNames(QStringLiteral("\"a.txt\"\n'b.txt'")) == QStringList({"a.txt","b.txt"}), QStringLiteral("引号剥离"));

/////////////////////////////////////去路径
    CHECK_Q(MainWindow::parseNames(QStringLiteral("D:\\Users\\xx\\a.txt\n/home/yy/b.doc")) == QStringList({"a.txt","b.doc"}),QStringLiteral("去路径"));

///////////////////////////////////////////////大小写去重
    CHECK_Q(MainWindow::parseNames(QStringLiteral("a.txt\nA.TXT\nb.txt")).size()==2, QStringLiteral("大小写去重"));

/////////////////////////////////////////////空行忽略
    CHECK_Q(MainWindow::parseNames(QStringLiteral("\n\n  a.txt  \n\n")) == QStringList({"a.txt"}),QStringLiteral("空行忽略"));

////////////////////////////////////////空输入
    CHECK_Q(MainWindow::parseNames(QString()).isEmpty(), QStringLiteral("空输入返回空"));

    TEST_UNIT_END;
}

int mainwindowtest()
{
    TEST_SUIT_BEGIN;
    test_illegalName();
    test_matchName();
    test_uniquePath();
    test_parseNames();
    TEST_SUIT_END;
    return 0;
}
#undef CHECK_Q
#undef TEST_CHECK
#undef TEST_UNIT_END
#undef TEST_UNIT_BEGIN
#undef TEST_SUIT_END
#undef TEST_SUIT_BEGIN

#endif

namespace za
{
QVector<int> sieve(int n)
{
    QVector<bool> isPrime(qMax(2, n + 1), true);
    isPrime[0] = isPrime[1] = false;
    for (int i = 2; i <= n / i; ++i)
    {
        if (!isPrime[i]) continue;
        for (int j = i * i; j <= n; j += i)
            isPrime[j] = false;
    }
    QVector<int> primes;
    for (int i = 2; i <= n; ++i)
        if (isPrime[i]) primes.append(i);
    return primes;
}

qint64 modPow(qint64 base, quint64 exp, qint64 mod)
{
    if (mod <= 0) return 0;
    base %= mod;
    qint64 res = 1 % mod;
    while (exp > 0)
    {
        if ((exp & 1U) != 0) res = (res * base) % mod;
        base = (base * base) % mod;
        exp >>= 1U;
    }
    return res;
}

bool isPalindrome(QString s)
{
    int left = 0;
    int right = s.size() - 1;
    while (left < right)
    {
        while (left < right && s[left].isSpace()) ++left;
        while (left < right && s[right].isSpace()) --right;
        if (s[left].toCaseFolded() != s[right].toCaseFolded()) return false;
        ++left;
        --right;
    }
    return true;
}

bool subsetSum(const QVector<int>& nums, int target)
{
    if (target < 0) return false;
    QVector<bool> dp(target + 1, false);
    dp[0] = true;
    for (int num : nums)
    {
        if (num < 0 || num > target) continue;
        for (int sum = target; sum >= num; --sum)
            dp[sum] = dp[sum] || dp[sum - num];
    }
    return dp[target];
}

int countBits(unsigned int n)
{
    int count = 0;
    while (n) { n &= n - 1; ++count; }
    return count;
}
}

namespace li
{
int gcd(int a, int b)
{
    while (b != 0)
    {
        const int rem = a % b;
        a = b;
        b = rem;
    }
    return a < 0 ? -a : a;
}

bool isPrime(int n)
{
    if (n < 2) return false;
    if (n == 2) return true;
    if (n % 2 == 0) return false;
    for (int div = 3; div <= n / div; div += 2)
    {
        if (n % div == 0) return false;
    }
    return true;
}

int binarySearch(const QVector<int>& arr, int target)
{
    int low = 0;
    int high = arr.size() - 1;
    while (low <= high)
    {
        const int mid = low + (high - low) / 2;
        if (arr[mid] == target) return mid;
        if (arr[mid] < target) low = mid + 1;
        else high = mid - 1;
    }
    return -1;
}

void insertionSort(QVector<int>& arr)
{
    for (int i = 1; i < arr.size(); ++i)
    {
        const int key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key)
        {
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
}

int partition(QVector<int>& arr, int low, int high)
{
    const int pivot = arr[high];
    int i = low;
    for (int j = low; j < high; ++j)
    {
        if (arr[j] <= pivot)
        {
            std::swap(arr[i], arr[j]);
            ++i;
        }
    }
    std::swap(arr[i], arr[high]);
    return i;
}

void quickSort(QVector<int>& arr, int low, int high)
{
    if (low >= high) return;
    const int p = partition(arr, low, high);
    quickSort(arr, low, p - 1);
    quickSort(arr, p + 1, high);
}

void merge(QVector<int>& arr, int left, int mid, int right)
{
    QVector<int> temp;
    temp.reserve(right - left);
    int i = left;
    int j = mid;
    while (i < mid && j < right)
    {
        if (arr[i] <= arr[j])
        {
            temp.append(arr[i]);
            ++i;
        }
        else
        {
            temp.append(arr[j]);
            ++j;
        }
    }
    while (i < mid)
    {
        temp.append(arr[i]);
        ++i;
    }
    while (j < right)
    {
        temp.append(arr[j]);
        ++j;
    }
    for (int k = 0; k < temp.size(); ++k)
    {
        arr[left + k] = temp[k];
    }
}

void mergeSort(QVector<int>& arr, int left, int right)
{
    if (right - left < 2) return;
    const int mid = left + (right - left) / 2;
    mergeSort(arr, left, mid);
    mergeSort(arr, mid, right);
    merge(arr, left, mid, right);
}

QVector<qint64> fibonacci(int n)
{
    QVector<qint64> res(n, 0);
    if (n > 1) res[1] = 1;
    for (int i = 2; i < n; ++i)
    {
        res[i] = res[i - 1] + res[i - 2];
    }
    return res;
}

int coinChange(const QVector<int>& coins, int amount)
{
    const int inf = amount + 1;
    QVector<int> dp(amount + 1, inf);
    dp[0] = 0;
    for (int i = 1; i <= amount; ++i)
    {
        for (int coin : coins)
        {
            if (coin > 0 && coin <= i && dp[i - coin] != inf)
            {
                dp[i] = qMin(dp[i], dp[i - coin] + 1);
            }
        }
    }
    return dp[amount] == inf ? -1 : dp[amount];
}

QVector<int> bfs(const QVector<QVector<int>>& graph, int start)
{
    QVector<int> order;
    if (start < 0 || start >= graph.size()) return order;

    QVector<bool> visited(graph.size(), false);
    QVector<int> queue{start};
    visited[start] = true;

    int head = 0;
    while (head < queue.size())
    {
        const int u = queue[head++];
        order.append(u);
        for (int v : graph[u])
        {
            if (v >= 0 && v < graph.size() && !visited[v])
            {
                visited[v] = true;
                queue.append(v);
            }
        }
    }
    return order;
}

QVector<int> dfs(const QVector<QVector<int>>& graph, int start)
{
    QVector<int> order;
    if (start < 0 || start >= graph.size()) return order;

    QVector<bool> visited(graph.size(), false);
    QVector<int> stack{start};
    while (!stack.isEmpty())
    {
        const int u = stack.takeLast();
        if (u < 0 || u >= graph.size() || visited[u]) continue;

        visited[u] = true;
        order.append(u);
        for (int i = graph[u].size() - 1; i >= 0; --i)
        {
            stack.append(graph[u][i]);
        }
    }
    return order;
}

QVector<int> dijkstra(const QVector<QVector<int>>& graph, int start)
{
    const int n = graph.size();
    const int inf = std::numeric_limits<int>::max() / 4;
    QVector<int> dist(n, inf);
    QVector<bool> visited(n, false);

    if (start < 0 || start >= n) return dist;
    dist[start] = 0;

    for (int step = 0; step < n; ++step)
    {
        int u = -1;
        for (int v = 0; v < n; ++v)
        {
            if (!visited[v] && (u < 0 || dist[v] < dist[u])) u = v;
        }
        if (u < 0 || dist[u] == inf) break;

        visited[u] = true;
        const int edgeCount = qMin(n, graph[u].size());
        for (int v = 0; v < edgeCount; ++v)
        {
            const int cost = graph[u][v];
            if (cost >= 0 && dist[u] <= inf - cost)
            {
                dist[v] = qMin(dist[v], dist[u] + cost);
            }
        }
    }
    return dist;
}

QVector<int> topoSort(const QVector<QVector<int>>& graph)
{
    QVector<int> inDegree(graph.size(), 0);
    for (const QVector<int>& edges : graph)
    {
        for (int v : edges)
        {
            if (v >= 0 && v < graph.size()) ++inDegree[v];
        }
    }

    QVector<int> queue;
    for (int i = 0; i < inDegree.size(); ++i)
    {
        if (inDegree[i] == 0) queue.append(i);
    }

    QVector<int> order;
    int head = 0;
    while (head < queue.size())
    {
        const int u = queue[head++];
        order.append(u);
        for (int v : graph[u])
        {
            if (v >= 0 && v < graph.size() && --inDegree[v] == 0)
            {
                queue.append(v);
            }
        }
    }
    return order.size() == graph.size() ? order : QVector<int>();
}

QVector<int> kmp(const QString& text, const QString& pattern)
{
    QVector<int> matches;
    if (pattern.isEmpty()) return matches;

    QVector<int> lps(pattern.size(), 0);
    for (int i = 1, matched = 0; i < pattern.size();)
    {
        if (pattern[i] == pattern[matched])
        {
            lps[i++] = ++matched;
        }
        else if (matched > 0)
        {
            matched = lps[matched - 1];
        }
        else
        {
            lps[i++] = 0;
        }
    }

    for (int i = 0, matched = 0; i < text.size();)
    {
        if (text[i] == pattern[matched])
        {
            ++i;
            ++matched;
            if (matched == pattern.size())
            {
                matches.append(i - matched);
                matched = lps[matched - 1];
            }
        }
        else if (matched > 0)
        {
            matched = lps[matched - 1];
        }
        else
        {
            ++i;
        }
    }
    return matches;
}

int editDistance(const QString& word1, const QString& word2)
{
    QVector<int> prev(word2.size() + 1);
    QVector<int> curr(word2.size() + 1);
    for (int j = 0; j <= word2.size(); ++j)
    {
        prev[j] = j;
    }

    for (int i = 1; i <= word1.size(); ++i)
    {
        curr[0] = i;
        for (int j = 1; j <= word2.size(); ++j)
        {
            const int cost = (word1[i - 1] == word2[j - 1] ? 0 : 1);
            curr[j] = qMin(qMin(prev[j] + 1, curr[j - 1] + 1), prev[j - 1] + cost);
        }
        prev.swap(curr);
    }
    return prev[word2.size()];
}

int lcs(const QString& text1, const QString& text2)
{
    QVector<int> prev(text2.size() + 1, 0);
    QVector<int> curr(text2.size() + 1, 0);

    for (QChar c1 : text1)
    {
        for (int j = 1; j <= text2.size(); ++j)
        {
            if (c1 == text2[j - 1])
            {
                curr[j] = prev[j - 1] + 1;
            }
            else
            {
                curr[j] = qMax(prev[j], curr[j - 1]);
            }
        }
        prev.swap(curr);
        curr.fill(0);
    }
    return prev[text2.size()];
}

int maxSubArray(const QVector<int>& arr)
{
    if (arr.isEmpty()) return 0;
    int currSum = arr[0];
    int maxSum = currSum;
    for (int i = 1; i < arr.size(); ++i)
    {
        currSum = qMax(arr[i], currSum + arr[i]);
        maxSum = qMax(maxSum, currSum);
    }
    return maxSum;
}
}

namespace gongju {
int counter = 0;
qint64 totalSum = 0;
QVector<int> globalArr;

QVector<int> prefixSum(const QVector<int>& arr)
{
    QVector<int> sums(arr.size() + 1, 0);
    for (int i = 0; i < arr.size(); ++i)
    {
        sums[i + 1] = sums[i] + arr[i];
    }
    return sums;
}

void reverseRange(QVector<int>& arr, int left, int right)
{
    while (left < right) {
        const int temp = arr[left];
        arr[left] = arr[right];
        arr[right] = temp;
        ++left;
        --right;
    }
}

void rotate(QVector<int>& arr, int k)
{
    if (arr.isEmpty()) return;
    k %= arr.size();
    if (k < 0) k += arr.size();

    reverseRange(arr, 0, k - 1);
    reverseRange(arr, k, arr.size() - 1);
    reverseRange(arr, 0, arr.size() - 1);
}

QVector<int> countingSort(const QVector<int>& arr)
{
    if (arr.isEmpty()) return arr;
    int minVal = arr.first();
    int maxVal = arr.first();
    for (int val : arr)
    {
        minVal = qMin(minVal, val);
        maxVal = qMax(maxVal, val);
    }

    QVector<int> counts(maxVal - minVal + 1, 0);
    for (int val : arr)
    {
        ++counts[val - minVal];
    }

    QVector<int> sorted;
    sorted.reserve(arr.size());
    for (int i = 0; i < counts.size(); ++i)
    {
        while (counts[i]-- > 0)
        {
            sorted.append(i + minVal);
        }
    }
    return sorted;
}

void siftDown(QVector<int>& arr, int root, int end)
{
    while (root * 2 + 1 < end)
    {
        int child = root * 2 + 1;
        if (child + 1 < end && arr[child] < arr[child + 1])
        {
            ++child;
        }
        if (arr[root] >= arr[child]) return;

        std::swap(arr[root], arr[child]);
        root = child;
    }
}

void heapSort(QVector<int>& arr)
{
    for (int i = arr.size() / 2 - 1; i >= 0; --i)
    {
        siftDown(arr, i, arr.size());
    }
    for (int i = arr.size() - 1; i > 0; --i)
    {
        std::swap(arr[0], arr[i]);
        siftDown(arr, 0, i);
    }
}

quint64 binaryGcd(quint64 a, quint64 b)
{
    if (a == 0) return b;
    if (b == 0) return a;

    int shift = 0;
    while (((a | b) & 1U) == 0) {
        a >>= 1U;
        b >>= 1U;
        ++shift;
    }
    while ((a & 1U) == 0)
    {
        a >>= 1U;
    }

    do
    {
        while ((b & 1U) == 0)
        {
            b >>= 1U;
        }
        if (a > b)
        {
            std::swap(a, b);
        }
        b -= a;
    }
    while (b != 0);
    return a << shift;
}

int knapsack(const QVector<int>& weights, const QVector<int>& values, int capacity)
{
    if (capacity < 0) return 0;

    QVector<int> dp(capacity + 1, 0);
    const int n = qMin(weights.size(), values.size());
    for (int i = 0; i < n; ++i)
    {
        if (weights[i] <= 0 || weights[i] > capacity) continue;
        for (int w = capacity; w >= weights[i]; --w)
        {
            dp[w] = qMax(dp[w], dp[w - weights[i]] + values[i]);
        }
    }
    return dp[capacity];
}

QVector<QVector<int>> floodFill(QVector<QVector<int>> grid, int startRow, int startCol, int newColor)
{
    if (grid.isEmpty() || startRow < 0 || startRow >= grid.size()) return grid;

    const int numCols = grid[startRow].size();
    if (startCol < 0 || startCol >= numCols) return grid;

    const int oldColor = grid[startRow][startCol];
    if (oldColor == newColor) return grid;

    QVector<int> stack{startRow * numCols + startCol};
    grid[startRow][startCol] = newColor;

    while (!stack.isEmpty())
    {
        const int pos = stack.takeLast();
        const int r = pos / numCols;
        const int c = pos % numCols;

        const int dr[] = {-1, 1, 0, 0};
        const int dc[] = {0, 0, -1, 1};

        for (int dir = 0; dir < 4; ++dir)
        {
            const int nr = r + dr[dir];
            const int nc = c + dc[dir];

            if (nr < 0 || nr >= grid.size()) continue;
            if (nc < 0 || nc >= grid[nr].size()) continue;

            if (grid[nr][nc] == oldColor)
            {
                grid[nr][nc] = newColor;
                stack.append(nr * numCols + nc);
            }
        }
    }
    return grid;
}

QVector<int> nextGreater(const QVector<int>& arr)
{
    QVector<int> res(arr.size(), -1);
    QVector<int> stack;
    for (int i = 0; i < arr.size(); ++i)
    {
        while (!stack.isEmpty() && arr[stack.last()] < arr[i])
        {
            res[stack.takeLast()] = arr[i];
        }
        stack.append(i);
    }
    return res;
}

QVector<int> rabinKarp(const QString& text, const QString& pattern)
{
    QVector<int> matches;
    if (pattern.isEmpty() || pattern.size() > text.size())
    {
        return matches;
    }

    const qint64 base = 257;
    const qint64 mod = 1000000007;
    qint64 patternHash = 0;
    qint64 windowHash = 0;
    qint64 highPow = 1;

    for (int i = 0; i < pattern.size(); ++i)
    {
        patternHash = (patternHash * base + pattern[i].unicode()) % mod;
        windowHash = (windowHash * base + text[i].unicode()) % mod;
        if (i + 1 < pattern.size())
        {
            highPow = (highPow * base) % mod;
        }
    }

    for (int i = 0; i + pattern.size() <= text.size(); ++i)
    {
        if (patternHash == windowHash && text.mid(i, pattern.size()) == pattern)
        {
            matches.append(i);
        }
        if (i + pattern.size() < text.size())
        {
            const qint64 outgoing = text[i].unicode() * highPow;
            windowHash = (windowHash - outgoing) % mod;
            if (windowHash < 0)
            {
                windowHash += mod;
            }
            windowHash = (windowHash * base + text[i + pattern.size()].unicode()) % mod;
        }
    }
    return matches;
}

int josephus(int n, int k)
{
    if (n <= 0 || k <= 0) return -1;
    int survivor = 0;
    for (int size = 2; size <= n; ++size)
    {
        survivor = (survivor + k) % size;
    }
    return survivor + 1;
}
}