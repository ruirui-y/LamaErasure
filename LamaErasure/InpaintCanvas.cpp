#include "InpaintCanvas.h"
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPen>
#include <QLineF>
#include <QFileInfo>
#include <QDebug>

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

    // Tracking ROI 显示框（黄色细线，独立于 BoundingBox）
    itemTrackRoi_ = scene_->addRect(QRectF(), QPen(Qt::yellow, 1), Qt::NoBrush);
    itemTrackRoi_->setZValue(19);
    itemTrackRoi_->hide();                                                                  // 默认隐藏
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

    // 换图后 Tracking ROI 失效，一并清除
    trackingRoiImage_ = QRect();
    isDraggingTrack_ = false;
    itemTrackRoi_->setRect(QRectF());
    itemTrackRoi_->hide();

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

void InpaintCanvas::clearTrackingRoi()
{
    trackingRoiImage_ = QRect();
    isDraggingTrack_ = false;
    if (itemTrackRoi_) {
        itemTrackRoi_->setRect(QRectF());
        itemTrackRoi_->hide();
    }
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

void InpaintCanvas::applyBrush(const QPointF& a, const QPointF& b, bool erase)
{
    if (src_.isNull() || mask_.isNull()) return;

    QPainter p(&mask_);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor c = erase ? Qt::black : Qt::white;

    QPen pen(c, brushRadius_ * 2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    p.setPen(pen);
    p.setBrush(c);

    // 连续笔触
    p.drawLine(QLineF(a, b));

    // 保证单击或移动很慢时也能形成圆形笔触
    p.drawEllipse(b, brushRadius_, brushRadius_);
}

void InpaintCanvas::mousePressEvent(QMouseEvent* e) {
    if (src_.isNull())
    {
        QGraphicsView::mousePressEvent(e);
        return;
    }
    
    drawing_ = true;
    QPointF sp = viewToScenePos(e->pos());

    // TrackROI 模式：左键拖一个矩形作为 Tracking ROI（不参与 Mask/标签）
    // 坐标转换：viewport widget 坐标 -> mapToScene -> 原图像素坐标（scene==image）
    if (trackingMode_ && e->button() == Qt::LeftButton) {
        const QPoint wp = e->pos();                                     // viewport(widget) 坐标
        const QPoint ip = viewToScenePos(wp).toPoint();                 // 原图像素坐标（唯一真实数据）
        trackStartWidget_ = wp;
        trackStartPos_ = ip;
        isDraggingTrack_ = true;
        trackingRoiImage_ = QRect(ip, ip);
        itemTrackRoi_->setRect(QRectF(trackingRoiImage_));
        itemTrackRoi_->show();
        qDebug().noquote() << QString("TrackROI widgetStart=(%1,%2) imageStart=(%3,%4)")
            .arg(wp.x()).arg(wp.y()).arg(ip.x()).arg(ip.y());
        e->accept();
        return;
    }

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

    // 画笔：左键涂白，右键擦黑（连续拖动画笔，不再记录/删除 labelPoints）
    const bool erase = (e->button() == Qt::RightButton);
    applyBrush(sp, sp, erase);
    lastPaintPos_ = sp;
    updateMaskPixmap();
    emit maskChanged();
    e->accept();
    return;
}

void InpaintCanvas::mouseMoveEvent(QMouseEvent* e)
{
    if (isDraggingTrack_)
    {
        const QPoint ip = viewToScenePos(e->pos()).toPoint();           // 原图像素坐标
        trackingRoiImage_ = QRect(trackStartPos_, ip).normalized();
        itemTrackRoi_->setRect(QRectF(trackingRoiImage_));
        e->accept();
        return;
    }

    if (isDraggingBox_) 
    {
        QPointF sp = viewToScenePos(e->pos());
        // normalized() 可以保证无论往哪个方向拖，长宽都是正数
        boundingBox_ = QRectF(boxStartPos_, sp).normalized();
        itemRect_->setRect(boundingBox_);
        e->accept();
        return;
    }

    // 连续画笔：左键拖动涂白，右键拖动擦黑（Photoshop 式体验）
    if (e->buttons() & Qt::LeftButton) {
        QPointF sp = viewToScenePos(e->pos());
        applyBrush(lastPaintPos_, sp, false);
        lastPaintPos_ = sp;
        updateMaskPixmap();
        emit maskChanged();
        e->accept();
        return;
    }
    if (e->buttons() & Qt::RightButton) {
        QPointF sp = viewToScenePos(e->pos());
        applyBrush(lastPaintPos_, sp, true);
        lastPaintPos_ = sp;
        updateMaskPixmap();
        emit maskChanged();
        e->accept();
        return;
    }
    // 否则走默认逻辑
    QGraphicsView::mouseMoveEvent(e);
}

void InpaintCanvas::mouseReleaseEvent(QMouseEvent* e) 
{
    if (isDraggingTrack_ && e->button() == Qt::LeftButton) {
        isDraggingTrack_ = false;
        // 拖完打印完整坐标日志（widget 侧 + image 侧），直接验证"鼠标框哪里 -> 原图坐标就是哪里"
        const QPoint wpEnd = e->pos();
        const QPoint ipEnd = viewToScenePos(wpEnd).toPoint();
        qDebug().noquote() << QString("TrackROI widgetEnd=(%1,%2) imageEnd=(%3,%4)")
            .arg(wpEnd.x()).arg(wpEnd.y()).arg(ipEnd.x()).arg(ipEnd.y());
        qDebug().noquote() << QString("TrackROI IMAGE rect=(%1,%2,%3,%4)")
            .arg(trackingRoiImage_.x()).arg(trackingRoiImage_.y())
            .arg(trackingRoiImage_.width()).arg(trackingRoiImage_.height());
        emit trackingRoiChanged(trackingRoiImage_);
        e->accept();
        return;
    }

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