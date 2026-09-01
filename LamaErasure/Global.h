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
#include <algorithm>


using ImageCallback = std::function<void(const QImage&)>;

// 保存 YOLO 标签
inline void saveYoloLabels(const QRectF& bbox, const QList<QPointF>& points, const QString& labelPath, const QSize& imgSize)
{
    QFile file(labelPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "无法创建标签文件: " << labelPath;
        return;
    }

    QTextStream out(&file);

    // 2. 准备归一化参数
    double w = imgSize.width();
    double h = imgSize.height();

    // 1. 归一化大框 BBox 坐标 (中心X, 中心Y, 宽, 高)
    // 使用 std::clamp 确保坐标在 0~1 之间，防止仿射变换时框的边缘超出图片边界
    double x_center = std::clamp((bbox.x() + bbox.width() / 2.0) / w, 0.0, 1.0);
    double y_center = std::clamp((bbox.y() + bbox.height() / 2.0) / h, 0.0, 1.0);
    double boxW = std::clamp(bbox.width() / w, 0.0, 1.0);
    double boxH = std::clamp(bbox.height() / h, 0.0, 1.0);

    // 2. 写入类别 ID (默认 0) 和大框数据
    QString line = QString("0 %1 %2 %3 %4")
        .arg(x_center, 0, 'f', 6)
        .arg(y_center, 0, 'f', 6)
        .arg(boxW, 0, 'f', 6)
        .arg(boxH, 0, 'f', 6);

    // 3. 归一化并追加所有关键点坐标 (X, Y, 可见度)
    for (const QPointF& pt : points) {
        double px = std::clamp(pt.x() / w, 0.0, 1.0);
        double py = std::clamp(pt.y() / h, 0.0, 1.0);

        // 格式：px py visibility (这里的 '2' 代表关键点可见且已标注)
        line += QString(" %1 %2 2").arg(px, 0, 'f', 6).arg(py, 0, 'f', 6);
    }

    // 写入文件
    out << line << "\n";
    file.close();

    qDebug() << u8"成功导出 YOLO Pose 标签至: " << labelPath;
}

#endif // GLOBAL_H