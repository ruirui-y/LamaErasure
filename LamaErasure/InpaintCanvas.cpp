#include "InpaintCanvas.h"
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QFileInfo>

static QImage makeMaskOverlayRGBA(const QImage& maskGray, int alpha = 120)

{
    QImage rgba(maskGray.size(), QImage::Format_ARGB32_Premultiplied);
    rgba.fill(Qt::transparent);
    QPainter p(&rgba);
    p.setCompositionMode(QPainter::CompositionMode_Source);

    // 红色半透明
    QColor c(255, 0, 0, alpha);

    p.setBrush(c);
    for (int y = 0; y < maskGray.height(); ++y) 
    {
        const uchar* row = maskGray.constScanLine(y);
        for (int x = 0; x < maskGray.width(); ++x) 
        {
            if (row[x] > 0) p.drawPoint(x, y);
        }
    }
    return rgba;
}

InpaintCanvas::InpaintCanvas(QWidget* parent)
    : QGraphicsView(parent)
{
    setRenderHint(QPainter::Antialiasing, false);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);

    initScene();
}

void InpaintCanvas::initScene() {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    itemImg_ = scene_->addPixmap(QPixmap());
    itemMask_ = scene_->addPixmap(QPixmap());
    itemMask_->setZValue(10);

    // 创建一个绿色的矢量框，放在最顶层 (ZValue更高)
    itemRect_ = scene_->addRect(QRectF(), QPen(Qt::green, 3), Qt::NoBrush);
    itemRect_->setZValue(20);
    itemRect_->hide();                                                                      // 默认隐藏
}

bool InpaintCanvas::loadImage(const QString& file) {
    QImage img(file);
    if (img.isNull()) return false;
    setImage(img);
    return true;
}

void InpaintCanvas::setImage(const QImage& img) 
{
    src_ = img.convertToFormat(QImage::Format_RGB888);

    mask_ = QImage(src_.size(), QImage::Format_Grayscale8);
    mask_.fill(0);                                                                  // 纯黑

    updatePixmap();
    updateMaskPixmap();

    scene_->setSceneRect(QRectF(QPointF(0, 0), QSizeF(src_.size())));
    fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);

    emit imageChanged();
}

void InpaintCanvas::setMask(const QImage& maskGray)
{
    if (src_.isNull()) return;

    QImage m = maskGray.convertToFormat(QImage::Format_Grayscale8);
    if (m.size() != src_.size())
        m = m.scaled(src_.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);

    mask_ = m;
    updateMaskPixmap();
    emit maskChanged();
}

void InpaintCanvas::clearMask() 
{
    if (mask_.isNull()) return;
    labelPoints_.clear();

    // 清除框
    boundingBox_ = QRectF();
    itemRect_->setRect(QRectF());
    itemRect_->hide();

    mask_.fill(0);
    updateMaskPixmap();
    emit maskChanged();
}

void InpaintCanvas::updatePixmap() 
{
    if (src_.isNull()) return;
    itemImg_->setPixmap(QPixmap::fromImage(src_));
}

void InpaintCanvas::updateMaskPixmap() 
{
    if (mask_.isNull()) return;
    QImage overlay = makeMaskOverlayRGBA(mask_, 110);
    itemMask_->setPixmap(QPixmap::fromImage(overlay));
}

QPointF InpaintCanvas::viewToScenePos(const QPoint& vp) const {
    return mapToScene(vp);
}

void InpaintCanvas::paintAt(const QPointF& sp) {
    if (src_.isNull() || mask_.isNull()) return;

    int x = (int)std::round(sp.x());
    int y = (int)std::round(sp.y());
    if (x < 0 || y < 0 || x >= mask_.width() || y >= mask_.height()) return;

    QPainter p(&mask_);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(drawing_ ? Qt::white : Qt::black);
}

void InpaintCanvas::mousePressEvent(QMouseEvent* e) {
    if (src_.isNull())
    {
        QGraphicsView::mousePressEvent(e);
        return;
    }
    
    drawing_ = true;
    QPointF sp = viewToScenePos(e->pos());

    // 按住 Shift + 鼠标左键，进入画框模式
    if ((e->modifiers() & Qt::ShiftModifier) && e->button() == Qt::LeftButton) {
        isDraggingBox_ = true;
        boxStartPos_ = sp;
        boundingBox_ = QRectF(sp, sp); // 初始化一个小点
        itemRect_->setRect(boundingBox_);
        itemRect_->show();
        e->accept();
        return;
    }

    // 左键涂点右键擦除
    QPainter p(&mask_);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (e->button() == Qt::LeftButton)
    {
        labelPoints_.append(sp);                                                                    // 标签
    } 
    else if (e->button() == Qt::RightButton)
    {
        for (int i = 0; i < labelPoints_.size(); ++i) 
        {
            QPointF diff = labelPoints_[i] - sp;
            // 计算两点间距离的平方
            double distSq = diff.x() * diff.x() + diff.y() * diff.y();

            // 如果点击位置在标记点的半径范围内 删除最近的一个
            if (distSq <= brushRadius_ * brushRadius_) 
            {
                labelPoints_.removeAt(i);
                break;
            }
        }
    }

    p.setBrush(e->button() == Qt::LeftButton ? Qt::white : Qt::black);                              // 黑底所以左键白色，右键黑色
    p.drawEllipse(QPointF(sp.x(), sp.y()), brushRadius_, brushRadius_);

    updateMaskPixmap();
    emit maskChanged();
    e->accept();
    return;
}

void InpaintCanvas::mouseMoveEvent(QMouseEvent* e)
{
    if (isDraggingBox_) 
    {
        QPointF sp = viewToScenePos(e->pos());
        // normalized() 可以保证无论往哪个方向拖，长宽都是正数
        boundingBox_ = QRectF(boxStartPos_, sp).normalized();
        itemRect_->setRect(boundingBox_);
        e->accept();
        return;
    }
    // 如果没有画框，走默认逻辑（比如你原本有没有在 move 里写点涂抹逻辑，如果没有可以直接调父类）
    QGraphicsView::mouseMoveEvent(e);
}

void InpaintCanvas::mouseReleaseEvent(QMouseEvent* e) 
{
    if (isDraggingBox_ && e->button() == Qt::LeftButton) {
        isDraggingBox_ = false;
        e->accept();
        return;
    }
    drawing_ = false;
    QGraphicsView::mouseReleaseEvent(e);
}

void InpaintCanvas::wheelEvent(QWheelEvent* e) {
    // Ctrl+滚轮缩放
    if (e->modifiers() & Qt::ControlModifier) {
        const qreal factor = (e->angleDelta().y() > 0) ? 1.1 : 1.0 / 1.1;
        zoom_ *= factor;
        scale(factor, factor);
        e->accept();
        return;
    }
    QGraphicsView::wheelEvent(e);
}

void InpaintCanvas::resizeEvent(QResizeEvent* e) {
    QGraphicsView::resizeEvent(e);
    // 不强行 fit，避免用户缩放被重置（你也可以按需加个“Fit”按钮）
}