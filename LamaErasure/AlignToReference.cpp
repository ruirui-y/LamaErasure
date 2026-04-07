#include "AlignToReference.h"
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <QDebug>

AlignToReference::AlignToReference() {}

static inline bool ValidateGray8(const cv::Mat& m)
{
    if (m.empty()) return false;
    if (m.depth() != CV_8U) return false;
    if (m.channels() != 1) return false;
    if (m.rows <= 0 || m.cols <= 0) return false;
    if (!m.data) return false;
    // 关键：step 必须 >= cols（每行至少要有 cols 个字节）
    if ((int)m.step < m.cols) return false;
    return true;
}

// 把任意输入变成：CV_8UC1 + step==cols + self-owned buffer
static cv::Mat MakeSafeGray8(const cv::Mat& in)
{
    if (in.empty()) return cv::Mat();

    cv::Mat m = in;

    // depth 兜底
    if (m.depth() != CV_8U)
        m.convertTo(m, CV_8U);

    // channel 兜底
    if (m.channels() == 3)
        cv::cvtColor(m, m, cv::COLOR_BGR2GRAY);
    else if (m.channels() == 4)
        cv::cvtColor(m, m, cv::COLOR_BGRA2GRAY);
    else if (m.channels() != 1)
        return cv::Mat();

    // 关键：重建一个“step=cols”的干净 Mat（逐行 memcpy，完全不依赖原 step）
    cv::Mat out(m.rows, m.cols, CV_8UC1);
    for (int y = 0; y < m.rows; ++y) {
        const uchar* src = m.ptr<uchar>(y);
        uchar* dst = out.ptr<uchar>(y);
        memcpy(dst, src, (size_t)m.cols);
    }
    return out;
}

cv::Mat AlignToReference::ToGray(const cv::Mat& bgr)
{
    cv::Mat gray;
    if (bgr.channels() == 3) cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    else if (bgr.channels() == 4) cv::cvtColor(bgr, gray, cv::COLOR_BGRA2GRAY);
    else gray = bgr.clone();
    return gray;
}

cv::Mat AlignToReference::ToGrayMaskU8(const QImage& maskGray)
{
    QImage m = maskGray.convertToFormat(QImage::Format_Grayscale8);
    cv::Mat mat(m.height(), m.width(), CV_8UC1,
        const_cast<uchar*>(m.bits()), m.bytesPerLine());
    cv::Mat out = mat.clone();
    // 二值化到 0/255
    cv::threshold(out, out, 127, 255, cv::THRESH_BINARY);
    return out;
}

bool AlignToReference::ComputeOrb(const cv::Mat& grayIn,
    std::vector<cv::KeyPoint>& kps,
    cv::Mat& desc) const
{
    kps.clear();
    desc.release();

    // 先强行做成“绝对安全的灰度 Mat”
    cv::Mat gray = MakeSafeGray8(grayIn);
    if (!ValidateGray8(gray)) {
        qDebug() << "ComputeOrb: invalid gray input. "
            << "empty=" << gray.empty()
            << "rows=" << gray.rows
            << "cols=" << gray.cols
            << "type=" << gray.type()
            << "step=" << (int)gray.step
            << "cont=" << gray.isContinuous();
        return false;
    }

    // 给 ORB 更保守的参数（避免小图/边界问题）
    const int edgeTh = 15;      // 31 有时对小图/ROI 很不友好
    const int patch = 31;       // 常用 31
    const int fastTh = 10;

    cv::Ptr<cv::ORB> orb = cv::ORB::create(
        std::max(200, p_.maxFeatures),
        1.2f,
        8,
        edgeTh,
        0,
        2,
        cv::ORB::HARRIS_SCORE,
        patch,
        fastTh
    );

    orb->detectAndCompute(gray, cv::noArray(), kps, desc);

    return (!kps.empty() && !desc.empty());
}

bool AlignToReference::SetReference(const QImage& refRgb, const QImage& refMaskGray, QList<QPointF> labelPoints, const QRectF& refRect, const Params& p)
{
    p_ = p;
    ready_ = false;

    if (refRgb.isNull() || refMaskGray.isNull()) return false;

    // 1. 图像处理与ORB特征提取
    cv::Mat bgrA = QImageToBgrMat(refRgb).clone();
    refGrayA_ = ToGray(bgrA);
    refMaskA_ = ToGrayMaskU8(refMaskGray);

    if (refMaskA_.size() != refGrayA_.size()) return false;
    if (!ComputeOrb(refGrayA_, kpsA_, descA_)) return false;

    // 2. 将QList转换成std::vector
    refPointsA_ = qListToCvVec(labelPoints);

    // 3. 存基准图的大框 (Bounding Box)
    refRectA_ = refRect;

    ready_ = true;
    return true;
}

bool AlignToReference::EstimateAffine_B2A(const cv::Mat& grayB, cv::Mat& M_B2A, int& outInliers) const
{
    outInliers = 0;
    M_B2A.release();

    // 提取特征
    std::vector<cv::KeyPoint> kpsB;
    cv::Mat descB;
    if (!ComputeOrb(grayB, kpsB, descB)) return false;

    // 将B里的特征取与基准图的点进行逐一比对
    cv::BFMatcher matcher(cv::NORM_HAMMING, false);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(descB, descA_, knn, 2);

    // 筛选出好的点对
    std::vector<cv::DMatch> good;
    good.reserve(knn.size());
    for (auto& m : knn)
    {
        if (m.size() < 2) continue;
        if (m[0].distance < p_.ratioTest * m[1].distance)
            good.push_back(m[0]);
    }

    if ((int)good.size() < 8) return false;

    // 取点对：B->A
    std::vector<cv::Point2f> ptsB, ptsA;
    ptsB.reserve(good.size());
    ptsA.reserve(good.size());

    for (auto& d : good)
    {
        ptsB.push_back(kpsB[d.queryIdx].pt);
        ptsA.push_back(kpsA_[d.trainIdx].pt);
    }

    // 仿射（旋转/缩放/平移）+ RANSAC
    cv::Mat inlierMask;
    M_B2A = cv::estimateAffinePartial2D(
        ptsB, ptsA,
        inlierMask,
        cv::RANSAC,
        p_.ransacReprojThreshold);

    if (M_B2A.empty()) return false;

    // 统计 inliers
    int cnt = 0;
    for (int i = 0; i < inlierMask.rows; ++i)
        cnt += (inlierMask.at<uchar>(i) != 0);

    outInliers = cnt;
    return (cnt >= p_.minInliers);
}

std::vector<cv::Point2f> AlignToReference::TransformPoints_AtoB(const cv::Mat& M_B2A) const
{
    if (M_B2A.empty() || refPointsA_.empty()) return {};

    // 1. 求逆矩阵：把 [B -> A] 的变换转为 [A -> B]
    cv::Mat M_A2B;
    cv::invertAffineTransform(M_B2A, M_A2B);

    // 2. 使用 OpenCV 的 transform 函数批量变换坐标点
    std::vector<cv::Point2f> ptsB;
    cv::transform(refPointsA_, ptsB, M_A2B);

    return ptsB;
}

cv::Mat AlignToReference::WarpMask_AtoB(
    const cv::Mat& maskA_u8,
    const cv::Mat& M_B2A,
    const cv::Size& sizeB,
    int dilateR)
{
    // 需要 A->B：对 M_B2A 求逆
    cv::Mat M_A2B;
    cv::invertAffineTransform(M_B2A, M_A2B);

    cv::Mat maskB(sizeB, CV_8UC1, cv::Scalar(0));
    cv::warpAffine(
        maskA_u8, maskB, M_A2B, sizeB,
        cv::INTER_NEAREST,
        cv::BORDER_CONSTANT,
        cv::Scalar(0));

    // 二值化一下防止插值毛边（理论上nearest不会，但保险）
    cv::threshold(maskB, maskB, 127, 255, cv::THRESH_BINARY);

    if (dilateR > 0)
    {
        cv::Mat k = cv::getStructuringElement(cv::MORPH_ELLIPSE,
            cv::Size(dilateR * 2 + 1, dilateR * 2 + 1));
        cv::dilate(maskB, maskB, k, cv::Point(-1, -1), 1);
    }

    return maskB;
}

// 工具函数：QList<QPointF> -> std::vector<cv::Point2f>
std::vector<cv::Point2f> AlignToReference::qListToCvVec(const QList<QPointF>& points) const
{
    std::vector<cv::Point2f> cvPoints;
    cvPoints.reserve(points.size());
    for (const QPointF& p : points)
    {
        cvPoints.push_back(cv::Point2f(static_cast<float>(p.x()),
            static_cast<float>(p.y())));
    }
    return cvPoints;
}

// 工具函数：std::vector<cv::Point2f> -> QList<QPointF>
QList<QPointF> AlignToReference::cvVecToQList(const std::vector<cv::Point2f>& points) const
{
    QList<QPointF> qPoints;
    for (const auto& p : points)
    {
        qPoints.append(QPointF(static_cast<double>(p.x),
            static_cast<double>(p.y)));
    }
    return qPoints;
}

cv::Mat AlignToReference::QImageToBgrMat(const QImage& img) const
{
    QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    cv::Mat m(rgb.height(), rgb.width(), CV_8UC3,
        (void*)rgb.constBits(), rgb.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(m, bgr, cv::COLOR_RGB2BGR);
    return bgr.clone();
}

QImage AlignToReference::GrayMatToQImage(const cv::Mat& gray)  const
{
    QImage out(gray.cols, gray.rows, QImage::Format_Grayscale8);
    for (int y = 0; y < gray.rows; ++y) {
        memcpy(out.scanLine(y), gray.ptr(y), gray.cols);
    }
    return out;
}

QImage AlignToReference::MakeMaskFor(const QImage& imgB, QList<QPointF>* ptsOutB, QRectF* rectOutB, bool* bOk) const
{
    // 0. 初始化与安全检查
    if (bOk) *bOk = false;
    if (!ready_ || imgB.isNull()) return QImage();

    // 将 Qt 图像转换为 OpenCV 格式进行计算
    cv::Mat bgrB = QImageToBgrMat(imgB);
    cv::Mat grayB = ToGray(bgrB);

    // 1. 特征匹配与变换矩阵计算
    cv::Mat M_B2A;                                                                  // 存储从当前图 B 到基准图 A 的仿射变换矩阵
    int inliers = 0;
    if (!EstimateAffine_B2A(grayB, M_B2A, inliers))
    {
        return QImage();                                                            // 如果匹配特征点不足，则无法建立空间对应关系，直接跳过
    }

    // 2. 标签坐标投影 (Label Transformation)
    if (ptsOutB)
    {
        // a. 利用 M_B2A 的逆矩阵，在 OpenCV 空间内完成坐标变换
        std::vector<cv::Point2f> ptsCvB = TransformPoints_AtoB(M_B2A);

        // b. 将变换后的 OpenCV 坐标转换回 Qt 的 QList 格式，供外部保存标签使用
        *ptsOutB = cvVecToQList(ptsCvB);
    }

    // 3. 自动计算缩放、平移后的大框 (Bounding Box)
    if (rectOutB && !refRectA_.isNull())
    {
        // a. 求 A 到 B 的逆矩阵（把 A 上的框映射到 B 上）
        cv::Mat M_A2B;
        cv::invertAffineTransform(M_B2A, M_A2B);

        // b. 提取基准框的 4 个角点
        std::vector<cv::Point2f> cornersA = {
            cv::Point2f(refRectA_.left(), refRectA_.top()),
            cv::Point2f(refRectA_.right(), refRectA_.top()),
            cv::Point2f(refRectA_.right(), refRectA_.bottom()),
            cv::Point2f(refRectA_.left(), refRectA_.bottom())
        };

        // c. 用仿射逆矩阵把这 4 个角点投影到目标图 B 上
        std::vector<cv::Point2f> cornersB;
        cv::transform(cornersA, cornersB, M_A2B);

        // d. 求变换后的 4 个点在 B 图上的“最小正交外接矩形”
        cv::Rect cvRectB = cv::boundingRect(cornersB);

        // e. 转换回 Qt 的 QRectF 格式
        *rectOutB = QRectF(cvRectB.x, cvRectB.y, cvRectB.width, cvRectB.height);
    }

    // 4. 掩码图对齐变换 (Mask Warping)
    // 将基准图的 MaskA 应用仿射变换，生成覆盖在当前图 B 上的 MaskB
    // p_.dilateR 用于对生成的遮罩进行膨胀处理，确保完全盖住贴纸边缘
    cv::Mat maskB = WarpMask_AtoB(refMaskA_, M_B2A, grayB.size(), p_.dilateR);

    // 5. 返回结果
    if (bOk) *bOk = true;
    return GrayMatToQImage(maskB);
}