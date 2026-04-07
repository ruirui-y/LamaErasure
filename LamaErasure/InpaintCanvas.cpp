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

void InpaintCanvas::mouseReleaseEvent(QMouseEvent* e) {
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