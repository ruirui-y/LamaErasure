#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QImage>
#include <atomic>

#include "Global.h"

class InpaintCanvas;
class LamaOrt;
class QLineEdit;
class QProgressBar;
class QLabel;
class QDockWidget;
class QThread;
class QPushButton;
class AlignToReference;

class MainWindow : public QMainWindow 
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

signals:
    void DisErrasureImage(const QImage& src, const QImage& mask, ImageCallback cb);

private slots:
    void onOpen();
    void onClearMask();
    void onQuickInpaint();
    void onInpaint();
    void onSave();
    void onBatch();
    void onPickInputDir();
    void onPickOutputDir();
    void onPickLabelDir();
    void onBatchStart();
    void onBatchCancel();
    void onTestOneImage();
    void onSetAsReference();
    void onAutoMask();
    void onShowHelp();
    void showNextImage();
    void showPrevImage();

private:
    void buildUi();
    void buildBatchDock();                                                  // 批处理面板
    void showBatchDock();

private:
    InpaintCanvas* canvas_ = nullptr;
    QSharedPointer<LamaOrt> ort_ = nullptr;
    QImage lastResult_;
    std::shared_ptr<AlignToReference> align_ = nullptr;

    // Batch UI
    QDockWidget* batchDock_ = nullptr;
    QLineEdit* edInputDir_ = nullptr;
    QLineEdit* edOutputDir_ = nullptr;
    QLineEdit* edLabelDir_ = nullptr;
    QProgressBar* batchProgress_ = nullptr;
    QLabel* batchStatus_ = nullptr;

    QPushButton* btnBatchStart_ = nullptr;
    QPushButton* btnBatchCancel_ = nullptr;

    std::atomic_bool batchCancel_{ false };
    QThread* batchThread_ = nullptr;

private:
    QStringList imageFileList_;                                             // 存放当前文件夹下所有图片的路径
    int currentIndex_ = -1;                                                 // 记录当前显示的是第几张

};

#endif // MAINWINDOW_H