#ifndef CONFIGREADER_H
#define CONFIGREADER_H

#include <QString>
#include "singletion.h"

class ConfigReader : public Singleton<ConfigReader>
{
    friend class Singleton<ConfigReader>;
public:
    struct Paths
    {
        QString iniPath;                                                        // 实际使用的 config.ini 绝对路径
        QString onnxPath;                                                       // onnx 模型绝对路径
        QString inputDir;                                                       // 输入目录绝对路径
        QString outputDir;                                                      // 输出目录绝对路径
        QString lastOpenDir;                                                    // 最后打开的目录绝对路径
    };

public:
    // iniRelativeToApp: 默认 "Config/config.ini"
    explicit ConfigReader(const QString& iniRelativeToApp = "Config/config.ini");
    
    bool reload();                                                              // 重新读取 ini
    bool isValid() const;                                                       // onnx 路径是否存在（至少有这个）

    void setLastOpenDir(const QString& path);                                   // 设置最后一次打开的路径
    const Paths& paths();                                                       // 读取到的路径
    QString lastError() const;                                                  // 错误信息

    // 方便：把 ini 里读到的相对路径转绝对路径（相对 exe 目录）
    static QString resolvePathFromApp(const QString& p);

private:
    QString iniPath_;
    Paths paths_;
    QString lastError_;
};

#endif // CONFIGREADER_H