#include "ConfigReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QDebug>

static inline QString AppDir()
{
    return QCoreApplication::applicationDirPath();
}

QString ConfigReader::resolvePathFromApp(const QString& p)
{
    if (p.isEmpty()) return {};
    QFileInfo fi(p);
    if (fi.isAbsolute()) return QDir::cleanPath(p);
    return QDir::cleanPath(QDir(AppDir()).filePath(p));
}

ConfigReader::ConfigReader(const QString& iniRelativeToApp)
{
    iniPath_ = QDir(AppDir()).filePath(iniRelativeToApp);
}

bool ConfigReader::reload()
{
    lastError_.clear();

    paths_ = Paths{};
    paths_.iniPath = iniPath_;

    QFileInfo iniFi(iniPath_);
    if (!iniFi.exists())
    {
        lastError_ = QString("config.ini not found: %1").arg(iniPath_);
        return false;
    }

    QSettings ini(iniPath_, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const QString onnx = ini.value("Paths/onnx", "lama_fp32.onnx").toString();
    const QString inDir = ini.value("Paths/input", "input").toString();
    const QString outDir = ini.value("Paths/output", "output").toString();
    const QString lastOpneDir = ini.value("Paths/lastopendir", "").toString();

    paths_.onnxPath = resolvePathFromApp(onnx);
    paths_.inputDir = resolvePathFromApp(inDir);
    paths_.outputDir = resolvePathFromApp(outDir);
    paths_.lastOpenDir = resolvePathFromApp(lastOpneDir);

    // 校验：至少 onnx 得存在
    QFileInfo onnxFi(paths_.onnxPath);
    if (!onnxFi.exists())
    {
        lastError_ = QString("onnx not found: %1").arg(paths_.onnxPath);
        return false;
    }

    // 输入/输出目录不存在就创建
    QDir().mkpath(paths_.inputDir);
    QDir().mkpath(paths_.outputDir);

    return true;
}

bool ConfigReader::isValid() const
{
    return lastError_.isEmpty();
}

const ConfigReader::Paths& ConfigReader::paths()
{
    reload();
    return paths_;
}

QString ConfigReader::lastError() const
{
    return lastError_;
}

void ConfigReader::setLastOpenDir(const QString& path)
{
    if (path.isEmpty()) return;
    QString cleanPath = QDir::toNativeSeparators(path);
    
    // 1. 写入配置文件
    QSettings ini(iniPath_, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    ini.setValue("Paths/lastopendir", path);
    ini.sync();                                                                 // 强制立即写入磁盘

    // 2. 同步更新内存中的数据，避免下次 reload 之前数据不一致
    paths_.lastOpenDir = path;
}