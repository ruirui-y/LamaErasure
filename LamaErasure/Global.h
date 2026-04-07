#ifndef GLOBAL_H
#define GLOBAL_H

#include <QImage>
#include <functional>
#include <QList>
#include <QPointF>
#include <QDebug>
#include <QRegularExpression>
#include <QFile>
#include <QTextStream>
#include <QSize>


using ImageCallback = std::function<void(const QImage&)>;

// 保存标签
inline void saveYoloLabels(const QList<QPointF>& points, const QString& imgPath, const QSize& imgSize)
{
    // 1. 将图片路径后缀改为 .txt
    QString labelPath = imgPath;
    labelPath.replace(QRegularExpression("\\.(jpg|png|jpeg|bmp)$"), ".txt");

    QFile file(labelPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "无法创建标签文件: " << labelPath;
        return;
    }

    QTextStream out(&file);

    // 2. 准备归一化参数
    double w = imgSize.width();
    double h = imgSize.height();

    // 设定标签框的大小（因为穴位是点，我们给一个极小的固定框，比如图片宽高的 1%）
    // 也可以根据你的 brushRadius 动态计算：double boxSize = (brushRadius * 2) / w;
    double boxW = 0.01;
    double boxH = 0.01;

    for (const QPointF& pt : points) {
        // 计算归一化中心点坐标
        // 使用 std::clamp 确保坐标永远在 0~1 之间，防止越界误差
        double x_center = std::clamp(pt.x() / w, 0.0, 1.0);
        double y_center = std::clamp(pt.y() / h, 0.0, 1.0);

        // 3. 写入 YOLO 格式行：<类别ID> <x_center> <y_center> <width> <height>
        // 类别 ID 默认为 0 (代表穴位)
        out << "0 "
            << QString::number(x_center, 'f', 6) << " "
            << QString::number(y_center, 'f', 6) << " "
            << boxW << " " << boxH << "\n";
    }

    file.close();
    qDebug() << u8"成功导出标签至: " << labelPath;
}

#endif // GLOBAL_H