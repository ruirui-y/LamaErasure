#ifndef ALIGNTOREFERENCE_H
#define ALIGNTOREFERENCE_H

#include <QImage>
#include <opencv2/opencv.hpp>
#include <memory>

class AlignToReference
{
public:
    using Ptr = std::shared_ptr<AlignToReference>;
    struct Params
    {
        int   maxFeatures = 2000;                                                               // ORB特征数量
        float ratioTest = 0.75f;                                                                // Lowe ratio
        int   minInliers = 20;                                                                  // 低于这个认为对齐失败
        double ransacReprojThreshold = 3.0;                                                     // RANSAC阈值(像素)
        int   dilateR = 2;                                                                      // 生成maskB后膨胀一下(防边缘残留)
    };

public:
    AlignToReference();

    // 设置基准图 A 与 maskA（maskA必须和A同尺寸）
    bool SetReference(const QImage& refRgb, const QImage& refMaskGray, QList<QPointF> labelPoints, const Params& p = Params());

    // 给一张 B，返回对齐后的 maskB（与B同尺寸）
    // bOk=false 表示对齐失败（你可选择返回空mask或直接跳过）
    QImage MakeMaskFor(const QImage& imgB, QList<QPointF>* ptsOutB, bool* bOk = nullptr) const;

    bool IsReady() const { return ready_; }

private:
    bool ComputeOrb(const cv::Mat& gray, std::vector<cv::KeyPoint>& kps, cv::Mat& desc) const;

    bool EstimateAffine_B2A(const cv::Mat& grayB, cv::Mat& M_B2A, int& outInliers) const;       // 估算仿射
    std::vector<cv::Point2f> TransformPoints_AtoB(const cv::Mat& M_B2A) const;                  // 将基准图A上的点转换到图像B的坐标系上
    static cv::Mat ToGray(const cv::Mat& bgr);
    static cv::Mat ToGrayMaskU8(const QImage& maskGray);

    static cv::Mat WarpMask_AtoB(
        const cv::Mat& maskA_u8,
        const cv::Mat& M_B2A,
        const cv::Size& sizeB,
        int dilateR);

private:
    std::vector<cv::Point2f> qListToCvVec(const QList<QPointF>& points) const;
    QList<QPointF> cvVecToQList(const std::vector<cv::Point2f>& points) const;
    cv::Mat QImageToBgrMat(const QImage& img) const;
    QImage GrayMatToQImage(const cv::Mat& gray8) const;

private:
    Params p_;
    bool ready_ = false;

    // Reference A
    cv::Mat refGrayA_;                                                                                      // 基准图
    cv::Mat refMaskA_;                                                                                      // 基准图对应的mask
    std::vector<cv::Point2f> refPointsA_;                                                                   // 存储基准图 A 上的穴位标签点

    std::vector<cv::KeyPoint> kpsA_;
    cv::Mat descA_;                                                                                         // ORB descriptors
};

#endif // ALIGNTOREFERENCE_H