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

    statusBar()->showMessage("Ready");
    resize(1200, 800);
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

    // 2. 动态构造输出路径：原目录/out/原文件名
    QString currentPath = imageFileList_[currentIndex_];
    QFileInfo fileInfo(currentPath);

    QDir sourceDir = fileInfo.dir();
    if (!sourceDir.exists("out")) {
        sourceDir.mkdir("out");
    }
    QString outputPath = sourceDir.absoluteFilePath("out/" + fileInfo.fileName());

    // 3.构建标签输出路径
    if (!sourceDir.exists("labels")) 
    {
        sourceDir.mkdir("labels");
    }
    QString labelPath = sourceDir.absoluteFilePath("labels/" + fileInfo.baseName() + ".txt");
    QList<QPointF> label_points = canvas_->labelPoints();
    QRectF bbox = canvas_->boundingBox();

    // 4. 执行异步擦除
    emit DisErrasureImage(canvas_->sourceImage(), canvas_->maskImage(), 
    [this, outputPath, bbox, label_points, labelPath](const QImage& result)
        {
            if (result.isNull())
                return;

            // 保存净图到 out/
            if (const_cast<QImage&>(result).save(outputPath))
            {
                // 保存标签
                saveYoloLabels(bbox, label_points, labelPath, result.size());

                qDebug() << "Saved image to:" << outputPath;
                qDebug() << "Saved label to:" << labelPath;
                statusBar()->showMessage(u8"快速保存成功！", 1500);
            }
            else 
            {
                statusBar()->showMessage(u8"保存失败！", 2000);
            }
        });

    // 5. 无需等待结果，直接切换到下一张
    showNextImage();
    canvas_->clearMask();
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

    if (!align_) align_ = std::make_shared<AlignToReference>();

    AlignToReference::Params p;
    p.maxFeatures = 3000;
    p.minInliers = 25;
    p.dilateR = 2;

    const bool ok = align_->SetReference(canvas_->sourceImage(), canvas_->maskImage(), canvas_->labelPoints(), canvas_->boundingBox(), p);
    statusBar()->showMessage(ok ? "Reference set OK" : "Reference set FAILED", 2000);
}

void MainWindow::showNextImage()
{
    if (imageFileList_.isEmpty() || currentIndex_ == -1) return;

    // 索引加1，如果到头了就回到第一张（循环播放）
    currentIndex_ = (currentIndex_ + 1) % imageFileList_.size();

    QString nextFile = imageFileList_[currentIndex_];
    canvas_->loadImage(nextFile);
    statusBar()->showMessage(QString("Image %1/%2").arg(currentIndex_ + 1).arg(imageFileList_.size()), 1000);
}

void MainWindow::showPrevImage()
{
    if (imageFileList_.isEmpty() || currentIndex_ == -1) return;

    // 索引减1，如果到头了就跳到最后一张
    currentIndex_ = (currentIndex_ - 1 + imageFileList_.size()) % imageFileList_.size();

    QString prevFile = imageFileList_[currentIndex_];
    canvas_->loadImage(prevFile);
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
    QImage maskB = align_->MakeMaskFor(imgB, &label_points, &bbox, &ok);
    if (!ok || maskB.isNull()) {
        statusBar()->showMessage("Align failed (not enough inliers).", 2000);
        return;
    }

    QImage preview = MakeOverlayPreview(imgB, maskB, QColor(255, 0, 0), 120);
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

    // 读取mask模板
    QImage maskTemplate = canvas_ ? canvas_->maskImage() : QImage();
    if (maskTemplate.isNull()) {
        batchStatus_->setText("Mask template is empty. Please paint mask first.");
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