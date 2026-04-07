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

signals:
    void maskChanged();
    void imageChanged();

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

    QGraphicsScene* scene_ = nullptr;
    QGraphicsPixmapItem* itemImg_ = nullptr;
    QGraphicsPixmapItem* itemMask_ = nullptr;
    QGraphicsRectItem* itemRect_ = nullptr;                             // 用于在画布上显示绿色的矢量框

    QImage src_;                                                        // RGB888
    QImage mask_;                                                       // Grayscale8 (0=keep, 255=hole)

    bool drawing_ = false;
    int brushRadius_ = 25;
    bool rightErase_ = true;                                            // 右键擦除
    qreal zoom_ = 1.0;
    QList<QPointF> labelPoints_;                                        // 记录穴位中心点的数组

    // 记录框的数据和画框状态
    QRectF boundingBox_;                                                // 保存真正的框坐标
    bool isDraggingBox_ = false;                                        // 是否正在画框
    QPointF boxStartPos_;                                               // 画框的起点
};

#endif // INPAINT_CANVAS_H