#include "MainWindow.h"
#include "ConfigReader.h"
#include "LamaOrt.h"
#include "InpaintCanvas.h"
#include "BatchWorker.h"
#include "AutoMask.h"
#include "AutoMaskDialog.h"
#include "AlignToReference.h"
#include "ThreadPool.h"
#include "WorkerThread.h"

#include <QDebug>
#include <QVector>
#include <QMessageBox>
#include <QToolBar>
#include <QAction>
#include <QSlider>
#include <QLabel>
#include <QFileDialog>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QDir>
#include <QFileInfoList>

#include <QDockWidget>
#include <QWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>

#include <QThread>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QTextStream>
#include <QRegularExpression>
#include <QPainter>
#include <algorithm>
#include <QStyle>


static void ShowImageWindow(const QString& title, const QImage& img)
{
    auto* w = new QWidget(nullptr);
    w->setAttribute(Qt::WA_DeleteOnClose, true);
    w->setWindowTitle(title);
    w->resize(1000, 700);

    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);

    auto* lab = new QLabel(w);
    lab->setAlignment(Qt::AlignCenter);
    lab->setScaledContents(true);                                                       
    lab->setPixmap(QPixmap::fromImage(img));
    lay->addWidget(lab);

    w->show();
}

static QImage MakeOverlayPreview(const QImage& src, const QImage& maskGray,
    const QColor& color = QColor(255, 0, 0),
    int alpha = 120)
{
    if (src.isNull() || maskGray.isNull()) return QImage();

    // 保证格式
    QImage base = src.convertToFormat(QImage::Format_ARGB32);
    QImage m = maskGray.convertToFormat(QImage::Format_Grayscale8);

    // 尺寸不一致就缩放到 src（一般你的 maskB 跟 imgB 一样大，但兜底一下）
    if (m.size() != base.size())
        m = m.scaled(base.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);

    QPainter p(&base);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    QColor c = color;
    c.setAlpha(alpha);

    // 逐像素叠加（mask>0 的位置画半透明色）
    for (int y = 0; y < base.height(); ++y) {
        const uchar* pm = m.constScanLine(y);
        QRgb* dst = reinterpret_cast<QRgb*>(base.scanLine(y));
        for (int x = 0; x < base.width(); ++x) {
            if (pm[x] > 127) {
                // alpha blend：dst = lerp(dst, color, alpha)
                const int a = c.alpha();
                const int inv = 255 - a;

                const int dr = qRed(dst[x]);
                const int dg = qGreen(dst[x]);
                const int db = qBlue(dst[x]);

                const int rr = (dr * inv + c.red() * a) / 255;
                const int rg = (dg * inv + c.green() * a) / 255;
                const int rb = (db * inv + c.blue() * a) / 255;

                dst[x] = qRgb(rr, rg, rb);
            }
        }
    }

    return base;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) 
{
    buildUi();
    buildBatchDock();

    auto t = ThreadPool::Instance()->GetThread();
    ort_ = t->CreateQObject<LamaOrt>();
    connect(this, &MainWindow::DisErrasureImage, ort_.get(), &LamaOrt::RunAsync);
}

MainWindow::~MainWindow() 
{
}

void MainWindow::buildUi() 
{
    // 1. 中央挂件
    QWidget* central = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // 2. 创建一个水平布局来放 按钮-画布-按钮
    QHBoxLayout* hLayout = new QHBoxLayout();
    // --- 左箭头按钮 ---
    QPushButton* btnPrev = new QPushButton("<", central);
    btnPrev->setFixedWidth(40);
    btnPrev->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding); // 高度填满
    btnPrev->setStyleSheet("QPushButton { background-color: rgba(200, 200, 200, 50); border: none; font-size: 20px; } "
        "QPushButton:hover { background-color: rgba(200, 200, 200, 100); }");

    // --- 画布 ---
    canvas_ = new InpaintCanvas(central);

    // 拖完 Tracking ROI 后在状态栏显示原图像素坐标（用户可直接判断是否框成 679x418 这种怪值）
    connect(canvas_, &InpaintCanvas::trackingRoiChanged, this,
        [this](const QRect& roi) {
            statusBar()->showMessage(
                QString("Tracking ROI: x=%1 y=%2 w=%3 h=%4")
                    .arg(roi.x()).arg(roi.y()).arg(roi.width()).arg(roi.height()), 4000);
        });

    // --- 右箭头按钮 ---
    QPushButton* btnNext = new QPushButton(">", central);
    btnNext->setFixedWidth(40);
    btnNext->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    btnNext->setStyleSheet(btnPrev->styleSheet());                      // 套用一样的样式

    // 3. 将它们装进水平布局
    hLayout->addWidget(btnPrev);
    hLayout->addWidget(canvas_, 1); // 1 表示画布占据主要空间
    hLayout->addWidget(btnNext);

    // 4. 将水平布局放入主垂直布局
    mainLayout->addLayout(hLayout);
    setCentralWidget(central);

    connect(btnPrev, &QPushButton::clicked, this, &MainWindow::showPrevImage);
    connect(btnNext, &QPushButton::clicked, this, &MainWindow::showNextImage);

    // Toolbar
    QToolBar* tb = addToolBar("Tools");
    tb->setMovable(false);

    // 添加各个 Action 并设置 ToolTip
    QAction* actOpen = tb->addAction("Open");
    actOpen->setToolTip(u8"打开单张或文件夹中的图片");

    QAction* actClear = tb->addAction("ClearMask");
    actClear->setToolTip(u8"清除当前所有的涂抹痕迹");

    QAction* actQuick = tb->addAction("QuickInpaint");
    actQuick->setToolTip(u8"擦除后保存到 out/ 和 labels/，并生成 YOLO 标签 (快捷键 Q)");
    actQuick->setShortcut(QKeySequence("Q"));                                       // 设置快捷键为 Q

    QAction* actRun = tb->addAction("Inpaint");
    actRun->setToolTip(u8"仅执行擦除预览，不保存文件");

    QAction* actSetRef = tb->addAction("SetRef");
    actSetRef->setToolTip(u8"将当前图的mask设置为基准");

    QAction* actTrackROI = tb->addAction("TrackROI");
    actTrackROI->setCheckable(true);
    actTrackROI->setToolTip(u8"在参考图上拖一个稳定区域作为 Tracking ROI（用于算水平位移，不属于Mask）");

    QAction* actTestOne = tb->addAction("TestOne");
    actTestOne->setToolTip(u8"测试基于基准图的批量擦除效果");

    QAction* actAuto = tb->addAction("AutoMask");
    actAuto->setToolTip(u8"基于算法自动识别遮挡区域");

    QAction* actSave = tb->addAction("SaveAs");
    actSave->setToolTip(u8"另存为新文件 (弹出对话框)");

    QAction* actBatch = tb->addAction("Batch");
    actBatch->setToolTip(u8"批量处理当前文件夹下的所有图片");

    QAction* actHelp = tb->addAction("Help");
    actHelp->setToolTip(u8"查看操作说明");

    tb->addSeparator();
    tb->addWidget(new QLabel("Brush:", tb));
    QSlider* sBrush = new QSlider(Qt::Horizontal, tb);
    sBrush->setRange(1, 80);
    sBrush->setValue(canvas_->brushRadius());
    sBrush->setFixedWidth(160);
    tb->addWidget(sBrush);

    connect(actOpen, &QAction::triggered, this, &MainWindow::onOpen);
    connect(actClear, &QAction::triggered, this, &MainWindow::onClearMask);
    connect(actQuick, &QAction::triggered, this, &MainWindow::onQuickInpaint); // 连接新功能
    connect(actSetRef, &QAction::triggered, this, &MainWindow::onSetAsReference);

    QAction* actAssistMask = tb->addAction("AssistMask");
    actAssistMask->setShortcut(QKeySequence("A"));
    actAssistMask->setToolTip(u8"基于已设参考图，对当前图预测 Mask 草稿（按 A），随后可直接在画布上用画笔修补");
    connect(actAssistMask, &QAction::triggered, this, &MainWindow::onAssistMask);
    connect(actTrackROI, &QAction::triggered, this, [this](bool checked) {
        canvas_->setTrackingMode(checked);
        statusBar()->showMessage(checked
            ? u8"TrackROI 模式：在参考图上拖一个矩形"
            : u8"TrackROI 模式关闭", 2000);
        });
    connect(actTestOne, &QAction::triggered, this, &MainWindow::onTestOneImage);
    connect(actAuto, &QAction::triggered, this, &MainWindow::onAutoMask);
    connect(actRun, &QAction::triggered, this, &MainWindow::onInpaint);
    connect(actSave, &QAction::triggered, this, &MainWindow::onSave);
    connect(actBatch, &QAction::triggered, this, &MainWindow::onBatch);
    connect(actHelp, &QAction::triggered, this, &MainWindow::onShowHelp);

    connect(sBrush, &QSlider::valueChanged, this, [this](int v) {
        canvas_->setBrushRadius(v);
        statusBar()->showMessage(QString("Brush=%1").arg(v), 800);
        });

    // 永久状态信息（不使用 timeout，绝不会被"快速保存成功/AssistMask/Brush"等临时消息覆盖）
    imageInfoLabel_ = new QLabel(this);
    imageInfoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addPermanentWidget(imageInfoLabel_);
    updateImageInfo();

    statusBar()->showMessage("Ready");
    resize(1200, 800);
}

// 状态栏永久信息：Current: 13/268 | Ref: 12 | xxx_0013.jpg
void MainWindow::updateImageInfo()
{
    if (!imageInfoLabel_) return;

    if (imageFileList_.isEmpty() || currentIndex_ < 0 || currentIndex_ >= imageFileList_.size()) {
        imageInfoLabel_->setText("Current: -/- | Ref: none | -");
        return;
    }

    const QString refText = (lastReferenceIndex_ >= 0)
        ? QString::number(lastReferenceIndex_ + 1)
        : QStringLiteral("none");

    imageInfoLabel_->setText(QString("Current: %1/%2 | Ref: %3 | %4")
        .arg(currentIndex_ + 1)
        .arg(imageFileList_.size())
        .arg(refText)
        .arg(QFileInfo(imageFileList_[currentIndex_]).fileName()));
}

// 对齐参数唯一来源：onSetAsReference() 与 onQuickInpaint() 的滚动 Reference 共用，
// 避免两处硬编码将来数值不一致。
AlignToReference::Params MainWindow::makeAlignParams() const
{
    AlignToReference::Params p;
    p.localTemplatePadding = 35;
    p.searchRadiusX = 400;
    p.searchRadiusY = 220;
    p.minLocalScore = 0.60;
    p.minMaskComponentArea = 50;
    p.dilateR = 2;
    p.assistMaskRadius = 18;        // A键自动圆半径，与 dilateR 无关
    // 第二阶段精匹配 + 防串点阈值（按实测日志再调）
    p.refineRadiusX = 120;
    p.refineRadiusY = 90;
    p.maxLocalDeviationX = 140;
    p.maxLocalDeviationY = 110;
    p.minTrackCenterDistance = 35;
    return p;
}

void MainWindow::onOpen()
{
    // 使用配置中记录的最后路径作为打开对话框的默认起点
    QString lastDir = ConfigReader::Instance()->paths().lastOpenDir;

    QString f = QFileDialog::getOpenFileName(this, "Open", lastDir, "Images (*.png *.jpg *.bmp)");
    if (f.isEmpty()) return;

    // 1. 获取文件夹信息
    QFileInfo fileInfo(f);
    QString currentDir = fileInfo.absolutePath();
    ConfigReader::Instance()->setLastOpenDir(currentDir);

    // 2. 找出文件夹里所有的图片，并按名称排序
    QDir dir = fileInfo.dir();
    QStringList filters;
    filters << "*.png" << "*.jpg" << "*.jpeg" << "*.bmp";
    imageFileList_ = dir.entryList(filters, QDir::Files, QDir::Name);

    // 3. 把文件名补全为完整路径，并找到当前这张图的索引
    for (int i = 0; i < imageFileList_.size(); ++i) {
        imageFileList_[i] = dir.absoluteFilePath(imageFileList_[i]);
        if (imageFileList_[i] == f) {
            currentIndex_ = i;
        }
    }

    // 4. 加载图片
    canvas_->loadImage(f);
    updateImageInfo();
    statusBar()->showMessage(QString("Image %1/%2: %3").arg(currentIndex_ + 1).arg(imageFileList_.size()).arg(f), 2000);
}

void MainWindow::onClearMask() {
    canvas_->clearMask();
}

void MainWindow::onQuickInpaint()
{
    // 1. 安全检查
    if (!ort_ || canvas_->sourceImage().isNull()) return;
    if (currentIndex_ < 0 || currentIndex_ >= imageFileList_.size()) {
        statusBar()->showMessage(u8"当前没有打开的文件路径，请先Open", 2000);
        return;
    }

    // 2. 先对当前这一张做独立快照。
    //    后面 showNextImage() 会换掉 Canvas，异步 callback 里绝不能再读 canvas_ / currentIndex_。
    const QImage currentImage = canvas_->sourceImage();
    const QImage finalMask = canvas_->maskImage();       // 最终真相 = 人工确认后的 Mask，不读 canvas_->labelPoints()
    if (finalMask.isNull()) {
        statusBar()->showMessage("Mask is empty.", 1500);
        return;
    }

    // 3. 从最终 Mask 重新提取 Component 中心（过滤小噪点）
    QVector<QPointF> centers;
    const int cnt = AlignToReference::ExtractMaskCenters(finalMask, 50, centers);
    qDebug().noquote() << QString("FinalMask component count=%1").arg(cnt);
    if (cnt != 7) {
        // 数量不对必须停下，绝不自动下一张，也不生成错标签
        statusBar()->showMessage(QString("Expected 7 masks, got %1. Please fix mask.").arg(cnt), 3500);
        return;
    }

    // 4. 固定排序：center.y 升序，相同则 center.x 升序（与 Reference 同一规则）
    std::sort(centers.begin(), centers.end(),
        [](const QPointF& a, const QPointF& b) {
            if (a.y() != b.y()) return a.y() < b.y();
            return a.x() < b.x();
        });

    QList<QPointF> label_points;
    for (int i = 0; i < centers.size(); ++i) {
        label_points.append(centers[i]);
        qDebug().noquote() << QString("SortedPoint[%1]=(%2,%3)")
            .arg(i).arg(QString::number(centers[i].x(), 'f', 1)).arg(QString::number(centers[i].y(), 'f', 1));
    }
    qDebug() << "QuickInpaint label validation OK";

    // 5. 滚动 Reference：当前这张一旦人工确认通过，立即成为下一张 A 键使用的 Reference。
    //    传入的是本张的原图 + 最终人工 Mask + 重新提取排序后的 label_points，
    //    绝不传 canvas_->labelPoints()（最终标签的唯一真相已是 finalMask -> ExtractMaskCenters -> sorted）。
    if (!align_) align_ = std::make_shared<AlignToReference>();
    const bool refOk = align_->SetReference(currentImage, finalMask,
                                           label_points, QRectF(),
                                           makeAlignParams());
    if (!refOk) {
        // Reference 更新失败必须停止整个 Q 流程：不擦除、不保存、不切下一张
        qDebug() << "QuickInpaint BLOCKED: failed to update rolling reference";
        statusBar()->showMessage("Failed to update rolling reference.", 3500);
        return;
    }
    lastReferenceIndex_ = currentIndex_;
    updateImageInfo();
    qDebug().noquote() << QString("Rolling reference updated -> index=%1 (%2)")
        .arg(lastReferenceIndex_ + 1)
        .arg(QFileInfo(imageFileList_[currentIndex_]).fileName());

    // 6. 占位 BoundingBox：由 7 个最终点 min/max + padding 生成（禁止人工框）。
    //    真实 BBox 由 tools/generate_bbox_from_keypoints.py 后处理重新计算。
    const double pad = 20.0;
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for (const QPointF& p : centers) {
        minX = qMin(minX, p.x()); minY = qMin(minY, p.y());
        maxX = qMax(maxX, p.x()); maxY = qMax(maxY, p.y());
    }
    const QRectF placeholderBBox(minX - pad, minY - pad,
                                 (maxX - minX) + 2 * pad, (maxY - minY) + 2 * pad);

    // 7. 动态构造输出路径：原目录/out/原文件名（此时 currentIndex_ 仍指向本张）
    QString currentPath = imageFileList_[currentIndex_];
    QFileInfo fileInfo(currentPath);
    QDir sourceDir = fileInfo.dir();
    if (!sourceDir.exists("out")) sourceDir.mkdir("out");
    QString outputPath = sourceDir.absoluteFilePath("out/" + fileInfo.fileName());
    if (!sourceDir.exists("labels")) sourceDir.mkdir("labels");
    QString labelPath = sourceDir.absoluteFilePath("labels/" + fileInfo.baseName() + ".txt");

    // 8. 执行异步擦除：入参与 callback 捕获的全部是本张的独立快照
    //    （currentImage / finalMask / outputPath / labelPath / label_points / placeholderBBox）
    emit DisErrasureImage(currentImage, finalMask,
    [this, outputPath, placeholderBBox, label_points, labelPath](const QImage& result)
        {
            if (result.isNull())
                return;

            if (const_cast<QImage&>(result).save(outputPath)) {
                saveYoloLabels(placeholderBBox, label_points, labelPath, result.size());
                qDebug() << "Saved image to:" << outputPath;
                qDebug() << "Saved label to:" << labelPath;
                statusBar()->showMessage(u8"快速保存成功！", 1500);
            }
            else {
                statusBar()->showMessage(u8"保存失败！", 2000);
            }
        });

    // 9. 成功后切下一张（loadImage 会自动清空 Mask；内部会刷新 Current/Ref 永久显示）
    showNextImage();
}

void MainWindow::onAutoMask()
{
    if (canvas_->sourceImage().isNull()) return;

    // 1) 弹窗收集参数（可选=有默认值，不填直接 OK 也行）
    AutoMaskDialog dlg(this);

    if (dlg.exec() != QDialog::Accepted) {
        statusBar()->showMessage("AutoMask canceled.", 1200);
        return;
    }

    const QImage src = canvas_->sourceImage();
    const auto p = dlg.params();

    // 2) 计算 mask
    QImage m = AutoMask::Instance()->AutoMaskFromStickers(src, p);
    if (m.isNull()) {
        statusBar()->showMessage("AutoMask failed.", 1500);
        return;
    }

    // 3) 显示到画布：先 setImage（会清空mask），再 setMask
    canvas_->setImage(src);
    canvas_->setMask(m);

    statusBar()->showMessage("AutoMask done.", 1200);
}

void MainWindow::onShowHelp()
{
    QString readmeText = QString::fromLocal8Bit("关于具体的操作说明请阅读LamaErasure.md");
    QMessageBox::information(this, u8"使用说明", readmeText);
}

void MainWindow::onSetAsReference()
{
    if (!canvas_ || canvas_->sourceImage().isNull()) {
        statusBar()->showMessage("No image on canvas.", 1500);
        return;
    }

    // 新人工辅助流程：只需参考图 + 多个独立 Mask，不再要求人工画 BoundingBox。
    // BoundingBox 由 tools/generate_bbox_from_keypoints.py 后处理从 7 个关键点生成。
    if (!align_) align_ = std::make_shared<AlignToReference>();

    const bool ok = align_->SetReference(canvas_->sourceImage(), canvas_->maskImage(),
                                         canvas_->labelPoints(), canvas_->boundingBox(),
                                         makeAlignParams());
    if (ok) {
        // Reference 入口 1/2：用户第一次手工 SetRef
        lastReferenceIndex_ = currentIndex_;
        updateImageInfo();
    }
    statusBar()->showMessage(ok ? "Reference set OK" : "Reference set FAILED", 2000);
}

void MainWindow::onAssistMask()
{
    if (!align_ || !align_->IsReady()) {
        statusBar()->showMessage("Reference not set. Click SetRef first.", 2000);
        return;
    }
    if (canvas_->sourceImage().isNull()) {
        statusBar()->showMessage("No image on canvas.", 1500);
        return;
    }

    int success = 0, total = 0;
    QImage predicted = align_->MakeAssistMaskFor(canvas_->sourceImage(), &success, &total);
    if (predicted.isNull()) {
        statusBar()->showMessage(QString("AssistMask: predicted 0/%1, please fix manually").arg(total), 3500);
        return;
    }

    // 直接把预测 Mask 草稿显示到当前画布，用户可继续用画笔修补
    canvas_->setMask(predicted);

    if (success < total)
        statusBar()->showMessage(QString("AssistMask: predicted %1/%2, please fix manually").arg(success).arg(total), 3000);
    else
        statusBar()->showMessage(QString("AssistMask: predicted %1/%2").arg(success).arg(total), 2500);
}

void MainWindow::showNextImage()
{
    if (imageFileList_.isEmpty() || currentIndex_ == -1) return;

    // 索引加1，如果到头了就回到第一张（循环播放）
    currentIndex_ = (currentIndex_ + 1) % imageFileList_.size();

    QString nextFile = imageFileList_[currentIndex_];
    canvas_->loadImage(nextFile);
    updateImageInfo();
    statusBar()->showMessage(QString("Image %1/%2").arg(currentIndex_ + 1).arg(imageFileList_.size()), 1000);
}

void MainWindow::showPrevImage()
{
    if (imageFileList_.isEmpty() || currentIndex_ == -1) return;

    // 索引减1，如果到头了就跳到最后一张
    currentIndex_ = (currentIndex_ - 1 + imageFileList_.size()) % imageFileList_.size();

    QString prevFile = imageFileList_[currentIndex_];
    canvas_->loadImage(prevFile);
    updateImageInfo();
    statusBar()->showMessage(QString("Image %1/%2").arg(currentIndex_ + 1).arg(imageFileList_.size()), 1000);
}

void MainWindow::onTestOneImage()
{
    if (!ort_)
    {
        statusBar()->showMessage("ort_ is null", 1500);
        return;
    }

    if (!align_ || !align_->IsReady()) {
        statusBar()->showMessage("Reference not set. Click SetRef first.", 2000);
        return;
    }

    const QString file = QFileDialog::getOpenFileName(
        this, "Select one image (B)", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp)"
    );
    if (file.isEmpty()) return;

    QImage imgB(file);
    if (imgB.isNull()) {
        statusBar()->showMessage("Failed to load image.", 1500);
        return;
    }

    bool ok = false;
    QList<QPointF> label_points;
    QRectF bbox;
    QVector<AlignToReference::LocalMatchResult> tracks;
    QImage maskB = align_->MakeMaskFor(imgB, &label_points, &bbox, &ok, &tracks);
    if (!ok || maskB.isNull()) {
        // 失败：统计成功/失败数，指出第一个失败 track 及其失败原因
        int failed = 0, total = tracks.size();
        int firstFailId = -1; double firstFailScore = 0; QString firstFailReason;
        for (const auto& r : tracks) {
            if (!r.ok) {
                ++failed;
                if (firstFailId < 0) { firstFailId = r.id; firstFailScore = r.score; firstFailReason = r.failReason; }
            }
        }
        statusBar()->showMessage(QString("Local match failed: %1/%2, track=%3 reason=%4 score=%5")
            .arg(total - failed).arg(total).arg(firstFailId).arg(firstFailReason)
            .arg(QString::number(firstFailScore, 'f', 2)), 3500);
        return;
    }

    int failed = 0;
    const int total = tracks.size();
    for (const auto& r : tracks) if (!r.ok) ++failed;

    statusBar()->showMessage(QString("Local match OK: %1/%2")
        .arg(total - failed).arg(total), 3000);

    QImage preview = MakeOverlayPreview(imgB, maskB, QColor(255, 0, 0), 120);

    // 调试显示：每个 Track 在目标图的匹配位置画绿色小框 + 编号
    {
        QPainter p(&preview);
        p.setPen(QPen(Qt::green, 2));
        p.setFont(QFont("monospace", 11));
        for (const auto& r : tracks) {
            if (!r.ok) continue;
            const QRect box(r.matchRect.x, r.matchRect.y, r.matchRect.width, r.matchRect.height);
            p.drawRect(box);
            // 编号写在框左上角内侧
            p.drawText(QRect(box.x() + 2, box.y() + 2, 24, 14), Qt::AlignLeft, QString::number(r.id));
        }
    }

    ShowImageWindow("B + mask overlay", preview);

    // 跑 inpaint
    statusBar()->showMessage(u8"TestOne 正在后台处理...");
    emit DisErrasureImage(imgB, maskB, [this](const QImage& result) {
        if (!result.isNull()) {
            ShowImageWindow("inpaint result", result);
            statusBar()->showMessage("TestOne done.", 1200);
        }
        else {
            statusBar()->showMessage("Inpaint failed.", 2000);
        }
        });
}

void MainWindow::onInpaint() 
{
    if (!ort_ || canvas_->sourceImage().isNull()) return;

    QImage src = canvas_->sourceImage();
    QImage mask = canvas_->maskImage();

    statusBar()->showMessage(u8"正在执行 Inpaint...");

    emit DisErrasureImage(src, mask, [this](const QImage& result) 
        {
            if (!result.isNull()) {
                canvas_->setImage(result);
                lastResult_ = result;
                statusBar()->showMessage("Inpaint done.", 1200);
            }
            else {
                statusBar()->showMessage("Inpaint failed.", 2000);
            }
        });
}

void MainWindow::onSave() 
{
    QImage img = canvas_->sourceImage();
    if (img.isNull()) return;

    QString f = QFileDialog::getSaveFileName(this, "Save", "", "PNG (*.png);;JPG (*.jpg)");
    if (f.isEmpty()) return;

    if (!img.save(f)) {
        QMessageBox::warning(this, "Save", "Failed to save.");
        return;
    }
    statusBar()->showMessage("Saved: " + f, 1200);
}

void MainWindow::buildBatchDock()
{
    batchDock_ = new QDockWidget("Batch", this);
    batchDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    batchDock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, batchDock_);
    batchDock_->hide();                                                                             // 默认隐藏，按 Batch 才显示

    QWidget* panel = new QWidget(batchDock_);
    batchDock_->setWidget(panel);

    auto* root = new QVBoxLayout(panel);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    // 路径区
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(8);

    // Input
    {
        auto* row = new QHBoxLayout();
        edInputDir_ = new QLineEdit(panel);
        edInputDir_->setPlaceholderText("Select input folder...");
        auto* btn = new QPushButton("...", panel);
        btn->setFixedWidth(32);
        row->addWidget(edInputDir_);
        row->addWidget(btn);
        form->addRow("Input Dir:", row);
        connect(btn, &QPushButton::clicked, this, &MainWindow::onPickInputDir);
    }

    // Output
    {
        auto* row = new QHBoxLayout();
        edOutputDir_ = new QLineEdit(panel);
        edOutputDir_->setPlaceholderText("Select output folder...");
        auto* btn = new QPushButton("...", panel);
        btn->setFixedWidth(32);
        row->addWidget(edOutputDir_);
        row->addWidget(btn);
        form->addRow("Output Dir:", row);
        connect(btn, &QPushButton::clicked, this, &MainWindow::onPickOutputDir);
    }

    // Label 目录选择
    {
        auto* row = new QHBoxLayout();
        edLabelDir_ = new QLineEdit(panel);
        edLabelDir_->setPlaceholderText("Select label folder...");

        auto* btn = new QPushButton("...", panel);
        btn->setFixedWidth(32);

        row->addWidget(edLabelDir_);
        row->addWidget(btn);

        form->addRow("Label Dir:", row);

        connect(btn, &QPushButton::clicked, this, &MainWindow::onPickLabelDir);
    }

    root->addLayout(form);

    // 进度区
    batchProgress_ = new QProgressBar(panel);
    batchProgress_->setRange(0, 100);
    batchProgress_->setValue(0);
    batchProgress_->setTextVisible(true);
    root->addWidget(batchProgress_);

    batchStatus_ = new QLabel("Idle", panel);
    batchStatus_->setWordWrap(true);
    root->addWidget(batchStatus_);

    // 按钮
    {
        auto* btnRow = new QHBoxLayout();
        btnRow->addStretch();

        btnBatchStart_ = new QPushButton("Start", panel);
        btnBatchCancel_ = new QPushButton("Cancel", panel);

        btnBatchStart_->setEnabled(true);
        btnBatchCancel_->setEnabled(false);

        btnRow->addWidget(btnBatchStart_);
        btnRow->addWidget(btnBatchCancel_);
        root->addLayout(btnRow);

        connect(btnBatchStart_, &QPushButton::clicked, this, &MainWindow::onBatchStart);
        connect(btnBatchCancel_, &QPushButton::clicked, this, &MainWindow::onBatchCancel);
    }

    root->addStretch();
}

void MainWindow::onBatch()
{
    showBatchDock();
}

void MainWindow::showBatchDock()
{
    if (!batchDock_) return;
    const bool vis = batchDock_->isVisible();
    batchDock_->setVisible(!vis);
    if (!vis) batchDock_->raise();
}

void MainWindow::onPickInputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "Select Input Folder");
    if (!dir.isEmpty()) {
        edInputDir_->setText(dir);
        batchStatus_->setText("Input folder set.");
    }
}

void MainWindow::onPickOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "Select Output Folder");
    if (!dir.isEmpty()) {
        edOutputDir_->setText(dir);
        batchStatus_->setText("Output folder set.");
    }
}

void MainWindow::onPickLabelDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "Select Label Folder");
    if (!dir.isEmpty()) 
    {
        edLabelDir_->setText(dir);
        if (batchStatus_) batchStatus_->setText("Label folder set.");
    }
}

void MainWindow::onBatchStart()
{
    if (!ort_) 
    {
        statusBar()->showMessage("ORT not ready.", 1500);
        return;
    }
    if (!batchDock_ || !edInputDir_ || !edOutputDir_) return;

    const QString inDir = edInputDir_->text().trimmed();
    const QString outDir = edOutputDir_->text().trimmed();
    const QString labelDir = edLabelDir_->text().trimmed();

    if (inDir.isEmpty() || !QDir(inDir).exists()) {
        batchStatus_->setText("Input dir invalid.");
        return;
    }
    if (outDir.isEmpty()) {
        batchStatus_->setText("Output dir empty.");
        return;
    }
    if (labelDir.isEmpty()) {
        batchStatus_->setText("Label dir empty.");
        return;
    }

    // 前置条件：Reference 已成功设置（BatchWorker 实际使用 align_ 中保存的 Reference）
    if (!align_ || !align_->IsReady()) {
        batchStatus_->setText("Reference not set. Paint mask and click Set Reference first.");
        return;
    }

    // UI 状态
    batchCancel_.store(false);
    btnBatchStart_->setEnabled(false);
    btnBatchCancel_->setEnabled(true);
    batchProgress_->setRange(0, 100);
    batchProgress_->setValue(0);
    batchStatus_->setText("Batch running...");

    // 清理旧线程（保险）
    if (batchThread_) {
        batchThread_->quit();
        batchThread_->wait();
        batchThread_->deleteLater();
        batchThread_ = nullptr;
    }

    // 创建线程 + worker
    batchThread_ = new QThread(this);

    auto* worker = new BatchWorker(ort_, align_, inDir, outDir, labelDir, &batchCancel_);
    worker->moveToThread(batchThread_);

    connect(batchThread_, &QThread::started, worker, &BatchWorker::run);

    connect(worker, &BatchWorker::progress, this, [this](int done, int total, const QString& file) {
        // 同步进度条
        if (total > 0) {
            batchProgress_->setRange(0, total);
            batchProgress_->setValue(done);
            batchProgress_->setFormat(QString("%1 / %2").arg(done).arg(total));
        }
        batchStatus_->setText(QString("Processing: %1").arg(file));
        }, Qt::QueuedConnection);

    connect(worker, &BatchWorker::finished, this, [this, worker](bool canceled, const QString& msg) {
        btnBatchStart_->setEnabled(true);
        btnBatchCancel_->setEnabled(false);

        batchStatus_->setText(msg);
        statusBar()->showMessage(msg, 2000);

        // 收尾
        worker->deleteLater();
        if (batchThread_) {
            batchThread_->quit();
            batchThread_->wait();
            batchThread_->deleteLater();
            batchThread_ = nullptr;
        }
        }, Qt::QueuedConnection);

    batchThread_->start();
}

void MainWindow::onBatchCancel()
{
    batchCancel_.store(true);
    batchStatus_->setText("Cancel requested...");
    statusBar()->showMessage("Cancel requested...", 1200);
}