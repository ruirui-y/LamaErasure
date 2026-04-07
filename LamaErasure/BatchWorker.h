#ifndef BATCHWORKER_H
#define BATCHWORKER_H

#include <QThread>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QDir>

#include "LamaOrt.h"
#include "AlignToReference.h"
#include "Global.h"

class BatchWorker : public QObject {
    Q_OBJECT
public:
    BatchWorker(LamaOrt::Ptr ort,
        AlignToReference::Ptr align,
        const QString& inDir,
        const QString& outDir,
        const QString& labelDir,
        std::atomic_bool* cancelFlag)
        : ort_(ort), align_(align),inDir_(inDir), outDir_(outDir),
         labelDir_(labelDir), cancel_(cancelFlag)
    {

    }

signals:
    void progress(int done, int total, const QString& file);
    void finished(bool canceled, const QString& msg);

public slots:
    void run() 
    {
        if (!ort_) { emit finished(false, "ort_ is null"); return; }

        // 收集图片
        QStringList filters = { "*.png","*.jpg","*.jpeg","*.bmp","*.tif","*.tiff","*.webp" };
        QDirIterator it(inDir_, filters, QDir::Files, QDirIterator::Subdirectories);

        QStringList files;
        while (it.hasNext()) files.push_back(it.next());

        const int total = files.size();
        if (total == 0) { emit finished(false, "No images found."); return; }

        // 确保输出目录存在
        QDir().mkpath(outDir_);
        QDir().mkpath(labelDir_);

        for (int i = 0; i < total; ++i) 
        {
            // 0. 中断检查
            if (cancel_ && cancel_->load()) 
            {
                emit finished(true, "Canceled.");
                return;
            }

            const QString inPath = files[i];
            QFileInfo fi(inPath);

            // 1. 读取原图
            QImageReader reader(inPath);
            reader.setAutoTransform(true);
            QImage img = reader.read();
            if (img.isNull()) 
            {
                emit progress(i + 1, total, fi.fileName() + " (read failed)");
                continue;
            }

            // 2. 自动化对齐与标签/掩码生成
            bool ok = false;
            QList<QPointF> label_points;
            QRectF target_box;
            QImage maskForThis = align_->MakeMaskFor(img, &label_points, &target_box, &ok);
            if (!ok || maskForThis.isNull())
            {
                continue;
            }

            // 3. 掩码尺寸校准
            if (maskForThis.size() != img.size()) {
                maskForThis = maskForThis.scaled(img.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
            }

            // 4. AI 推理擦除 (Lama 填充)
            QImage out = ort_->Run(img, maskForThis);
            if (out.isNull()) {
                emit progress(i + 1, total, fi.fileName() + " (inpaint failed)");
                continue;
            }

            // 5. 保存擦除后的净图
            const QString outPath = QDir(outDir_).filePath(fi.fileName());
            QImageWriter writer(outPath);
            if (fi.suffix().toLower() == "jpg" || fi.suffix().toLower() == "jpeg") writer.setQuality(95);

            if (!writer.write(out)) {
                emit progress(i + 1, total, fi.fileName() + " (save failed)");
                continue;
            }

            // 6. 保存对应的 YOLO 标签文件
            QString labelPath = QDir(labelDir_).filePath(fi.baseName() + ".txt");
            saveYoloLabels(target_box, label_points, labelPath, img.size());

            emit progress(i + 1, total, fi.fileName());
        }

        emit finished(false, "Done.");
    }

private:
    LamaOrt::Ptr ort_ = nullptr;
    AlignToReference::Ptr align_ = nullptr;
    QString inDir_;
    QString outDir_;
    QString labelDir_;

    std::atomic_bool* cancel_ = nullptr;
};

#endif // BATCHWORKER_H