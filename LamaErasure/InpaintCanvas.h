#ifndef INPAINT_CANVAS_H
#define INPAINT_CANVAS_H
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QPoint>

/*
* 画布面板
*/

class InpaintCanvas : public QGraphicsView 
{
    Q_OBJECT
public:
    explicit InpaintCanvas(QWidget* parent = nullptr);

    bool loadImage(const QString& file);
    void setImage(const QImage& img);                                   // 设置当前显示图（并重置mask）
    void setMask(const QImage& maskGray);
    void clearMask();

    const QImage& sourceImage() const { return src_; }
    const QImage& maskImage()   const { return mask_; }                 // Grayscale8, 0/255

    void setBrushRadius(int r) { brushRadius_ = qMax(1, r); }
    int  brushRadius() const { return brushRadius_; }

    QList<QPointF> labelPoints() const { return labelPoints_; }         // 画的点的坐标
    QRectF boundingBox() const { return boundingBox_; }                 // 画的框的坐标

    // ---- Tracking ROI：稳定区域，用于对齐位移计算（坐标为原图像素）----
    // 铁律：trackingRoiImage_ 是唯一真实数据，存原图像素坐标；
    // 鼠标坐标须经 viewToScenePos() 转成原图坐标后存储（scene==image）。
    void setTrackingMode(bool on) { trackingMode_ = on; }               // 切换到"画 TrackROI"模式
    bool trackingMode() const { return trackingMode_; }
    QRect trackingRoi() const { return trackingRoiImage_; }             // 用户框选的 Tracking ROI（原图像素坐标）
    void clearTrackingRoi();                                            // 清除 Tracking ROI

signals:
    void maskChanged();
    void imageChanged();
    void trackingRoiChanged(const QRect& roi);                          // 拖完 ROI 后发出（供状态栏显示原图坐标）

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;                       // 画框
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void initScene();
    void updatePixmap();
    void updateMaskPixmap();
    QPointF viewToScenePos(const QPoint& vp) const;
    void paintAt(const QPointF& sp);
    void applyBrush(const QPointF& a, const QPointF& b, bool erase);    // 连续画笔：a->b 画/擦

    QGraphicsScene* scene_ = nullptr;
    QGraphicsPixmapItem* itemImg_ = nullptr;
    QGraphicsPixmapItem* itemMask_ = nullptr;
    QGraphicsRectItem* itemRect_ = nullptr;                             // 用于在画布上显示绿色的矢量框
    QGraphicsRectItem* itemTrackRoi_ = nullptr;                         // Tracking ROI 显示框（黄色细线）

    QImage src_;                                                        // RGB888
    QImage mask_;                                                       // Grayscale8 (0=keep, 255=hole)

    bool drawing_ = false;
    QPointF lastPaintPos_;
    int brushRadius_ = 13;
    bool rightErase_ = true;                                            // 右键擦除
    qreal zoom_ = 1.0;
    QList<QPointF> labelPoints_;                                        // 记录穴位中心点的数组

    // 记录框的数据和画框状态
    QRectF boundingBox_;                                                // 保存真正的框坐标
    bool isDraggingBox_ = false;                                        // 是否正在画框
    QPointF boxStartPos_;                                               // 画框的起点

    // Tracking ROI 状态（全部为【原图像素坐标】，整数，无浮点）
    QRect trackingRoiImage_;                                            // 唯一真实数据：用户框选的 Tracking ROI
    bool trackingMode_ = false;                                         // 当前是否处于画 TrackROI 模式
    bool isDraggingTrack_ = false;                                      // 是否正在画 Tracking ROI
    QPoint trackStartPos_;                                              // 画 Tracking ROI 的起点（原图坐标）
    QPoint trackStartWidget_;                                           // 拖拽起点的 viewport 坐标（仅用于调试日志）
};

#endif // INPAINT_CANVAS_H