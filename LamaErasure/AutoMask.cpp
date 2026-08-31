#include "AutoMask.h"
#include <algorithm>
#include <cmath>
#include <QDebug>

AutoMask::AutoMask(QObject *parent)
	: QObject(parent)
{}

AutoMask::~AutoMask()
{}

cv::Mat AutoMask::QImageToBgrMat(const QImage& img) const
{
    QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    cv::Mat m(rgb.height(), rgb.width(), CV_8UC3,
        (void*)rgb.constBits(), rgb.bytesPerLine());
    cv::Mat bgr;
    cv::cvtColor(m, bgr, cv::COLOR_RGB2BGR);
    return bgr.clone();
}

QImage AutoMask::GrayMatToQImage(const cv::Mat& gray)  const
{
    QImage out(gray.cols, gray.rows, QImage::Format_Grayscale8);
    for (int y = 0; y < gray.rows; ++y) {
        memcpy(out.scanLine(y), gray.ptr(y), gray.cols);
    }
    return out;
}

QImage AutoMask::AutoMaskFromStickers(const QImage& srcRgb, MaskParams mask_params)
{
    if (srcRgb.isNull()) return QImage();

    cv::Mat bgr = QImageToBgrMat(srcRgb);

    // 1) HSV：筛白色（低S + 高V）色相/饱和度/亮度
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

    // 白贴纸：S 很低，V 很高
    cv::Scalar lower(0, 0, 170);   // H随便，S<=60，V>=170
    cv::Scalar upper(180, 60, 255);
    cv::Mat bin;
    cv::inRange(hsv, lower, upper, bin);

    // 2) 形态学：去噪 + 填洞
    cv::Mat k3 = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::Mat k5 = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::morphologyEx(bin, bin, cv::MORPH_OPEN, k3, cv::Point(-1, -1), 1);
    cv::morphologyEx(bin, bin, cv::MORPH_CLOSE, k5, cv::Point(-1, -1), 2);

    // 3) 找轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    qDebug() << "contours = " << contours.size();

    // 4) 轮廓 → 圆候选
    std::vector<CircleCand> cands;
    cands.reserve(contours.size());

    const int W = bgr.cols, H = bgr.rows;

    for (auto& ct : contours) 
    {
        double area = cv::contourArea(ct);
        if (area < 50) continue;                                                    // 太小的噪点不要（可调）

        double peri = cv::arcLength(ct, true);
        if (peri <= 1e-6) continue;

        // 圆形度：1.0 最圆
        double circularity = 4.0 * CV_PI * area / (peri * peri);
        if (circularity < mask_params.circularity) continue;                        // 可调：0.6~0.8

        cv::Point2f c; float r;
        cv::minEnclosingCircle(ct, c, r);

        // 半径范围过滤
        if (r < mask_params.min_r || r > mask_params.max_r) continue;

        // 贴纸一般在图中部偏上
        if (c.x < 0 || c.y < 0 || c.x >= W || c.y >= H) continue;

        // 打分：圆形度高 + 面积大更像贴纸
        double score = circularity * std::sqrt(area);
        cands.push_back({ c, r, score });
    }

    // 如果没找到，返回全黑 mask
    cv::Mat mask(H, W, CV_8UC1, cv::Scalar(0));
    if (cands.empty()) return GrayMatToQImage(mask);

    // 5) 取最好的 3 个
    std::sort(cands.begin(), cands.end(),
        [](const CircleCand& a, const CircleCand& b) { return a.score > b.score; });

    const int takeN = std::min<int>(mask_params.mask_num, (int)cands.size());

    for (int i = 0; i < takeN; ++i) {
        auto& cc = cands[i];
        int rr = (int)std::lround(cc.r) + std::max(0, mask_params.marginPx);
        cv::circle(mask, cc.c, rr, cv::Scalar(255), -1, cv::LINE_AA);
    }

    // 6) 轻微膨胀，防止边缘残留
    if (mask_params.marginPx > 0) {
        cv::Mat k = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
        cv::dilate(mask, mask, k, cv::Point(-1, -1), 1);
    }

    return GrayMatToQImage(mask);
}