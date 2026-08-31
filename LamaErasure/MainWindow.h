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
    explicit MainWindow(QWidget* parent = nullptr);                                     // 构造
    ~MainWindow();                                                                      // 析构

signals:
    void DisErrasureImage(const QImage& src, const QImage& mask, ImageCallback cb);     // 异步擦除信号

private slots:
    void onOpen();                                                                      // 打开单张或文件夹
    void onClearMask();                                                                 // 清除当前遮罩
    void onQuickInpaint();                                                              // 擦除并直接覆盖保存
    void onInpaint();                                                                   // 仅擦除预览
    void onSave();                                                                      // 另存为新文件
    void onBatch();                                                                     // 切换批处理面板
    void onPickInputDir();                                                              // 选择输入目录
    void onPickOutputDir();                                                             // 选择输出目录
    void onPickLabelDir();                                                              // 选择标签目录
    void onBatchStart();                                                                // 开始批处理
    void onBatchCancel();                                                               // 取消批处理
    void onTestOneImage();                                                              // 测试单张对齐效果
    void onSetAsReference();                                                            // 将当前设为基准图
    void onAutoMask();                                                                  // 自动检测贴纸遮罩
    void onShowHelp();                                                                  // 显示帮助信息
    void showNextImage();                                                               // 切换到下一张
    void showPrevImage();                                                               // 切换到上一张

private:
    void buildUi();                                                                     // 构建主界面
    void buildBatchDock();                                                              // 构建批处理面板
    void showBatchDock();                                                               // 切换批处理面板显隐

private:
    InpaintCanvas* canvas_{ nullptr };                                                  // 主画布控件
    QSharedPointer<LamaOrt> ort_{ nullptr };                                            // LaMa推理引擎
    QImage lastResult_;                                                                 // 最近一次擦除结果
    std::shared_ptr<AlignToReference> align_{ nullptr };                                // 特征对齐器

    // ---- Batch UI ----
    QDockWidget* batchDock_{ nullptr };                                                 // 批处理面板
    QLineEdit* edInputDir_{ nullptr };                                                  // 输入目录路径
    QLineEdit* edOutputDir_{ nullptr };                                                 // 输出目录路径
    QLineEdit* edLabelDir_{ nullptr };                                                  // 标签目录路径
    QProgressBar* batchProgress_{ nullptr };                                            // 批处理进度条
    QLabel* batchStatus_{ nullptr };                                                    // 批处理状态文本

    QPushButton* btnBatchStart_{ nullptr };                                             // 开始按钮
    QPushButton* btnBatchCancel_{ nullptr };                                            // 取消按钮

    std::atomic_bool batchCancel_{ false };                                             // 取消标记（线程安全）
    QThread* batchThread_{ nullptr };                                                   // 批处理工作线程

private:
    QStringList imageFileList_;                                                         // 当前文件夹下所有图片路径
    int currentIndex_{ -1 };                                                            // 当前显示的图片索引

};

#endif // MAINWINDOW_H