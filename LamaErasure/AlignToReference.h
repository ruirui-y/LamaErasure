#ifndef ALIGNTOREFERENCE_H
#define ALIGNTOREFERENCE_H

#include <QImage>
#include <QList>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QVector>
#include <opencv2/core.hpp>
#include <memory>

// 多点局部模板追踪：每个独立 Mask 独立完成二维对齐
class AlignToReference
{
public:
    using Ptr = std::shared_ptr<AlignToReference>;

    // 匹配参数
    struct Params
    {
        int localTemplatePadding = 35;                                                  // 模板扩边
        int searchRadiusX = 400;                                                        // 粗搜索 X 半径
        int searchRadiusY = 220;                                                        // 粗搜索 Y 半径
        double minLocalScore = 0.60;                                                    // 最低匹配分数
        int minMaskComponentArea = 50;                                                  // 最小 Mask 面积
        int dilateR = 2;                                                                // Mask 膨胀半径
        int assistMaskRadius = 18;                                                      // A键 Mask 半径

        int refineRadiusX = 120;                                                        // 精搜索 X 半径
        int refineRadiusY = 90;                                                         // 精搜索 Y 半径
        int maxLocalDeviationX = 140;                                                   // 最大 X 偏差
        int maxLocalDeviationY = 110;                                                   // 最大 Y 偏差
        int minTrackCenterDistance = 35;                                                // Track 最小间距
    };

    // 参考 Track：单个独立 Mask 的局部追踪信息
    struct LocalTrack
    {
        int id = -1;                                                                    // Track 编号
        cv::Rect maskRect;                                                              // 参考图非零包围盒
        cv::Rect templateRect;                                                          // 扩边后模板区域
        cv::Mat maskPatch;                                                              // 二值 Mask 补丁
        cv::Mat templateGray;                                                           // 参考灰度模板
        cv::Point2f refCenter;                                                          // 模板中心（参考图坐标）
    };

    // Track 匹配结果
    struct LocalMatchResult
    {
        int id = -1;                                                                    // Track 编号
        cv::Point2f refCenter = cv::Point2f(0, 0);                                      // 参考图中心

        cv::Rect searchRect;                                                            // 最终搜索区域
        cv::Rect matchRect;                                                             // 目标图匹配位置
        cv::Point2f currentCenter = cv::Point2f(0, 0);                                  // 目标图中心
        int dx = 0;                                                                     // X 位移
        int dy = 0;                                                                     // Y 位移
        double score = 0.0;                                                             // 匹配分数
        bool ok = false;                                                                // 是否成功

        // 粗匹配结果
        int coarseDx = 0;
        int coarseDy = 0;
        double coarseScore = 0.0;
        cv::Rect coarseSearchRect;
        cv::Rect coarseMatchRect;
        cv::Point2f coarseCenter;

        QString failReason;                                                             // 失败原因
    };

public:
    AlignToReference();

    // 设置基准图 A 与 maskA，内部拆分多个独立 Mask 并为每个生成局部模板
    bool SetReference(const QImage& refRgb, const QImage& refMaskGray,
                      QList<QPointF> labelPoints, const QRectF& refRect,
                      const Params& p);

    // 返回对齐后的 finalMask（与 B 同尺寸），任一 Track 失败则整张失败
    QImage MakeMaskFor(const QImage& imgB, QList<QPointF>* ptsOutB, QRectF* rectOutB,
                       bool* bOk = nullptr,
                       QVector<LocalMatchResult>* outTracks = nullptr) const;

    // 人工辅助模式（A 键）：只预测草稿，允许部分 Track 成功
    QImage MakeAssistMaskFor(const QImage& imgB, int* outSuccess = nullptr,
                             int* outTotal = nullptr) const;

    // 从最终 Mask 重新提取 Component 中心（Q 流程唯一真相来源）
    static int ExtractMaskCenters(const QImage& maskGray, int minComponentArea,
                                  QVector<QPointF>& centers);

    bool IsReady() const { return ready_; }                                             // Reference 是否有效

    const QVector<LocalTrack>& localTracks() const { return localTracks_; }             // 参考 Track 列表

private:
    bool BuildLocalTracks();                                                            // 构建参考 Track

    // 粗匹配
    bool CoarseMatchTrack(const LocalTrack& track, const cv::Mat& grayB,
                          const cv::Size& imgSize, LocalMatchResult& out) const;

    // 根据全局位移预测位置并执行局部精匹配
    bool RefinedMatchTrack(const LocalTrack& track, const cv::Mat& grayB,
                           const cv::Size& imgSize, int globalDx, int globalDy,
                           LocalMatchResult& out) const;

    static int MedianInt(const QVector<int>& v);                                        // 计算中位数

    // 执行粗匹配并根据中位数计算整体移动趋势
    void RunTwoStageMatch(const cv::Mat& grayB, const cv::Size& imgSize,
                          QVector<LocalMatchResult>& tracks, int& globalDx, int& globalDy) const;

    // 合并移动后的 Mask
    cv::Mat ComposeFinalMask(const QVector<LocalMatchResult>& tracks,
                             const QVector<bool>& use, int H, int W) const;

    // 计算 Mask 联合包围盒
    cv::Rect MaskUnionRect(const QVector<LocalMatchResult>& tracks,
                           const QVector<bool>& use, int W, int H) const;

    // 粘贴单个 Mask 补丁
    static void PasteMaskPatch(cv::Mat& finalMask, const cv::Mat& patch,
                               int dstX, int dstY);

    static cv::Mat ToGray(const cv::Mat& bgr);                                          // 转灰度
    static cv::Mat ToGrayMaskU8(const QImage& maskGray);                                // Mask 转灰度

private:
    cv::Mat QImageToBgrMat(const QImage& img) const;                                    // 转 BGR
    QImage GrayMatToQImage(const cv::Mat& gray8) const;                                 // 灰度转 QImage

private:
    Params p_;                                                                          // 匹配参数
    bool ready_ = false;                                                                // Reference 状态

    // Reference A
    cv::Mat refGrayA_;                                                                  // 参考灰度图
    cv::Mat refMaskA_;                                                                  // 参考 Mask
    QList<QPointF> refPointsA_;                                                         // 参考标签点
    QRectF refRectA_;                                                                   // 参考训练框

    // 每个独立 Mask 的局部追踪信息
    QVector<LocalTrack> localTracks_;

    // 参考 labelPoint -> 最近 trackId 绑定
    QVector<int> pointTrackId_;

    // 目标 BBox 相对参考 Mask 联合盒的 margin
    int bbLeftMargin_   = 0;
    int bbTopMargin_    = 0;
    int bbRightMargin_  = 0;
    int bbBottomMargin_ = 0;
};

#endif // ALIGNTOREFERENCE_H