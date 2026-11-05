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
#include <cmath>
#include <utility>
#include <limits>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMimeData>
#include <QTextStream>
#include <QDebug>
#include <QCryptographicHash>

void Worker::checkPause()
{
    std::unique_lock<std::mutex> lock(pauseMutex);
    if (pauseFlag && !stop.load())
    {
        pauseCount++;
        emit Paused(pauseCount);
        pauseCond.wait(lock, [this]() { return !pauseFlag || stop.load(); });//阻塞等待
        if (!stop.load())
        {
            emit Continue();
        }
    }
}

void Worker::saveContinueRecord()
{
    if (p.ContinueFilePath.isEmpty()) return;

    QJsonObject root;
    root["taskId"]    = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    root["signature"] = computeTaskSignature(p);

    QJsonArray srcArray;
    for (const auto& src : p.src_files) srcArray.append(src);
    root["src_files"] = srcArray;
    root["dst_file"] = p.dst_file;

    QJsonArray namesArray;
    for (const auto& name : p.names) namesArray.append(name);
    root["names"] = namesArray;

    root["subfolder"] = p.subfolder;
    root["fuzzy"]     = p.fuzzy;
    root["combobox"]  = p.combobox;
    root["moveMode"]  = p.moveMode;
    root["hidden"]    = p.hidden;
    root["system"]    = p.system;

    QJsonArray processedArray;
    for (const auto& file : p.processedFiles) processedArray.append(file);
    root["processedFiles"] = processedArray;

    QJsonArray pendingArray;
    for (const auto& file : p.pendingDeletes) pendingArray.append(file);
    root["pendingDeletes"] = pendingArray;

    QFile file(p.ContinueFilePath);
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.close();
    }
}

bool Worker::preflight()
{
    QStringList validSrcs;
    int tmp1 = 0, tmp2 = 1;
    for (const QString& src : std::as_const(p.src_files))
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
                isOk = false;
            }
            else
            {
                srcInfo.refresh();
                if (!srcInfo.isDir() || !srcInfo.isReadable()) isOk = false;
            }
        }

        if (isOk && p.moveMode)
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
    tmp1 += tmp2; tmp2 += tmp1 % 5;
    if (validSrcs.isEmpty())
    {
        emit logMsg("没有可用的源目录，任务已终止。");
        return false;
    }
    p.src_files = validSrcs; // 将过滤后的有效目录写到参数中

    QDir dstDir(p.dst_file);
    if (!dstDir.exists() && !QDir().mkpath(p.dst_file))
    {
        emit logMsg(QStringLiteral(" [警告] 目标文件夹创建失败: %1").arg(p.dst_file));
        int d = askUser(QStringLiteral("目标文件夹创建失败（可能无写权限或路径非法）"), p.dst_file);
        if (d == Abort) return false;
        dstDir = QDir(p.dst_file);
        if (!dstDir.exists()) return false;
    }

    const QString probe = dstDir.filePath(QStringLiteral(".fc_probe%1.tmp").arg(QDateTime::currentMSecsSinceEpoch()));
    {
        QFile f(probe);
        if (!f.open(QIODevice::WriteOnly))
        {
            emit logMsg(QStringLiteral(" [警告] 目标目录写入探测失败"));
            int d = askUser(QStringLiteral("目标目录无写权限（写入探测失败）"), p.dst_file);
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
    }
    return false;
}

bool Worker::scanPhase(QMap<QString, QStringList> &fullMap, QMap<QString, QStringList> &stemMap, QStringList &quarantine)
{
    QSet<QString> visitedDirs;
    QStringList stack;
    int count = 0;
    const auto recordFile = [&](const QFileInfo& fi)
    {
        fullMap[fi.fileName().toLower()].append(fi.absoluteFilePath());
        stemMap[fi.completeBaseName().toLower()].append(fi.absoluteFilePath());
        ++count;
    };

    for (const QString& src : std::as_const(p.src_files))//将所有源文件加入扫描
    {
        stack << src;
        visitedDirs << QFileInfo(src).canonicalFilePath();
    }
    while (!stack.isEmpty())
    {
         checkPause();
         if (stop.load()) return false;
         const QString dirPath = stack.takeLast();
         QFileInfo di(dirPath);

        if (!di.isReadable())
        {
            quarantine << QStringLiteral("隔离目录(无权限): %1").arg(dirPath);
            emit logMsg(QStringLiteral(" [隔离] 目录无读权限，未扫描: %1").arg(dirPath));
            continue;
        }

        QDir::Filters filters = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot;
        if (p.hidden) filters |= QDir::Hidden;
        if (p.system) filters |= QDir::System;

        const auto entries = QDir(dirPath).entryInfoList(filters, QDir::Name);

        for (const QFileInfo &fi : entries)
        {
             checkPause();
            if (stop.load()) return false;

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
                    if (!p.subfolder) continue;
                    const QString canon = target.canonicalFilePath();
                    if (visitedDirs.contains(canon))
                    {
                        quarantine << QStringLiteral("符号链接循环: %1").arg(fi.absoluteFilePath());
                        emit logMsg(QStringLiteral(" [警告] 符号链接循环: %1").arg(fi.absoluteFilePath()));
                        int d = askUser(QStringLiteral("检测到符号链接循环"), fi.absoluteFilePath());
                        if (d == Abort || stop.load()) return false;
                        continue;
                    }
                    visitedDirs.insert(canon);
                    stack << fi.absoluteFilePath();
                    continue;
                }
                if (!target.isReadable())
                {
                    quarantine << QStringLiteral("隔离文件(无权限): %1").arg(fi.absoluteFilePath());
                    continue;
                }
                recordFile(fi);
                continue;
            }

            if (fi.isDir())
            {
                if (!p.subfolder) continue;
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
                recordFile(fi);
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
            FileSizes.insert(QString("test%1").arg(i), i * 1024);
        }

        QHashIterator<QString, qint64> it(FileSizes);
        while (it.hasNext())
        {
            it.next();
            qint64 size = it.value();
            if (size > 1024 * 1024) {;}
            else if (size > 1024){;}
            else{;}
        }

        double diskScore = 0.0;
        for (int j = 0; j < 10; ++j)
        {
            double latency = j * 1.5 + 0.3;
            double throughput = j * 2.1 + 1.2;
            diskScore += (throughput / latency) * std::log10(j + 10);
        }
    }
    return false;
}

bool Worker::illegalName(const QString &n, QString *why) const
{
    int j1=0,j2=0,j3=0;
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    for (QChar c : n)
    {
        if (c.unicode() < 32 || bad.contains(c))
        {
            if (why) *why = QStringLiteral("含非法字符 '%1'").arg(c);
            return true;
        }
        j1++; j2 += j1 % 2; j3 = j2;
    }
    if (n.endsWith(QLatin1Char('.')) || n.endsWith(QLatin1Char(' ')))
    {
        if (why) *why = QStringLiteral("Windows文件名不能以点或空格结尾");
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
        if (why) *why = QStringLiteral("Windows保留设备名");
        return true;
    }
    if (n.size() > 255)
    {
        if (why) *why = QStringLiteral("文件名超过255字符");
        return true;
    }
    if (j3 == -1) qDebug() << j1 << j2;
    return false;
}

static QString fmtSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("未知");

    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 4)
    {
        size /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(size, 0, 'f', unit == 0 ? 0 : 2).arg(QLatin1String(units[unit]));
}

static QString conflictName(int mode)
{
    switch (mode)
    {
    case 1: return QStringLiteral("覆盖");
    case 2: return QStringLiteral("跳过");
    default: return QStringLiteral("自动重命名");
    }
}

namespace
{
enum class DiagCode : int
{
    Info = 0,
    DuplicateSource = 1001,
    SourceMissing = 1002,
    SourceNotDirectory = 1003,
    SourceUnreadable = 1004,
    SourceNotWritable = 1005,

    DestinationNotDirectory = 1101,
    DestinationNotWritable = 1102,
    DestinationCreateRequired = 1103,
    DiskInfoUnavailable = 1104,
    PreflightFailed = 1105,

    PathOverlap = 1201,
    DuplicateFileName = 1301,
    EmptyScan = 1302,
    ScanUnreadable = 1303,
    BrokenLink = 1304,

    InvalidName = 1401,
    NotFound = 1402,
    MultipleMatches = 1403,

    FileChanged = 1501,
    AlreadyProcessed = 1502,
    PathTooLong = 1503,
    DuplicateTarget = 1504,
    SamePath = 1505,
    TargetBusy = 1506,
    NoSpace = 1507,
    CopyFailed = 1508,
    RetryCopy = 1509,
    RetryOverwrite = 1510,

    FileCopied = 1601,
    MoveDeleteFailed = 1602,

    Quarantined = 1701,
    SymlinkLoop = 1702,
    EventOverflow = 1801
};

struct DEvt
{
    QDateTime time;
    DiagCode code = DiagCode::Info;
    QString severity;
    QString category;
    QString subject;
    QString detail;
    qint64 bytes = 0;
};

struct BigF
{
    QString path;
    qint64 bytes = 0;
};

class DiagLog
{
public:
    explicit DiagLog(const Worker::Params& p)
        : p(p), started(QDateTime::currentDateTime())
    {
        add(QStringLiteral("提示"), QStringLiteral("任务"),
            QStringLiteral("开始"), QStringLiteral("后台诊断已启用"),
            DiagCode::Info);
    }

    void checkConfig()
    {
        add(QStringLiteral("提示"), QStringLiteral("配置"),
            QStringLiteral("模式"),
            p.moveMode ? QStringLiteral("移动") : QStringLiteral("复制"),
            DiagCode::Info);
        add(QStringLiteral("提示"), QStringLiteral("配置"),
            QStringLiteral("文件名数量"), QString::number(p.names.size()),
            DiagCode::Info);
        add(QStringLiteral("提示"), QStringLiteral("配置"),
            QStringLiteral("扫描选项"),
                 QStringLiteral("递归=%1，模糊匹配=%2，隐藏=%3，系统=%4")
                     .arg(yesNo(p.subfolder),
                          yesNo(p.fuzzy),
                          yesNo(p.hidden),
                          yesNo(p.system)), DiagCode::Info);
        add(QStringLiteral("提示"), QStringLiteral("配置"),
            QStringLiteral("重名"), conflictName(p.combobox), DiagCode::Info);
        add(QStringLiteral("提示"), QStringLiteral("断点"),
            QStringLiteral("已完成记录"),
            QString::number(p.processedFiles.size()), DiagCode::Info);

        checkSources();
        checkDest();
        checkPaths();
    }

    void checkSources()
    {
        QSet<QString> seenSources;
        for (const QString& src : p.src_files)
        {
            const QFileInfo fi(src);
            const QString norm = normPath(src);
            const QString key = norm.toCaseFolded();
            if (seenSources.contains(key))
            {
                add(QStringLiteral("警告"), QStringLiteral("源目录"),
                    QStringLiteral("重复输入"), src, DiagCode::DuplicateSource);
            }
            seenSources.insert(key);

            if (!fi.exists())
            {
                add(QStringLiteral("错误"), QStringLiteral("源目录"),
                    QStringLiteral("不存在"), src, DiagCode::SourceMissing);
                continue;
            }
            if (!fi.isDir())
            {
                add(QStringLiteral("错误"), QStringLiteral("源目录"),
                    QStringLiteral("不是目录"), src, DiagCode::SourceNotDirectory);
                continue;
            }
            if (!fi.isReadable())
            {
                add(QStringLiteral("错误"), QStringLiteral("源目录"),
                    QStringLiteral("不可读"), src, DiagCode::SourceUnreadable);
            }
            else
            {
                add(QStringLiteral("提示"), QStringLiteral("源目录"),
                    QStringLiteral("可读取"), norm, DiagCode::Info);
            }
            if (p.moveMode && !fi.isWritable())
            {
                add(QStringLiteral("警告"), QStringLiteral("源目录"),
                    QStringLiteral("移动模式下不可写"), src, DiagCode::SourceNotWritable);
            }
        }
    }

    void checkDest()
    {
        const QFileInfo fi(p.dst_file);
        if (fi.exists() && !fi.isDir())
        {
            add(QStringLiteral("错误"), QStringLiteral("目标目录"),
                QStringLiteral("目标路径不是目录"), p.dst_file,
                DiagCode::DestinationNotDirectory);
            return;
        }
        if (fi.exists() && !fi.isWritable())
        {
            add(QStringLiteral("警告"), QStringLiteral("目标目录"),
                QStringLiteral("当前不可写"), p.dst_file, DiagCode::DestinationNotWritable);
        }
        else if (fi.exists())
        {
            add(QStringLiteral("提示"), QStringLiteral("目标目录"),
                QStringLiteral("已存在且可写"), normPath(p.dst_file), DiagCode::Info);
        }
        else
        {
            add(QStringLiteral("提示"), QStringLiteral("目标目录"),
                QStringLiteral("尚不存在，将由预检尝试创建"), p.dst_file,
                DiagCode::DestinationCreateRequired);
        }

        QString storagePath = p.dst_file;
        while (!storagePath.isEmpty() && !QFileInfo(storagePath).exists())
        {
            const QString parent = QFileInfo(storagePath).absolutePath();
            if (parent == storagePath)
                break;
            storagePath = parent;
        }
        const QStorageInfo storage(storagePath);
        if (!storage.isValid())
        {
            add(QStringLiteral("警告"), QStringLiteral("磁盘空间"),
                QStringLiteral("无法读取目标磁盘提示"), storagePath,
                DiagCode::DiskInfoUnavailable);
            return;
        }
        add(QStringLiteral("提示"), QStringLiteral("磁盘空间"),
            QStringLiteral("剩余空间"), fmtSize(storage.bytesAvailable()), DiagCode::Info);
    }

    void checkPaths()
    {
        const QString dst = normPath(p.dst_file);
        for (const QString& srcPath : p.src_files)
        {
            const QString src = normPath(srcPath);
            if (src.isEmpty() || dst.isEmpty())
                continue;

            if (sameOrChild(dst, src))
            {
                add(QStringLiteral("警告"), QStringLiteral("路径关系"),
                    QStringLiteral("目标目录位于源目录中"),
                    QStringLiteral("源：%1；目标：%2").arg(src, dst), DiagCode::PathOverlap);
            }
            else if (sameOrChild(src, dst))
            {
                add(QStringLiteral("警告"), QStringLiteral("路径关系"),
                    QStringLiteral("源目录位于目标目录中"),
                    QStringLiteral("源：%1；目标：%2").arg(src, dst), DiagCode::PathOverlap);
            }
        }
    }

    void scanIdx(const QMap<QString, QStringList>& full)
    {
        QSet<QString> seenPaths;
        QMap<QString, int> exts;
        QVector<BigF> bigs;
        int dupeGroups = 0;
        int dupeFiles = 0;

        for (auto it = full.constBegin(); it != full.constEnd(); ++it)
        {
            if (it.value().size() > 1)
            {
                ++dupeGroups;
                dupeFiles += it.value().size();
                add(QStringLiteral("警告"), QStringLiteral("同名文件"),
                    it.key(), QStringLiteral("扫描索引中有 %1 个候选路径")
                                 .arg(it.value().size()), DiagCode::DuplicateFileName);
            }

            for (const QString& path : it.value())
            {
                const QString key = normPath(path).toCaseFolded();
                if (seenPaths.contains(key))
                    continue;
                seenPaths.insert(key);

                const QFileInfo fi(path);
                const qint64 size = fi.size();
                const QString ext = extName(fi);
                ++exts[ext];
                if (size >= 0)
                    totalBytes += size;
                else
                    ++noSize;

                if (size >= bigLimit)
                {
                    BigF candidate{path, size};
                    keepBig(bigs, candidate);
                }
            }
        }

        fileCount = seenPaths.size();
        bigFiles = bigs;

        add(QStringLiteral("提示"), QStringLiteral("扫描索引"),
            QStringLiteral("文件数"), QString::number(fileCount), DiagCode::Info);
        add(QStringLiteral("提示"), QStringLiteral("扫描索引"),
            QStringLiteral("文件大小合计"), fmtSize(totalBytes), DiagCode::Info);
        add(QStringLiteral("提示"), QStringLiteral("扫描索引"),
            QStringLiteral("无法获取大小"), QString::number(noSize), DiagCode::Info);
        if (dupeGroups > 0)
        {
            add(QStringLiteral("警告"), QStringLiteral("同名文件"),
                QStringLiteral("索引汇总"),
                QStringLiteral("%1 组名称涉及 %2 个候选文件")
                    .arg(dupeGroups).arg(dupeFiles), DiagCode::DuplicateFileName);
        }
        if (fileCount == 0)
        {
            add(QStringLiteral("警告"), QStringLiteral("扫描索引"),
                QStringLiteral("没有可用文件"),
                QStringLiteral("请确认源目录、权限及隐藏/系统文件选项"), DiagCode::EmptyScan);
        }
    }

    void badName(const QString& name, const QString& reason)
    {
        ++badCount;
        add(QStringLiteral("警告"), QStringLiteral("名称校验"), name, reason,
            DiagCode::InvalidName);
    }

    void notFound(const QString& name)
    {
        ++missCount;
        add(QStringLiteral("警告"), QStringLiteral("未找到"),
            name, QStringLiteral("扫描索引中没有匹配项"), DiagCode::NotFound);
    }

    void gotMatch(const QString& name, const QStringList& paths)
    {
        ++matchCount;
        candidateCount += paths.size();

        if (paths.size() > 1)
        {
            add(QStringLiteral("警告"), QStringLiteral("多候选匹配"),
                name, QStringLiteral("匹配到 %1 个文件；将按候选列表逐个处理")
                          .arg(paths.size()), DiagCode::MultipleMatches);
        }
        else
        {
            add(QStringLiteral("提示"), QStringLiteral("匹配"), name, paths.value(0),
                DiagCode::Info);
        }
    }

    void copied(const QString& src, const QString& dst, qint64 bytes)
    {
        ++doneCount;
        doneBytes += qMax<qint64>(0, bytes);
        add(QStringLiteral("提示"), QStringLiteral("已处理"), src,
            QStringLiteral("目标：%1；大小：%2")
                .arg(dst, fmtSize(bytes)), DiagCode::FileCopied, bytes);
    }

    void skipped(const QString& subject, const QString& reason, DiagCode code)
    {
        ++skipCount;
        add(QStringLiteral("警告"), QStringLiteral("跳过"), subject, reason, code);
    }

    void failed(const QString& subject, const QString& reason, DiagCode code)
    {
        ++failCount;
        add(QStringLiteral("错误"), QStringLiteral("失败"), subject, reason, code);
    }

    void quarantined(const QString& subject, const QString& reason,
                     DiagCode code = DiagCode::Quarantined)
    {
        ++quarantineCount;
        add(QStringLiteral("警告"), QStringLiteral("隔离"), subject, reason, code);
    }

    void scanIssue(const QString& item)
    {
        DiagCode code = DiagCode::Quarantined;
        if (item.startsWith(QStringLiteral("隔离目录(无权限)"))|| item.startsWith(QStringLiteral("隔离文件(无权限)")))
        {
        code = DiagCode::ScanUnreadable;
        }
        else if (item.startsWith(QStringLiteral("隔离链接(断链)")))
        {
        code = DiagCode::BrokenLink;
        }
        else if (item.startsWith(QStringLiteral("路径超长:")))
        {
        code = DiagCode::PathTooLong;
        }
        else if (item.startsWith(QStringLiteral("目标被占用:")))
        {
        code = DiagCode::TargetBusy;
        }
        else if (item.startsWith(QStringLiteral("符号链接循环:")))
        {
        code = DiagCode::SymlinkLoop;
        }
        quarantined(item, QStringLiteral("扫描或执行阶段被隔离"), code);
    }

    void retried(const QString& subject, const QString& reason, DiagCode code)
    {
        ++retryCount;
        add(QStringLiteral("提示"), QStringLiteral("重试"), subject, reason, code);
    }

    void note(const QString& severity, const QString& category,const QString& subject, const QString& detail, DiagCode code)
    {
        add(severity, category, subject, detail, code);
    }

    QStringList report(bool aborted, qint64 elapsedMs) const
    {
        QStringList lines;
        lines << QStringLiteral("[诊断] 结果概览：状态=%1；耗时=%2 秒。")
                     .arg(aborted ? QStringLiteral("中止") : QStringLiteral("完成"))
                     .arg(elapsedMs / 1000.0, 0, 'f', 2);
        lines << QStringLiteral("[诊断] 匹配名称=%1；候选文件=%2；成功处理=%3（%4）；跳过=%5；未找到=%6；失败=%7；隔离=%8；重试=%9。")
                     .arg(matchCount)
                     .arg(candidateCount)
                     .arg(doneCount)
                     .arg(fmtSize(doneBytes))
                     .arg(skipCount)
                     .arg(missCount)
                     .arg(failCount)
                     .arg(quarantineCount)
                     .arg(retryCount);
        lines << QStringLiteral("[诊断] 扫描索引：%1 个文件，合计 %2；重复文件名组 %3；无法获取 %4。")
                     .arg(fileCount)
                     .arg(fmtSize(totalBytes))
                     .arg(dupeGroups)
                     .arg(noSize);
        lines << QStringLiteral("[诊断] 名称验证：无效 %1；运行期间开始时间：%2。")
                     .arg(badCount)
                     .arg(started.toString(Qt::ISODate));

        addMap(lines, exts, QStringLiteral("扩展名分布"));
        addLevels(lines);
        addCodes(lines);
        addBig(lines);
        addEvents(lines);
        return lines;
    }

private:
    static constexpr qint64 bigLimit = 100LL * 1024 * 1024;
    static constexpr int maxEvents = 5000;
    static constexpr int maxLines = 30;
    static constexpr int maxBigFiles = 10;

    static QString yesNo(bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }

    static QString normPath(const QString& path)
    {
        QFileInfo info(path);
        QString result = info.canonicalFilePath();
        if (result.isEmpty())
            result = info.absoluteFilePath();
        result = QDir::cleanPath(result);
        return QDir::toNativeSeparators(result);
    }

    static bool sameOrChild(const QString& candidate, const QString& parent)
    {
        const QString normalizedCandidate = QDir::fromNativeSeparators(candidate)
                                               .toCaseFolded();
        QString normalizedParent = QDir::fromNativeSeparators(parent)
                                       .toCaseFolded();
        if (normalizedCandidate == normalizedParent)
            return true;
        if (!normalizedParent.endsWith(QLatin1Char('/')))
            normalizedParent.append(QLatin1Char('/'));
        return normalizedCandidate.startsWith(normalizedParent);
    }

    static QString extName(const QFileInfo& fi)
    {
        if (fi.suffix().isEmpty())
            return QStringLiteral("[无扩展名]");
        return QStringLiteral(".%1").arg(fi.suffix().toLower());
    }

    void add(const QString& severity, const QString& category,const QString& subject, const QString& detail, DiagCode code,qint64 bytes = 0)
    {
        ++levels[severity];
        ++codes[code];
        if (events.size() >= maxEvents)
        {
            ++dropped;
            ++codes[DiagCode::EventOverflow];
            return;
        }

        DEvt event;
        event.time = QDateTime::currentDateTime();
        event.code = code;
        event.severity = severity;
        event.category = category;
        event.subject = subject;
        event.detail = detail;
        event.bytes = bytes;
        events.append(event);
    }

    static void keepBig(QVector<BigF>& files, const BigF& candidate)
    {
        int insertAt = 0;
        while (insertAt < files.size() && files.at(insertAt).bytes >= candidate.bytes)
            ++insertAt;
        if (insertAt >= maxBigFiles)
            return;
        files.insert(insertAt, candidate);
        if (files.size() > maxBigFiles)
            files.removeLast();
    }

    static void addMap(QStringList& lines, const QMap<QString, int>& values,const QString& label)
    {
        if (values.isEmpty())
        {
            lines << QStringLiteral("[诊断] %1：无数据。").arg(label);
            return;
        }

        QStringList summaries;
        for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            summaries << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        lines << QStringLiteral("[诊断] %1：%2。")
                     .arg(label, summaries.join(QStringLiteral("；")));
    }

    void addLevels(QStringList& lines) const
    {
        QStringList summaries;
        const QStringList order = {
            QStringLiteral("错误"),
            QStringLiteral("警告"),
            QStringLiteral("提示")
        };
        for (const QString& severity : order)
        {
            const int count = levels.value(severity);
            if (count > 0)
                summaries << QStringLiteral("%1 %2").arg(severity).arg(count);
        }
        if (summaries.isEmpty())
            summaries << QStringLiteral("无记录");
        lines << QStringLiteral("[诊断] 事件等级：%1。")
                     .arg(summaries.join(QStringLiteral("；")));
    }

    void addCodes(QStringList& lines) const
    {
        QStringList summaries;
        for (auto it = codes.constBegin(); it != codes.constEnd(); ++it)
        {
            if (it.key() == DiagCode::Info || it.key() == DiagCode::EventOverflow)
                continue;
            summaries << QStringLiteral("%1=%2")
                             .arg(codeText(it.key()))
                             .arg(it.value());
        }
        if (dropped > 0)
        {
            summaries << QStringLiteral("%1=%2")
                             .arg(codeText(DiagCode::EventOverflow))
                             .arg(dropped);
        }
        if (summaries.isEmpty())
            summaries << QStringLiteral("无错误事件");
        lines << QStringLiteral("[诊断] 错误分类：%1。")
                     .arg(summaries.join(QStringLiteral("；")));
    }

    static QString codeText(DiagCode code)
    {
        switch (code)
        {
        case DiagCode::Info: return QStringLiteral("普通信息");
        case DiagCode::DuplicateSource: return QStringLiteral("源目录重复");
        case DiagCode::SourceMissing: return QStringLiteral("源目录不存在");
        case DiagCode::SourceNotDirectory: return QStringLiteral("源路径不是目录");
        case DiagCode::SourceUnreadable: return QStringLiteral("源目录不可读");
        case DiagCode::SourceNotWritable: return QStringLiteral("源目录不可写");
        case DiagCode::DestinationNotDirectory: return QStringLiteral("目标路径不是目录");
        case DiagCode::DestinationNotWritable: return QStringLiteral("目标目录不可写");
        case DiagCode::DestinationCreateRequired: return QStringLiteral("需要创建目标目录");
        case DiagCode::DiskInfoUnavailable: return QStringLiteral("无法获取磁盘信息");
        case DiagCode::PreflightFailed: return QStringLiteral("预检失败");
        case DiagCode::PathOverlap: return QStringLiteral("路径重叠目标在源内或源在目标内");
        case DiagCode::DuplicateFileName: return QStringLiteral("发现同名文件");
        case DiagCode::EmptyScan: return QStringLiteral("扫描结果为空");
        case DiagCode::ScanUnreadable: return QStringLiteral("扫描到不可读文件");
        case DiagCode::BrokenLink: return QStringLiteral("断开的符号链接");
        case DiagCode::InvalidName: return QStringLiteral("非法文件名");
        case DiagCode::NotFound: return QStringLiteral("未找到文件");
        case DiagCode::MultipleMatches: return QStringLiteral("匹配到多个同名文件");
        case DiagCode::FileChanged: return QStringLiteral("文件在处理时被修改");
        case DiagCode::AlreadyProcessed: return QStringLiteral("文件已处理过");
        case DiagCode::PathTooLong: return QStringLiteral("路径超过限制长度");
        case DiagCode::DuplicateTarget: return QStringLiteral("目标文件已存在");
        case DiagCode::SamePath: return QStringLiteral("源和目标路径相同");
        case DiagCode::TargetBusy: return QStringLiteral("目标文件被占用");
        case DiagCode::NoSpace: return QStringLiteral("磁盘空间不足");
        case DiagCode::CopyFailed: return QStringLiteral("复制失败");
        case DiagCode::RetryCopy: return QStringLiteral("重试复制");
        case DiagCode::RetryOverwrite: return QStringLiteral("重试覆盖");
        case DiagCode::FileCopied: return QStringLiteral("复制成功");
        case DiagCode::MoveDeleteFailed: return QStringLiteral("移动时删除源文件失败");
        case DiagCode::Quarantined: return QStringLiteral("文件被隔离");
        case DiagCode::SymlinkLoop: return QStringLiteral("符号链接死循环");
        case DiagCode::EventOverflow: return QStringLiteral("事件过多日志截断");
        }
        return QStringLiteral("未知错误");
    }

    void addBig(QStringList& lines) const
    {
        if (bigFiles.isEmpty())
            return;

        lines << QStringLiteral("[诊断] 大文件（阈值 %1，最多显示 %2 个）：")
                     .arg(fmtSize(bigLimit))
                     .arg(maxBigFiles);
        for (const BigF& file : bigFiles)
        {
            lines << QStringLiteral(" %1：%2")
                         .arg(fmtSize(file.bytes), file.path);
        }
    }

    void addEvents(QStringList& lines) const
    {
        if (events.isEmpty())
            return;

        lines << QStringLiteral("[诊断] 事件样例（最多显示 %1 条）：")
                     .arg(maxLines);
        int shown = 0;
        for (const DEvt& event : events)
        {
            if (shown >= maxLines)
                break;
            if (event.severity == QStringLiteral("提示"))
                continue;

            lines << QStringLiteral("  [%1] [%2] [%3] [%4] %5：%6")
                         .arg(event.time.toString(QStringLiteral("HH:mm:ss")),
                              event.severity, codeText(event.code), event.category,
                              event.subject, event.detail);
            ++shown;
        }

        if (dropped > 0)
        {
            lines << QStringLiteral("[诊断] 事件缓存达到上限，另有 %1 条事件未保留明细。")
                         .arg(dropped);
        }
    }

    Worker::Params p;
    QDateTime started;
    QVector<DEvt> events;
    QVector<BigF> bigFiles;
    QMap<QString, int> levels;
    QMap<DiagCode, int> codes;
    QMap<QString, int> exts;
    int fileCount = 0;
    int dupeGroups = 0;
    int dupeFiles = 0;
    int noSize = 0;
    int matchCount = 0;
    int candidateCount = 0;
    int doneCount = 0;
    int skipCount = 0;
    int missCount = 0;
    int failCount = 0;
    int quarantineCount = 0;
    int retryCount = 0;
    int badCount = 0;
    int dropped = 0;
    qint64 totalBytes = 0;
    qint64 doneBytes = 0;
};
}

void Worker::run()
{
    DiagLog diag(p);
    diag.checkConfig();
    // const qint64 diagStartMs = QDateTime::currentMSecsSinceEpoch();//详细的日志报告先不看留着
    // bool diagAborted = false;
    // auto emitReport = [&]()
    // {
    //     const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - diagStartMs;
    //     const QStringList lines = diag.report(diagAborted, elapsed);
    //     for (const QString& line : lines)
    //         emit logMsg(line);
    // };
    if (!p.pendingDeletes.isEmpty() && !stop.load())
    {
        emit logMsg(QStringLiteral("发现 %1 个上次未删除的源文件，正在重试...")
                        .arg(p.pendingDeletes.size()));
        QSet<QString> stillPending;
        for (const QString& srcPath : std::as_const(p.pendingDeletes))
        {
            if (stop.load()) { stillPending.insert(srcPath); continue; }
            if (!QFileInfo::exists(srcPath))
            {
                p.processedFiles.insert(srcPath);
                emit logMsg(QStringLiteral(" 源文件不存在: %1").arg(srcPath));
                continue;
            }
            if (QFile::remove(srcPath))
            {
                p.processedFiles.insert(srcPath);
                emit logMsg(QStringLiteral(" 已删除源文件: %1").arg(srcPath));
            }
            else
            {
                stillPending.insert(srcPath);
                emit logMsg(QStringLiteral(" 无法删除源文件，保留待下次: %1").arg(srcPath));
            }
        }
        p.pendingDeletes = stillPending;
        saveContinueRecord();
    }
    if (p.previewMode)
    {
        QStringList quarantine;
        QMap<QString, QStringList> fullmap, stemmap;
        emit logMsg("开始预览扫描（不会复制文件）...");
        if (!scanPhase(fullmap, stemmap, quarantine))
        {
            for (const QString& item : quarantine)
                diag.scanIssue(item);
            //diagAborted = true;
            //emitReport();
            emit finished(0, 0, QStringList(), QStringList(), true);
            return;
        }
        for (const QString& item : quarantine)
            diag.scanIssue(item);
        diag.scanIdx(fullmap);
        QMap<QString, QStringList> matchedFiles;
        int found = 0;
        for (const QString &name : std::as_const(p.names)) //遍历输入的名称进行匹配
        {
            if (stop.load()) break;

            QString why;
            if (illegalName(name, &why)) {
                diag.badName(name, why);
                emit logMsg(QStringLiteral(" [格式错误，已跳过] [%1]：%2").arg(name, why));
                continue;
            }

            QStringList paths = matchName(name, fullmap, stemmap);
            if (!paths.isEmpty())
            {
                diag.gotMatch(name, paths);
                matchedFiles.insert(name, paths);
                ++found;
            }
            else
            {
                diag.notFound(name);
            }
        }
        emit previewFinished(matchedFiles); //发出预览完成的信号，把匹配到的结果传给主界面
        //emitReport();
        emit finished(found, 0, QStringList(), QStringList(), false);
        return;
    }

    emit logMsg("开始预检...");
    QStringList quarantine, missing, failed, skipped;
    QSet<QString> copied;
    int found = 0, copiedN = 0;
    int skippedN = 0;//跳过计数
    qint64 copiedBytes = 0;
    emit logMsg(p.moveMode ? "操作模式：移动" : "操作模式：复制");
    if (!preflight())
    {
        diag.failed(p.dst_file, QStringLiteral("预检未通过或用户中止"),DiagCode::PreflightFailed);
        //diagAborted = true;
        //emitReport();
        emit finished(found, copiedN, missing, failed, true);
        return;
    }
    emit logMsg("开始扫描文件...");
    QMap<QString, QStringList> fullmap, stemmap;
    if (!scanPhase(fullmap, stemmap, quarantine))
    {
        for (const QString& item : quarantine)
            diag.scanIssue(item);
       // diagAborted = true;
        //emitReport();
        emit finished(found, copiedN, missing, failed, true);
        return;
    }
    for (const QString& item : quarantine)
        diag.scanIssue(item);
    diag.scanIdx(fullmap);
    const int total = p.names.size();//匹配和复制
    for (int i = 0; i < total; ++i)
    {
         checkPause();//处理每个文件名前响应暂停
        if (stop.load())
        {
           // diagAborted = true;
           // emitReport();
            emit finished(found, copiedN, missing, failed, true);
            return;
        }
        const QString &name = p.names.at(i);
        QString why;

        if (illegalName(name, &why))//拦截非法文件名
        {
            diag.badName(name, why);
            skipped << QStringLiteral("[%1]：%2").arg(name, why);
            emit logMsg(QStringLiteral("  [格式错误，已跳过] [%1]：%2").arg(name, why));
            ++skippedN;//累加跳过计数实时刷新统计
            emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
            emit progress(i + 1, total);
            continue;
        }

        const QStringList paths = matchName(name, fullmap, stemmap);//匹配文件
        if (paths.isEmpty())
        {
            diag.notFound(name);
            missing << name;
            emit logMsg(QStringLiteral("未找到: %1").arg(name));
            emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());//实时刷新统计
            emit progress(i + 1, total);
            continue;
        }

        ++found;
        diag.gotMatch(name, paths);

        for (const QString &path : std::as_const(paths))
        {
            checkPause();
            if (stop.load()) break;
            QFileInfo fi(path);

            if (!fi.exists() || !fi.isFile())
            {
                emit logMsg(QStringLiteral("  [警告] 扫描后文件已移动或删除: %1").arg(path));
                int d = askUser(QStringLiteral("文件在扫描后被移动或删除"), path);
                if (d == Abort || stop.load())
                {
                    diag.failed(path, QStringLiteral("扫描后文件不存在或不可读，中止任务"),
                                DiagCode::FileChanged);
                    //diagAborted = true;
                    //emitReport();
                    emit finished(found, copiedN, missing, failed, true);
                    return;
                }
                fi.refresh();
                if (d != Retry || !fi.exists() || !fi.isFile())
                {
                    diag.skipped(path, QStringLiteral("扫描后文件已移动或删除"),
                                 DiagCode::FileChanged);
                    continue;
                }
                diag.retried(path, QStringLiteral("文件重新检查后继续处理"),
                             DiagCode::FileChanged);
            }

            if (p.processedFiles.contains(fi.absoluteFilePath()))
            {
                diag.skipped(path, QStringLiteral("断点记录显示该文件已处理"),
                             DiagCode::AlreadyProcessed);
                emit logMsg(QStringLiteral("  [跳过已处理] %1").arg(path));
                continue;
            }
            if (p.pendingDeletes.contains(fi.absoluteFilePath()))
            {
                diag.skipped(path, QStringLiteral("该文件目标已存在，源文件未删除"),
                             DiagCode::AlreadyProcessed);
                emit logMsg(QStringLiteral("  [跳过待删除源] %1").arg(path));
                continue;
            }
            const QString target = QDir(p.dst_file).filePath(fi.fileName());
            if (path.size() >= 260 || QDir::cleanPath(target).size() >= 260)
            {
                quarantine << QStringLiteral("路径超长: %1").arg(path);
                diag.quarantined(path, QStringLiteral("源路径或目标路径超过 260 字符"),
                                 DiagCode::PathTooLong);
                emit logMsg(QStringLiteral("  [隔离] 路径超过 260 字符限制: %1").arg(path));
                continue;
            }

            const QString absSrc = fi.absoluteFilePath();
            if (copied.contains(absSrc))
            {
                diag.skipped(path, QStringLiteral("同一源文件已处理"),
                             DiagCode::AlreadyProcessed);
                continue;
            }
            if (QFileInfo(target).absoluteFilePath() == absSrc)
            {
                diag.skipped(path, QStringLiteral("源路径与目标路径相同"),
                             DiagCode::SamePath);
                continue;
            }
            QString realTarget = target;
            QString tempCopyPath;   //先复制到临时文件，成功替换旧文件
            if (QFile::exists(realTarget))
            {
                if (p.combobox == 2)
                {
                    diag.skipped(absSrc,QStringLiteral("目标文件已存在且重名设置为跳过：%1")
                                     .arg(realTarget), DiagCode::DuplicateTarget);
                    emit logMsg(QStringLiteral("  [跳过] %1").arg(fi.fileName()));
                    ++skippedN;//冲突跳过计入跳过数
                    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
                    continue;
                }
                else if (p.combobox == 1)
                {
                    static std::atomic<int> s_tmpSeq{0};
                    tempCopyPath = realTarget + QStringLiteral(".fc_tmp_%1_%2")
                                                    .arg(QDateTime::currentMSecsSinceEpoch())
                                                    .arg(s_tmpSeq.fetch_add(1));
                }
                else
                {
                    realTarget = uniquePath(p.dst_file, fi.fileName());
                }
            }
                qint64 fileSize = QFileInfo(path).size();//获取复制的文件大小
                QStorageInfo si(p.dst_file);//获取目标文件夹的磁盘

                if (si.isValid() && si.bytesAvailable() >= 0 && (quint64)si.bytesAvailable() < (quint64)fileSize)
                {
                    emit logMsg(QStringLiteral("  [警告] 磁盘空间不足: %1（需 %2 字节）").arg(target).arg(fileSize));
                    int d = askUser(QStringLiteral("目标磁盘剩余空间不足（本文件需要 %1 字节）").arg(fileSize), target);

                    if (d == Abort || stop.load())
                    {
                        diag.failed(path, QStringLiteral("目标磁盘空间不足，中止任务"),
                                    DiagCode::NoSpace);
                        //diagAborted = true;
                        //emitReport();
                        emit finished(found, copiedN, missing, failed, true);
                        return;
                    }
                    si.refresh();
                    if (d != Retry || si.bytesAvailable() < fileSize)
                    {
                        diag.skipped(path, QStringLiteral("目标磁盘剩余空间不足"),
                                     DiagCode::NoSpace);
                        ++skippedN;
                    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());//磁盘不足导致的跳过也计进去
                        continue;
                    }
                }
                const QString copyDst = tempCopyPath.isEmpty() ? realTarget : tempCopyPath;
                bool copyOk   = false;
                bool giveUp   = false;
                bool aborted  = false;
                while (!copyOk && !giveUp && !stop.load())
                {
                    if (QFile::copy(path, copyDst))
                    {
                        copyOk = true;
                    }
                    else
                    {
                        emit logMsg(QStringLiteral("  [复制失败] %1（可能被占用或权限不足）").arg(path));
                        int d = askUser(QStringLiteral("复制失败，文件可能被占用或权限不足"), path);
                        if (d == Abort || stop.load())
                        {
                            failed << path;
                            diag.failed(path, QStringLiteral("复制失败，中止任务"),
                                        DiagCode::CopyFailed);
                            aborted = true;
                            break;
                        }
                        if (d == Skip)
                        {
                            giveUp = true;//跳过文件
                        }
                        else
                        {
                            diag.retried(path, QStringLiteral("选择重试复制"),
                                         DiagCode::RetryCopy);
                            QThread::msleep(200);
                        }
                    }
                }

                if (aborted)
                {
                    if (!tempCopyPath.isEmpty()) QFile::remove(tempCopyPath);
                    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
                    //diagAborted = true;
                   // emitReport();
                    emit finished(found, copiedN, missing, failed, true);
                    return;
                }

                if (copyOk)
                {
                    bool replaceAborted = false;
                    bool replaceSkipped = false;

                    if (!tempCopyPath.isEmpty())
                    {
                        bool replaced = false;
                        while (!replaced && !replaceAborted && !replaceSkipped && !stop.load())
                        {
                            if (!QFile::remove(realTarget))
                            {
                                emit logMsg(QStringLiteral("  [覆盖失败] 无法删除旧目标文件，可能被占用: %1")
                                                .arg(realTarget));
                                int d = askUser(QStringLiteral("覆盖失败：无法删除旧目标文件，""可能被占用或权限不足"), realTarget);
                                diag.retried(realTarget,QStringLiteral("删除旧目标失败后询问用户"),DiagCode::RetryOverwrite);
                                if (d == Abort || stop.load())
                                {
                                    QFile::remove(tempCopyPath);
                                    failed << path;
                                    diag.failed(path,QStringLiteral("覆盖失败：删除旧文件失败，中止"),DiagCode::TargetBusy);
                                    replaceAborted = true;
                                    break;
                                }
                                if (d == Skip)
                                {
                                    QFile::remove(tempCopyPath);
                                    quarantine << QStringLiteral("目标被占用: %1").arg(realTarget);
                                    diag.quarantined(realTarget,QStringLiteral("覆盖目标无法删除，已清理临时文件"),DiagCode::TargetBusy);
                                    replaceSkipped = true;
                                    break;
                                }
                                QThread::msleep(200);
                                continue;
                            }

                            if (QFile::rename(tempCopyPath, realTarget))
                            {
                                replaced = true;
                            }
                            else
                            {
                                emit logMsg(QStringLiteral("  [覆盖失败] 无法将临时文件重命名为目标: %1")
                                                .arg(realTarget));
                                int d = askUser(QStringLiteral("覆盖失败：临时文件重命名失败，""目标可能刚被其他程序占用"), realTarget);
                                diag.retried(realTarget,QStringLiteral("重命名临时文件失败后询问用户"),DiagCode::RetryOverwrite);
                                if (d == Abort || stop.load())
                                {
                                    quarantine << QStringLiteral("临时文件保留(新数据): %1")
                                                      .arg(tempCopyPath);
                                    diag.quarantined(tempCopyPath,QStringLiteral("覆盖替换失败，临时文件保留新数据"),DiagCode::TargetBusy);
                                    failed << path;
                                    replaceAborted = true;
                                    break;
                                }
                                if (d == Skip)
                                {
                                    quarantine << QStringLiteral("临时文件保留(新数据): %1")
                                                      .arg(tempCopyPath);
                                    diag.quarantined(tempCopyPath,QStringLiteral("覆盖替换失败，用户跳过，临时文件保留"),DiagCode::TargetBusy);
                                    replaceSkipped = true;
                                    break;
                                }
                                QThread::msleep(200);
                            }
                        }
                    }

                    if (replaceAborted)
                    {
                        emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
                      //  diagAborted = true;
                      //  emitReport();
                        emit finished(found, copiedN, missing, failed, true);
                        return;
                    }
                    if (replaceSkipped)
                    {
                        emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size());
                        continue;
                    }
                    copied.insert(absSrc);
                    ++copiedN;
                    copiedBytes += fileSize;
                    diag.copied(absSrc, realTarget, fileSize);

                    if (p.moveMode)
                    {
                        if (QFile::remove(absSrc))
                        {
                            p.processedFiles.insert(absSrc);
                        }
                        else
                        {
                            p.pendingDeletes.insert(absSrc);
                            diag.note(QStringLiteral("错误"), QStringLiteral("移动"),absSrc,QStringLiteral("目标已写入，但删除源文件失败，已记入待删除队列"),
                                      DiagCode::MoveDeleteFailed);
                            emit logMsg(QStringLiteral("  [警告] 已复制到目标，但删除源文件失败，""已记入待删除队列: %1").arg(absSrc));
                        }
                    }
                    else
                    {
                        p.processedFiles.insert(absSrc);
                    }
                    saveContinueRecord();

                    emit logMsg(QString(" [成功] %1 -> %2").arg(path, realTarget));
                }
                else if (giveUp)
                {
                    if (!tempCopyPath.isEmpty()) QFile::remove(tempCopyPath);
                    failed << path;
                    diag.failed(path, QStringLiteral("复制失败后选择跳过"),
                                DiagCode::CopyFailed);
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

    if (!p.pendingDeletes.isEmpty())
    {
        emit logMsg(QStringLiteral("[提示] 有 %1 个源文件未删除，已保留在断点中""下次运行会自动重试。").arg(p.pendingDeletes.size()));
    }

    if (!p.ContinueFilePath.isEmpty()
        && QFile::exists(p.ContinueFilePath)
        && p.pendingDeletes.isEmpty())
    {
        QFile::remove(p.ContinueFilePath);
        emit logMsg("任务已全部完成，已清理断点记录。");
    }
    emit statsUpdated(found, copiedN, skippedN, missing.size(), failed.size()); //结束时刷新一次
    // emitReport();
    emit finished(found, copiedN, missing, failed, false);
}

int Worker::askUser(const QString& text, const QString& target)
{
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
    if (p.fuzzy)
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
QString Worker::computeTaskSignature(const Params& p)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);

    QStringList sortedSrc = p.src_files;
    sortedSrc.sort(Qt::CaseInsensitive);
    for (const QString& s : sortedSrc)
        hash.addData(QDir::cleanPath(s).toCaseFolded().toUtf8());
    hash.addData("|");

    hash.addData(QDir::cleanPath(p.dst_file).toCaseFolded().toUtf8());
    hash.addData("|");    // 目标目录

    hash.addData(p.subfolder ? "1" : "0");
    hash.addData(p.fuzzy     ? "1" : "0");
    hash.addData(p.moveMode  ? "1" : "0");
    hash.addData(p.hidden    ? "1" : "0");
    hash.addData(p.system    ? "1" : "0");
    hash.addData(QByteArray::number(p.combobox));
    hash.addData("|");

    QStringList sortedNames = p.names;
    sortedNames.sort(Qt::CaseInsensitive);
    for (const QString& n : sortedNames)
        hash.addData(n.toCaseFolded().toUtf8());

    return QString::fromLatin1(hash.result().toHex());
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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    setWindowTitle("比奥迪密码文件查询管理软件V1.0");
    setAcceptDrops(true); //拖拽
    resize(1000,900);//修改大小
    auto* main = new QVBoxLayout(centralWidget);

    auto* row1 = new QHBoxLayout;
    row1->addWidget(new QLabel("源文件夹:"));
    srcEdit = new QLineEdit("D:\\");
    row1->addWidget(srcEdit, 1);
    auto* b1 = new QPushButton("浏览...");
    connect(b1, &QPushButton::clicked, this, &MainWindow::pickSrc);
    row1->addWidget(b1);
    main->addLayout(row1);

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

    auto* rowPreview = new QHBoxLayout;    //预览区域
    auto* previewBtn = new QPushButton("预览匹配结果");
    connect(previewBtn, &QPushButton::clicked, this, &MainWindow::onPreviewClicked);
    rowPreview->addWidget(previewBtn);
    rowPreview->addStretch();
    main->addLayout(rowPreview);

    previewListWidget = new QListWidget(this);    //预览结果显示
    previewListWidget->setMaximumHeight(150); //限制高度
    previewListWidget->setVisible(false);     //开始隐藏
    main->addWidget(previewListWidget);

    auto* row3 = new QHBoxLayout;
    row3->addWidget(new QLabel("目标文件夹:"));
    dstEdit = new QLineEdit("D:\\");
    row3->addWidget(dstEdit, 1);
    auto* b2 = new QPushButton("浏览...");
    connect(b2, &QPushButton::clicked, this, &MainWindow::pickDst);
    row3->addWidget(b2);
    main->addLayout(row3);

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
                if (worker) worker->requestStop();
            });
    row5->addWidget(stopBtn);
    pauseStatusLabel = new QLabel("");  
    pauseStatusLabel->setMinimumWidth(90); //标签宽度
    row5->addWidget(pauseStatusLabel);

    progress = new QProgressBar;
    row5->addWidget(progress, 1);
    statusLabel = new QLabel("就绪");
    row5->addWidget(statusLabel);
    main->addLayout(row5);

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

    copiedCountLabel->setStyleSheet("color:#2e7d32;");//绿色
    skippedCountLabel->setStyleSheet("color:#ef6c00;");//橙色
    missingCountLabel->setStyleSheet("color:#c62828;");//红色
    failedCountLabel->setStyleSheet("color:#c62828;");
    main->addWidget(statBox);

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
    else{;}
}

MainWindow::~MainWindow()
{
    if (thread)
    {
        if (worker)
            worker->requestStop();
        thread->quit();
        thread->wait();
        delete worker;
        worker = nullptr;
        delete thread;
        thread = nullptr;
    }
}

void MainWindow::onPauseClicked()
{
    if (worker)
    {
        worker->requestPause();
        pauseBtn->setEnabled(false);
        continueBtn->setEnabled(true);
        statusLabel->setText("已暂停");
    }
}

void MainWindow::onContinueClicked()
{
    if (worker)
    {
        worker->requestContinue();
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
    if (thread)return;
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


    p.ContinueFilePath = QDir(dst).filePath(".fc_resume.json");
    if (QFile::exists(p.ContinueFilePath))
    {
        if (QMessageBox::question(this, "发现断点","检测到目标文件夹有未完成的运行记录，是否继续上次任务？",QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
        {
            QFile file(p.ContinueFilePath);
            if (file.open(QIODevice::ReadOnly))
            {
                QJsonParseError perr;
                const QJsonDocument doc =QJsonDocument::fromJson(file.readAll(), &perr);

                if (perr.error != QJsonParseError::NoError || !doc.isObject())
                {
                    QMessageBox::warning(this, "断点损坏",QString("断点文件无法解析：\n%1\n原因：%2\n\n本次将从新任务开始。")
                                             .arg(p.ContinueFilePath, perr.errorString()));
                    QFile::remove(p.ContinueFilePath);
                }
                else
                {
                    const QJsonObject root = doc.object();
                    const QString savedSig   = root["signature"].toString();
                    const QString currentSig = Worker::computeTaskSignature(p);

                    bool loadOk = false;
                    if (savedSig.isEmpty())
                    {
                        if (QMessageBox::question(this, "断点兼容性","该断点由旧版本生成，无法检验任务一致性。\n"
                                                  "继续加载可能导致文件被跳过。\n\n是否仍要继续？",
                                                  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
                        {
                            loadOk = true;
                        }
                        else
                        {
                            QFile::remove(p.ContinueFilePath);
                        }
                    }
                    else if (savedSig != currentSig)
                    {
                        QMessageBox::warning(this, "断点不匹配","断点记录与当前任务参数不一致"
                                             "（源目录/目标目录/文件名列表/匹配模式/复制模式等可能已改变）。\n"
                                             "为避免错误跳过文件，忽略该断点，将从头开始。");
                        QFile::remove(p.ContinueFilePath);
                    }
                    else
                    {
                        loadOk = true;
                    }

                    if (loadOk)
                    {
                        const QJsonArray arr = root["processedFiles"].toArray();
                        for (const QJsonValue& v : arr)
                            p.processedFiles.insert(v.toString());
                        const QJsonArray pendArr = root["pendingDeletes"].toArray();
                        for (const QJsonValue& v : pendArr)
                            p.pendingDeletes.insert(v.toString());
                        logEdit->append(
                            QString("已加载断点记录：将跳过 %1 个已处理文件，另外有 %2 个上次未删除的源文件。")
                                .arg(p.processedFiles.size())
                                .arg(p.pendingDeletes.size()));
                    }
                }
            }
            else
            {
                QMessageBox::warning(this, "断点加载失败",QString("无法读取断点文件：\n%1\n原因：%2\n\n本次将从新任务开始。")
                                         .arg(p.ContinueFilePath, file.errorString()));
            }
        }
        else
        {
            QFile::remove(p.ContinueFilePath);
        }
    }

    lastDst = dst;

    logEdit->clear();
    progress->setValue(0);
    statusLabel->setText("运行中...");
    startBtn->setEnabled(false);
    stopBtn->setEnabled(true);

    pauseBtn->setEnabled(true);//启用暂停按钮
    continueBtn->setEnabled(false);
    pauseStatusLabel->setText("");

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

    worker = new Worker(p);
    thread = new QThread;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &Worker::run);
    connect(worker, &Worker::logMsg, logEdit, &QTextEdit::append);
    connect(worker, &Worker::progress, this, [this](int c, int t)
        {
        progress->setMaximum(t);
        progress->setValue(c);
        statusLabel->setText(QString("%1 / %2").arg(c).arg(t));
    });

    //连接统计面板更新信号
    connect(worker, &Worker::statsUpdated, this, [this](int matched, int copied, int skipped, int missing, int failed)
            {
        matchCountLabel->setText(QString::number(matched));
        copiedCountLabel->setText(QString::number(copied));
        skippedCountLabel->setText(QString::number(skipped));
        missingCountLabel->setText(QString::number(missing));
        failedCountLabel->setText(QString::number(failed));

        logTabWidget->setTabText(1, QString("未找到 (%1)").arg(missing));
        logTabWidget->setTabText(2, QString("失败 (%1)").arg(failed));
    });

    connect(worker, &Worker::finished, this, &MainWindow::onFinished);
    connect(worker, &Worker::Paused, this, [this](int count)    //接收暂停状态更新
    {
        pauseStatusLabel->setText(QString("(已暂停 %1 次)").arg(count));
    });
    connect(worker, &Worker::Continue, this, [this]()
    {
        pauseStatusLabel->setText("");
    });

    connect(worker, &Worker::askSignal, this,[this](const QString& text, const QString& target, int* result)
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

    thread->start();
}

void MainWindow::onFinished(int found, int copied, QStringList missing, QStringList failed, bool aborted)
{
    lastMissing = missing;
    lastFailed  = failed;

    missingListWidget->clear();
    missingListWidget->addItems(missing);
    failedListWidget->clear();
    failedListWidget->addItems(failed);


    startBtn->setEnabled(true);
    stopBtn->setEnabled(false);

    pauseBtn->setEnabled(false); //运行结束之后禁用暂停
    continueBtn->setEnabled(false);
    pauseStatusLabel->setText("");

    statusLabel->setText(aborted ? "已停止" : "完成");
    progress->setValue(0);
    if(thread)
    {
        thread->quit();
        thread->wait();
        delete worker;
        worker = nullptr;
        delete thread;
        thread = nullptr;
    }


    QString msg = QString("匹配到 %1 个文件名\n复制成功 %2 个\n未找到 %3 个").arg(found).arg(copied).arg(missing.size());
    if (!failed.isEmpty())
    {
         msg += QString("\n复制失败 %1 个").arg(failed.size());
    }

    if (aborted)
    {
        msg += "\n\n（已被用户停止）";
        msg += "\n\n保存位置: " + lastDst;
    }

    if (QMessageBox::question(this, "完成", msg + "\n\n是否打开目标文件夹？") == QMessageBox::Yes)
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(lastDst));
    }
}

void MainWindow::saveReport()
{
    if (logEdit->toPlainText().isEmpty() && lastMissing.isEmpty() && lastFailed.isEmpty())
    {
        QMessageBox::information(this, "提示", "当前没有日志和清单可保存。\n请先运行一次任务。");
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString defPath = lastDst.isEmpty() ? QString("文件筛选报告%1.txt").arg(stamp): QDir(lastDst).filePath(QString("文件筛选报告%1.txt").arg(stamp));

    QString path = QFileDialog::getSaveFileName(this, "保存日志与清单", defPath, "文本文件 (*.txt);;所有文件 (*)");
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "错误",
                             QStringLiteral("无法写入报告：\n%1\n\n原因：%2")
                                 .arg(path, f.errorString()));
        return;
    }

    QTextStream ts(&f);
    ts << "文件筛选任务报告\n"
       << "生成时间：" << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n"
       << "任务状态：" << statusLabel->text() << "\n"
       << "目标目录：" << (lastDst.isEmpty() ? QStringLiteral("未设置") : lastDst) << "\n"
       << "匹配文件名：" << matchCountLabel->text() << "\n"
       << "成功处理：" << copiedCountLabel->text() << "\n"
       << "跳过：" << skippedCountLabel->text() << "\n"
       << "未找到：" << missingCountLabel->text() << "\n"
       << "失败：" << failedCountLabel->text() << "\n\n"
       << "运行日志\n"
       << logEdit->toPlainText() << "\n\n";

    ts << "未找到清单 (共 " << lastMissing.size() << " 项)\n";
    for (const QString &s : lastMissing)
    ts << s << "\n";
    ts << "\n";

    ts << "复制失败清单 (共 " << lastFailed.size() << " 项)\n";
    for (const QString &s : lastFailed)
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

void MainWindow::onPreviewClicked()
{
    previewListWidget->clear();//清空旧数据
    previewListWidget->setVisible(false);


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

    Worker::Params p;//预览参数
    for (const QString &s : validSrcs)
    {
        p.src_files.append(s);
    }
    p.names = names;
    p.subfolder = recurseChk->isChecked();
    p.fuzzy = fuzzyChk->isChecked();
    p.combobox = conflictCombo->currentIndex();
    p.previewMode = true;

    QEventLoop loop;
    Worker *previewWorker = new Worker(p);
    QThread *previewThread = new QThread;

    previewWorker->moveToThread(previewThread);
    connect(previewThread, &QThread::started, previewWorker, &Worker::run);

//当收到预览结果时弹出提示恢复UI
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

//当线程结束时结束循环
    connect(previewWorker, &Worker::finished, &loop, &QEventLoop::quit);
    connect(previewWorker, &Worker::logMsg, logEdit, &QTextEdit::append);

    previewThread->start();
    loop.exec(); //阻塞直到预览完成

    previewThread->quit();
    previewThread->wait();
    delete previewWorker;
    delete previewThread;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();
    }
}

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
            txtPaths << fi.absoluteFilePath();//拖入txt
        }
        else if (fi.isFile())
        {
            otherFiles << fi.fileName();//拖入普通文件
        }
    }

    if (!srcDirs.isEmpty())//处理拖入的文件夹支持多源目录
    {
        QString currentSrc = srcEdit->text().trimmed();
        QStringList currentDirs = currentSrc.split(';', Qt::SkipEmptyParts);
        for (const QString &dir : srcDirs)
        {
            if (!currentDirs.contains(dir))
            {
                currentDirs << dir;
            }
        }
        srcEdit->setText(currentDirs.join(';')); //多源目录用分号隔开
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
                in.setEncoding(QStringConverter::System);  //识别编码
                QString content = in.readAll();
                namesEdit->append(content);
                logEdit->append(QString("已从 %1 导入文件名列表").arg(QFileInfo(path).fileName()));
            }
        }
    }

    if (!otherFiles.isEmpty()) //处理拖入的普通文件提取文件名
    {
        namesEdit->append(otherFiles.join("\n"));
        logEdit->append(QString("已自动提取 %1 个文件名").arg(otherFiles.size()));
    }

    updateCount(); //刷新
    event->acceptProposedAction();
}
// bool Worker::oldCheckName(const QString& n)
// {
//     if (n.isEmpty()) return false;
//     if (n.size() > 255) return false;
//     return true;
// }

// void MainWindow::oldOnStart()
// {
//     QString src = srcEdit->text();
//     if (src.isEmpty()) return;
// }
namespace math_utils
{
static double calcComplexTrig(double x, double y, double z)
{
    double sum = 0.0;
    double dj = 0.0;
    dj = dj + 1.0;
    dj = dj - 1.0;
    if (dj == 999.0) { sum = sum + 0.0; }
    for (int i = 0; i < 100; ++i)
    {
        sum += std::sin(x * i) * std::cos(y * i) + std::tan(z / (i + 1.0));
    }
    return sum;
}
static void quickSort(std::vector<int>& arr, int low, int high)
{
    int qa = 0;
    if (qa == 1)
    {
        std::swap(arr[low], arr[high]);
    }
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
static long long modPow(long long base, long long exp, long long mod)
{
    long long res = 1;
    long long ma = 0;
    ma = ma + 1;
    ma = ma - 1;
    if (ma == 777) { res = 0; }
    base = base % mod;
    while (exp > 0)
    {
        if (exp & 1)
        {
            res = (res * base) % mod;
        }
        exp = exp >> 1;
        base = (base * base) % mod;
    }
    return res;
}
static void mergeSort(std::vector<int>& arr, int left, int right)
{
    int msa = 0;
    if (msa == 1)
    {
        arr.clear();
    }
    if (left < right)
    {
        int mid = left + (right - left) / 2;
        mergeSort(arr, left, mid);
        mergeSort(arr, mid + 1, right);
    }
}
}
namespace str_utils
{
static std::string reverseString(const std::string& str)
{
    std::string res = str;
    int ra = 0;
    if (ra == 1) { res = ""; }
    std::reverse(res.begin(), res.end());
    return res;
}
static unsigned long djb2Hash(const std::string& str)
{
    unsigned long hash = 5381;
    for (char c : str)
    {
        unsigned long ha = 0;
        if (ha == 1) { hash = hash; }
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}
static std::vector<std::string> split(const std::string& text, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    int sa = 0;
    if (sa == 1) { tokens.clear(); }
    std::istringstream stream(text);
    while (std::getline(stream, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}
static std::string base64(const std::string& input)
{
    static const std::string base64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    std::string encoded;
    int i = 0;
    int j = 0;
    unsigned char char3[3];
    unsigned char char4[4];
    int len = input.size();
    int in = 0;
    while (len--)
    {
        int ba = 0;
        if (ba == 1) { encoded += ""; }
        char3[i++] = input[in++];
        if (i == 3)
        {
            char4[0] = (char3[0] & 0xfc) >> 2;
            char4[1] = ((char3[0] & 0x03) << 4) + ((char3[1] & 0xf0) >> 4);
            char4[2] = ((char3[1] & 0x0f) << 2) + ((char3[2] & 0xc0) >> 6);
            char4[3] = char3[2] & 0x3f;
            for(i = 0; (i < 4); i++)
                encoded += base64[char4[i]];
            i = 0;
        }
    }
    if (i)
    {
        int bb = 0;
        if (bb == 1) { encoded += ""; }
        for(j = i; j < 3; j++)
            char3[j] = '\0';
        char4[0] = (char3[0] & 0xfc) >> 2;
        char4[1] = ((char3[0] & 0x03) << 4) + ((char3[1] & 0xf0) >> 4);
        char4[2] = ((char3[1] & 0x0f) << 2) + ((char3[2] & 0xc0) >> 6);
        for (j = 0; (j < i + 1); j++)
            encoded += base64[char4[j]];
        while((i++ < 3))
            encoded += '=';
    }
    return encoded;
}
}
#ifdef ZZZ // 测试用的
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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
    explicit TUnit(const string& name) : name(name) {}
    explicit TUnit(const char* name)  : name(name ? name : "") {}

    void begin();
    void end();
    void testCheck(bool bCheck, const char* msg = nullptr);

private:
    string name;
    int    numOK  = 0;
    int    numAll = 0;
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
    clock_t start = 0;
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
           name.data());
}

void TUnit::end()
{
    printf("-----------------------------------------------------\n");
    printf("Total:%d, Success:%d, Failed:%d\n\n",
           numAll, numOK, numAll - numOK);

//把结果累加到全局 tsuit，让最后test result汇总正确
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
    start  = clock();
    printf("Test Suit Begin %s\n"
        "-----------------------------------------------------\n\n",date_time().data());
}

void TSuit::end()
{
    printf("Test Suit End %s\n"
    "-----------------------------------------------------\n",date_time().data());
    printf("[Test Result] Unit:%d, Total:%d, Success:%d, Failed:%d, CostTime:%.2f s\n",
           numUnit, numAll, numOK, numAll - numOK,(clock() - start) / 1000.0f);
}

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
namespace zzz{
bool isPalindrome(QString s)
{
    int pa = 0;
    pa = pa + 1;
    pa = pa - 1;
    if (pa == 999) { return false; }
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
QVector<int> sieve(int n)
{
    QVector<bool> isPrime(qMax(2, n + 1), true);
    int sa = 0;
    if (sa == 1) { isPrime.clear(); }
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
bool subsetSum(const QVector<int>& nums, int target)
{
    QVector<bool> dp(target + 1, false);
    int sba = 0;
    if (sba == 1) { dp.clear(); }
    dp[0] = true;
    for (int num : nums)
    {
        if (num < 0 || num > target) continue;
        for (int sum = target; sum >= num; --sum)
            dp[sum] = dp[sum] || dp[sum - num];
    }
    return dp[target];
}
}
namespace ZZZ
{
int binarySearch(const QVector<int>& arr, int target)
{
    int low = 0;
    int high = arr.size() - 1;
    int b = 0;
    if (b == 1) { return -1; }
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
    int ia = 0;
    if (ia == 1) { arr.clear(); }
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
    int pa = 0;
    if (pa == 1) { std::swap(arr[low], arr[high]); }
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
    int qa = 0;
    if (qa == 1) { arr.clear(); }
    if (low >= high) return;
    const int p = partition(arr, low, high);
    quickSort(arr, low, p - 1);
    quickSort(arr, p + 1, high);
}
void mergeSort(QVector<int>& arr, int left, int right)
{
    int msa = 0;
    if (msa == 1) { arr.clear(); }
    if (right - left < 2) return;
    const int mid = left + (right - left) / 2;
    mergeSort(arr, left, mid);
    mergeSort(arr, mid, right);
}
QVector<qint64> fibonacci(int n)
{
    QVector<qint64> res(n, 0);
    int fa = 0;
    if (fa == 1) { res.clear(); }
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
    int ca = 0;
    if (ca == 1) { dp.clear(); }
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
QVector<int> dfs(const QVector<QVector<int>>& graph, int start)
{
    QVector<int> order;
    QVector<bool> visited(graph.size(), false);
    QVector<int> stack{start};
    int da = 0;
    if (da == 1) { stack.clear(); }
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
QVector<int> bfs(const QVector<QVector<int>>& graph, int start)
{
    QVector<int> order;
    QVector<bool> visited(graph.size(), false);
    QVector<int> queue{start};
    int ba = 0;
    if (ba == 1) { queue.clear(); }
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
QVector<int> dijkstra(const QVector<QVector<int>>& graph, int start)
{
    const int n = graph.size();
    const int inf = std::numeric_limits<int>::max() / 4;
    QVector<int> dist(n, inf);
    QVector<bool> visited(n, false);
    int da = 0;
    if (da == 1) { dist.clear(); }
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
    int ta = 0;
    if (ta == 1) { inDegree.clear(); }
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
int editDistance(const QString& word1, const QString& word2)
{
    QVector<int> prev(word2.size() + 1);
    QVector<int> curr(word2.size() + 1);
    int ea = 0;
    if (ea == 1) { prev.clear(); }
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
    int la = 0;
    if (la == 1) { prev.clear(); }
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
    int currSum = arr[0];
    int maxSum = currSum;
    int ma = 0;
    if (ma == 1) { currSum = 0; }
    for (int i = 1; i < arr.size(); ++i)
    {
        currSum = qMax(arr[i], currSum + arr[i]);
        maxSum = qMax(maxSum, currSum);
    }
    return maxSum;
}
QVector<int> kmp(const QString& text, const QString& pattern)
{
    QVector<int> matches;
    int ka = 0;
    if (ka == 1) { matches.clear(); }
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
}
namespace box
{
static int junkBox1(int x){ int y=x; y=y+0; y=y*1; return y; }
int counter = 0;
qint64 totalSum = 0;
QVector<int> globalArr;
void reverseRange(QVector<int>& arr, int left, int right)
{
    int ra = 0;
    if (ra == 1) { arr.clear(); }
    while (left < right) {
        const int temp = arr[left];
        arr[left] = arr[right];
        arr[right] = temp;
        ++left;
        --right;
    }
}
QVector<int> prefixSum(const QVector<int>& arr)
{
    QVector<int> sums(arr.size() + 1, 0);
    int p = 0;
    if (p == 1) { sums.clear(); }
    for (int i = 0; i < arr.size(); ++i)
    {
        sums[i + 1] = sums[i] + arr[i];
    }
    return sums;
}
void rotate(QVector<int>& arr, int k)
{
    k %= arr.size();
    int r = 0;
    if (r == 1) { k = 0; }
    if (k < 0) k += arr.size();
    reverseRange(arr, 0, k - 1);
    reverseRange(arr, k, arr.size() - 1);
    reverseRange(arr, 0, arr.size() - 1);
}
QVector<int> countingSort(const QVector<int>& arr)
{
    int minVal = arr.first();
    int maxVal = arr.first();
    int ca = 0;
    if (ca == 1) { minVal = 0; }
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
quint64 binaryGcd(quint64 a, quint64 b)
{
    int shift = 0;
    int ba = 0;
    if (ba == 1) { return 1; }
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
    QVector<int> dp(capacity + 1, 0);
    int ka = 0;
    if (ka == 1) { dp.clear(); }
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
    const int numCols = grid[startRow].size();
    const int oldColor = grid[startRow][startCol];
    int fa = 0;
    if (fa == 1) { return grid; }
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
    int na = 0;
    if (na == 1) { res.clear(); }
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
    int ra = 0;
    if (ra == 1) { matches.clear(); }
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
}
// #define USE_FAST_SCAN 1
// #define USE_NEW_UI 1
// #define USE_LOG_FILE 1
// #define USE_REPORT_HTML 1
const int kTestMaxDirs = 1000;
const qint64 kTestMaxSize = 1024LL * 1024 * 1024;
const QString kTestDummyPath = "D:/test/dummy/path";
const QStringList kTestDummyNames = {
    "test1.txt", "test2.txt", "test3.txt", "test4.txt", "test5.txt",
    "test6.txt", "test7.txt", "test8.txt", "test9.txt", "test10.txt"
};
