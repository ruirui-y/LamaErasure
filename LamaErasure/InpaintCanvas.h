#ifndef INPAINT_CANVAS_H
#define INPAINT_CANVAS_H
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QPoint>

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

    QList<QPointF> labelPoints() const { return labelPoints_; }

signals:
    void maskChanged();
    void imageChanged();

protected:
    void mousePressEvent(QMouseEvent* e) override;
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

    QImage src_;                                                        // RGB888
    QImage mask_;                                                       // Grayscale8 (0=keep, 255=hole)

    bool drawing_ = false;
    int brushRadius_ = 25;
    bool rightErase_ = true;                                            // 右键擦除
    qreal zoom_ = 1.0;
    QList<QPointF> labelPoints_;                                        // 记录穴位中心点的数组
};

#endif // INPAINT_CANVAS_H