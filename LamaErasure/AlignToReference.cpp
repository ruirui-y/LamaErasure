#include "AlignToReference.h"
#include <opencv2/imgproc.hpp>
#include <QDebug>
#include <algorithm>
#include <cmath>

AlignToReference::AlignToReference() {}

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

// 把单个 mask 补丁合并进 finalMask（处理边界裁剪，重叠处 bitwise OR）
static void PasteMaskPatchImpl(cv::Mat& finalMask, const cv::Mat& patch, int dstX, int dstY)
{
    if (patch.empty() || finalMask.empty()) return;

    const int fW = finalMask.cols, fH = finalMask.rows;
    const int pW = patch.cols, pH = patch.rows;

    int dstX0 = dstX, dstY0 = dstY;
    int srcX0 = 0, srcY0 = 0;
    int copyW = pW, copyH = pH;

    if (dstX0 < 0) { srcX0 = -dstX0; copyW -= srcX0; dstX0 = 0; }
    if (dstY0 < 0) { srcY0 = -dstY0; copyH -= srcY0; dstY0 = 0; }
    if (dstX0 + copyW > fW) copyW = fW - dstX0;
    if (dstY0 + copyH > fH) copyH = fH - dstY0;
    if (copyW <= 0 || copyH <= 0) return;

    cv::Rect dstRect(dstX0, dstY0, copyW, copyH);
    cv::Rect srcRect(srcX0, srcY0, copyW, copyH);
    cv::Mat dstRoi = finalMask(dstRect);
    const cv::Mat srcRoi = patch(srcRect);
    cv::bitwise_or(dstRoi, srcRoi, dstRoi);
}

void AlignToReference::PasteMaskPatch(cv::Mat& finalMask, const cv::Mat& patch, int dstX, int dstY)
{
    PasteMaskPatchImpl(finalMask, patch, dstX, dstY);
}

bool AlignToReference::SetReference(const QImage& refRgb, const QImage& refMaskGray,
                                    QList<QPointF> labelPoints, const QRectF& refRect,
                                    const Params& p)
{
    p_ = p;
    ready_ = false;
    localTracks_.clear();
    pointTrackId_.clear();
    bbLeftMargin_ = bbTopMargin_ = bbRightMargin_ = bbBottomMargin_ = 0;

    if (refRgb.isNull() || refMaskGray.isNull()) return false;

    // 1. 保存完整参考灰度图与 Mask
    cv::Mat bgrA = QImageToBgrMat(refRgb).clone();
    refGrayA_ = ToGray(bgrA);
    refMaskA_ = ToGrayMaskU8(refMaskGray);
    if (refMaskA_.size() != refGrayA_.size()) return false;

    // 2. Mask 必须非空
    std::vector<cv::Point> nz;
    cv::findNonZero(refMaskA_, nz);
    if (nz.empty()) {
        qDebug() << "SetReference: mask is empty.";
        return false;
    }

    // 3. 保存标签点与 BoundingBox（训练标签，不参与追踪）
    refPointsA_ = labelPoints;
    refRectA_   = refRect;

    // 4. 拆分独立 Mask + 生成局部模板
    if (!BuildLocalTracks()) return false;

    // 5. 绑定 labelPoint -> 最近 track（按空间最近，不依赖 vector 顺序）
    pointTrackId_.clear();
    pointTrackId_.reserve(refPointsA_.size());
    for (const QPointF& pt : refPointsA_) {
        int best = -1;
        double bestD = 1e18;
        for (const LocalTrack& t : localTracks_) {
            const double dx = pt.x() - t.refCenter.x;
            const double dy = pt.y() - t.refCenter.y;
            const double d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; best = t.id; }
        }
        pointTrackId_.append(best);
    }

    ready_ = true;
    return true;
}

bool AlignToReference::BuildLocalTracks()
{
    localTracks_.clear();

    // 双保险二值化
    cv::Mat bin = refMaskA_.clone();
    cv::threshold(bin, bin, 127, 255, cv::THRESH_BINARY);

    // 拆分互不连接的 Component（label 0 = 背景）
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(bin, labels, stats, centroids, 8, CV_32S);
    if (n <= 1) {
        qDebug() << "BuildLocalTracks: no components found in mask.";
        return false;
    }

    const int W = refGrayA_.cols, H = refGrayA_.rows;

    for (int i = 1; i < n; ++i) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < p_.minMaskComponentArea) continue;   // 过滤噪点

        const int x = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(i, cv::CC_STAT_TOP);
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        cv::Rect maskRect(x, y, w, h);
        cv::Point2f refCenter(maskRect.x + maskRect.width / 2.0,
                              maskRect.y + maskRect.height / 2.0);

        // 在 maskRect 四周加 padding，使模板包含绿色标记 + 周围皮肤/纹理/轮廓
        cv::Rect tpl = maskRect;
        tpl.x -= p_.localTemplatePadding;
        tpl.y -= p_.localTemplatePadding;
        tpl.width  += p_.localTemplatePadding * 2;
        tpl.height += p_.localTemplatePadding * 2;
        tpl &= cv::Rect(0, 0, W, H);                    // 裁剪到原图范围
        if (tpl.width < 4 || tpl.height < 4) continue;

        LocalTrack t;
        t.id = localTracks_.size();
        t.maskRect = maskRect;
        t.templateRect = tpl;
        t.refCenter = refCenter;
        // 关键：模板从原始参考灰度图截取，绝不用带红色 Mask Overlay / 黑色 Mask 的预览图
        t.templateGray = refGrayA_(tpl).clone();
        t.maskPatch = refMaskA_(maskRect).clone();
        if (t.templateGray.empty()) continue;

        localTracks_.append(t);
    }

    if (localTracks_.empty()) {
        qDebug() << "BuildLocalTracks: all components filtered out.";
        return false;
    }

    // 固定排序：center.y 升序，相同则 center.x 升序（与 Q 流程一致，保证 Track0~6 顺序确定）
    std::sort(localTracks_.begin(), localTracks_.end(),
        [](const LocalTrack& a, const LocalTrack& b) {
            if (a.refCenter.y != b.refCenter.y) return a.refCenter.y < b.refCenter.y;
            return a.refCenter.x < b.refCenter.x;
        });
    for (int i = 0; i < localTracks_.size(); ++i) localTracks_[i].id = i;

    // 清晰日志：确认程序确实识别出了 N 个 Mask（已按固定规则排序）
    qDebug().noquote() << QString("SetRef imageSize=%1x%2").arg(W).arg(H);
    qDebug().noquote() << QString("SetRef LocalTrack count=%1").arg(localTracks_.size());
    qDebug().noquote() << QString("Reference Sorted Tracks:");
    for (const LocalTrack& t : localTracks_) {
        qDebug().noquote() << QString(
            "Track[%1] maskRect=(%2,%3,%4,%5) templateRect=(%6,%7,%8,%9) refCenter=(%10,%11)")
            .arg(t.id)
            .arg(t.maskRect.x).arg(t.maskRect.y).arg(t.maskRect.width).arg(t.maskRect.height)
            .arg(t.templateRect.x).arg(t.templateRect.y).arg(t.templateRect.width).arg(t.templateRect.height)
            .arg(QString::number(t.refCenter.x, 'f', 1)).arg(QString::number(t.refCenter.y, 'f', 1));
    }

    // 计算参考 Mask 联合包围盒，并据此推导 BoundingBox margin
    int ux1 = W, uy1 = H, ux2 = 0, uy2 = 0;
    for (const LocalTrack& t : localTracks_) {
        ux1 = qMin(ux1, t.maskRect.x);
        uy1 = qMin(uy1, t.maskRect.y);
        ux2 = qMax(ux2, t.maskRect.x + t.maskRect.width);
        uy2 = qMax(uy2, t.maskRect.y + t.maskRect.height);
    }
    const cv::Rect refMaskUnion(ux1, uy1, ux2 - ux1, uy2 - uy1);

    if (!refRectA_.isNull()) {
        const int rx1 = (int)std::floor(refRectA_.left());
        const int ry1 = (int)std::floor(refRectA_.top());
        const int rx2 = (int)std::ceil(refRectA_.right());
        const int ry2 = (int)std::ceil(refRectA_.bottom());
        // 训练框相对 Mask 联合盒的 4 个 margin（正负都保留，目标图原样还原）
        bbLeftMargin_   = refMaskUnion.x - rx1;
        bbTopMargin_    = refMaskUnion.y - ry1;
        bbRightMargin_  = rx2 - (refMaskUnion.x + refMaskUnion.width);   // = rx2 - refMaskUnion.right
        bbBottomMargin_ = ry2 - (refMaskUnion.y + refMaskUnion.height);  // = ry2 - refMaskUnion.bottom
        qDebug().noquote() << QString(
            "SetRef bboxMargin L=%1 T=%2 R=%3 B=%4 (refUnion=(%5,%6,%7,%8))")
            .arg(bbLeftMargin_).arg(bbTopMargin_).arg(bbRightMargin_).arg(bbBottomMargin_)
            .arg(refMaskUnion.x).arg(refMaskUnion.y).arg(refMaskUnion.width).arg(refMaskUnion.height);
    } else {
        qDebug() << "SetRef: refRect(BoundingBox) is null, target bbox will be mask union only.";
    }

    return true;
}

// 第一阶段：大范围粗匹配，仅给出粗位移预测，不直接决定成功/失败
bool AlignToReference::CoarseMatchTrack(const LocalTrack& track, const cv::Mat& grayB,
                                        const cv::Size& imgSize, LocalMatchResult& out) const
{
    out.id = track.id;
    out.refCenter = track.refCenter;
    out.coarseDx = 0;
    out.coarseDy = 0;
    out.coarseScore = 0.0;
    out.ok = false;
    out.dx = 0;
    out.dy = 0;
    out.score = 0.0;

    const int W = imgSize.width, H = imgSize.height;
    const cv::Rect& tpl = track.templateRect;
    const int tw = tpl.width, th = tpl.height;

    // 粗匹配二维搜索窗口（局部，避免整图搜索导致 Template 串点）
    int sx1 = qMax(0, tpl.x - p_.searchRadiusX);
    int sy1 = qMax(0, tpl.y - p_.searchRadiusY);
    int sx2 = qMin(W, tpl.x + tw + p_.searchRadiusX);
    int sy2 = qMin(H, tpl.y + th + p_.searchRadiusY);
    cv::Rect searchRect(sx1, sy1, sx2 - sx1, sy2 - sy1);
    out.coarseSearchRect = searchRect;

    if (searchRect.width < tw || searchRect.height < th) {
        qDebug().noquote() << QString("CoarseMatch[%1] search window too small").arg(track.id);
        return false;
    }

    cv::Mat searchGray = grayB(searchRect);
    cv::Mat result;
    cv::matchTemplate(searchGray, track.templateGray, result, cv::TM_CCOEFF_NORMED);
    if (result.empty()) return false;

    double minVal = 0, maxVal = 0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

    const int currentTemplateX = searchRect.x + maxLoc.x;
    const int currentTemplateY = searchRect.y + maxLoc.y;
    out.coarseMatchRect = cv::Rect(currentTemplateX, currentTemplateY, tw, th);
    out.coarseCenter = cv::Point2f(currentTemplateX + tw / 2.0,
                                   currentTemplateY + th / 2.0);

    out.coarseDx = currentTemplateX - tpl.x;
    out.coarseDy = currentTemplateY - tpl.y;
    out.coarseScore = maxVal;

    return true;
}

// 第二阶段：基于全局位移预测位置，仅在附近做小范围精匹配，得到最终 dx/dy
bool AlignToReference::RefinedMatchTrack(const LocalTrack& track, const cv::Mat& grayB,
                                         const cv::Size& imgSize, int globalDx, int globalDy,
                                         LocalMatchResult& out) const
{
    const int W = imgSize.width, H = imgSize.height;
    const cv::Rect& tpl = track.templateRect;
    const int tw = tpl.width, th = tpl.height;

    // 预测模板位置 = 参考模板位置 + 整只手的全局位移
    const int predictedX = tpl.x + globalDx;
    const int predictedY = tpl.y + globalDy;

    int sx1 = qMax(0, predictedX - p_.refineRadiusX);
    int sy1 = qMax(0, predictedY - p_.refineRadiusY);
    int sx2 = qMin(W, predictedX + tw + p_.refineRadiusX);
    int sy2 = qMin(H, predictedY + th + p_.refineRadiusY);
    cv::Rect searchRect(sx1, sy1, sx2 - sx1, sy2 - sy1);
    out.searchRect = searchRect;

    if (searchRect.width < tw || searchRect.height < th) {
        qDebug().noquote() << QString("RefinedMatch[%1] search window too small").arg(track.id);
        return false;
    }

    cv::Mat searchGray = grayB(searchRect);
    cv::Mat result;
    cv::matchTemplate(searchGray, track.templateGray, result, cv::TM_CCOEFF_NORMED);
    if (result.empty()) return false;

    double minVal = 0, maxVal = 0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

    const int currentTemplateX = searchRect.x + maxLoc.x;
    const int currentTemplateY = searchRect.y + maxLoc.y;
    out.matchRect = cv::Rect(currentTemplateX, currentTemplateY, tw, th);
    out.currentCenter = cv::Point2f(currentTemplateX + tw / 2.0,
                                    currentTemplateY + th / 2.0);

    out.dx = currentTemplateX - tpl.x;
    out.dy = currentTemplateY - tpl.y;
    out.score = maxVal;

    return true;
}

int AlignToReference::MedianInt(const QVector<int>& v)
{
    if (v.isEmpty()) return 0;
    QVector<int> s = v;
    std::sort(s.begin(), s.end());
    const int n = s.size();
    if (n % 2 == 1) return s[n / 2];
    return (s[n / 2 - 1] + s[n / 2]) / 2;
}

QImage AlignToReference::MakeMaskFor(const QImage& imgB, QList<QPointF>* ptsOutB, QRectF* rectOutB,
                                     bool* bOk, QVector<LocalMatchResult>* outTracks) const
{
    // 0. 初始化与安全检查
    if (bOk) *bOk = false;
    if (!ready_ || imgB.isNull()) return QImage();

    cv::Mat bgrB = QImageToBgrMat(imgB);
    cv::Mat grayB = ToGray(bgrB);
    const int W = grayB.cols, H = grayB.rows;

    // V1：参考图与目标图尺寸必须一致，不做自动缩放
    if (grayB.size() != refGrayA_.size()) {
        qDebug() << "MakeMaskFor size mismatch: ref="
                 << refGrayA_.cols << "x" << refGrayA_.rows
                 << " cur=" << W << "x" << H;
        if (outTracks) outTracks->clear();
        return QImage();
    }

    const int N = localTracks_.size();
    QVector<LocalMatchResult> tracks(N);
    int globalDx = 0, globalDy = 0;
    RunTwoStageMatch(grayB, grayB.size(), tracks, globalDx, globalDy);

    // 严格模式（TestOne / Batch）：任一 Track 失败 -> 整张图失败，不出错标签
    int failed = 0;
    for (int i = 0; i < N; ++i) if (!tracks[i].ok) ++failed;
    if (failed > 0) {
        if (outTracks) *outTracks = tracks;
        if (bOk) *bOk = false;
        return QImage();
    }

    // 每个 Mask 按自己的最终 dx/dy 单独移动，合并成 finalMask
    QVector<bool> use(N, true);
    cv::Mat finalMask = ComposeFinalMask(tracks, use, H, W);

    // 4. 膨胀（保持现有逻辑）
    if (p_.dilateR > 0) {
        cv::Mat k = cv::getStructuringElement(cv::MORPH_ELLIPSE,
            cv::Size(p_.dilateR * 2 + 1, p_.dilateR * 2 + 1));
        cv::dilate(finalMask, finalMask, k, cv::Point(-1, -1), 1);
    }

    const cv::Rect curMaskUnion = MaskUnionRect(tracks, use, W, H);

    // 5. 标签点：跟随最近 track 平移（用最终 refined dx/dy，仅供调试；最终标签由 Q 从 Mask 重提）
    if (ptsOutB) {
        ptsOutB->clear();
        ptsOutB->reserve(refPointsA_.size());
        for (int pi = 0; pi < refPointsA_.size(); ++pi) {
            const QPointF& pt = refPointsA_[pi];
            const int tid = (pi < pointTrackId_.size()) ? pointTrackId_[pi] : -1;
            int dx = 0, dy = 0;
            if (tid >= 0) {
                for (const LocalMatchResult& r : tracks) {
                    if (r.id == tid) { dx = r.dx; dy = r.dy; break; }
                }
            }
            ptsOutB->append(QPointF(pt.x() + dx, pt.y() + dy));
        }
    }

    // 6. 自动生成目标图 BoundingBox（联合包围盒 + 参考 margin，裁剪到图内）
    if (rectOutB) {
        int bx1 = curMaskUnion.x - bbLeftMargin_;
        int by1 = curMaskUnion.y - bbTopMargin_;
        int bx2 = curMaskUnion.x + curMaskUnion.width + bbRightMargin_;
        int by2 = curMaskUnion.y + curMaskUnion.height + bbBottomMargin_;
        bx1 = qMax(0, bx1);
        by1 = qMax(0, by1);
        bx2 = qMin(W, bx2);
        by2 = qMin(H, by2);
        *rectOutB = QRectF(bx1, by1, qMax(1, bx2 - bx1), qMax(1, by2 - by1));
    }

    // 7. 汇总日志
    qDebug().noquote() << QString(
        "LocalMatch summary: success=%1 failed=0 finalMaskRect=(%2,%3,%4,%5)")
        .arg(N)
        .arg(curMaskUnion.x).arg(curMaskUnion.y)
        .arg(curMaskUnion.width).arg(curMaskUnion.height);

    if (outTracks) *outTracks = tracks;
    if (bOk) *bOk = true;
    return GrayMatToQImage(finalMask);
}

// 两阶段匹配（Coarse -> Global median -> Refined -> 校验 -> 重复检测），填充 tracks / globalDx/globalDy
void AlignToReference::RunTwoStageMatch(const cv::Mat& grayB, const cv::Size& imgSize,
                                        QVector<LocalMatchResult>& tracks, int& globalDx, int& globalDy) const
{
    const int N = localTracks_.size();
    QVector<bool> coarseValid(N, false);

    // 第一阶段：Coarse 大范围匹配，得到每个 Track 粗位移
    QVector<int> okCoarseDx, okCoarseDy;
    for (int i = 0; i < N; ++i) {
        const LocalTrack& t = localTracks_[i];
        coarseValid[i] = CoarseMatchTrack(t, grayB, imgSize, tracks[i]);
        qDebug().noquote() << QString("CoarseMatch[%1] dx=%2 dy=%3 score=%4")
            .arg(t.id).arg(tracks[i].coarseDx).arg(tracks[i].coarseDy)
            .arg(QString::number(tracks[i].coarseScore, 'f', 3));
        if (coarseValid[i] && tracks[i].coarseScore >= p_.minLocalScore) {
            okCoarseDx.append(tracks[i].coarseDx);
            okCoarseDy.append(tracks[i].coarseDy);
        }
    }

    // 全局位移 = 粗位移中位数（抗单点串点干扰）
    globalDx = MedianInt(okCoarseDx);
    globalDy = MedianInt(okCoarseDy);
    qDebug().noquote() << QString("GlobalShift dx=%1 dy=%2 (from %3 valid coarse tracks)")
        .arg(globalDx).arg(globalDy).arg(okCoarseDx.size());

    // 第二阶段：Refined 在预测位置附近小范围精匹配
    for (int i = 0; i < N; ++i) {
        const LocalTrack& t = localTracks_[i];
        if (!coarseValid[i]) {
            tracks[i].ok = false;
            tracks[i].failReason = "coarse_failed";
            continue;
        }

        const bool rok = RefinedMatchTrack(t, grayB, imgSize, globalDx, globalDy, tracks[i]);
        if (!rok) {
            tracks[i].ok = false;
            tracks[i].failReason = "refine_window_invalid";
            continue;
        }

        const cv::Point2f predictedCenter(
            t.templateRect.x + t.templateRect.width / 2.0 + globalDx,
            t.templateRect.y + t.templateRect.height / 2.0 + globalDy);

        // 校验 1：分数过低
        if (tracks[i].score < p_.minLocalScore) {
            tracks[i].ok = false;
            tracks[i].failReason = "low_score";
            qDebug().noquote() << QString(
                "LocalTrack[%1] FAILED refCenter=(%2,%3) score=%4 (<%5)")
                .arg(t.id)
                .arg(QString::number(t.refCenter.x, 'f', 1)).arg(QString::number(t.refCenter.y, 'f', 1))
                .arg(QString::number(tracks[i].score, 'f', 3)).arg(QString::number(p_.minLocalScore, 'f', 2));
            continue;
        }

        // 校验 2：局部位移相对全局位移偏差过大（疑似串点/离群）
        if (std::abs(tracks[i].dx - globalDx) > p_.maxLocalDeviationX ||
            std::abs(tracks[i].dy - globalDy) > p_.maxLocalDeviationY) {
            tracks[i].ok = false;
            tracks[i].failReason = "excessive_deviation";
            qDebug().noquote() << QString(
                "LocalTrack[%1] FAILED: globalShift=(%2,%3) localShift=(%4,%5) deviation too large")
                .arg(t.id).arg(globalDx).arg(globalDy)
                .arg(tracks[i].dx).arg(tracks[i].dy);
            continue;
        }

        tracks[i].ok = true;
        qDebug().noquote() << QString(
            "RefinedMatch[%1] predictedCenter=(%2,%3) currentCenter=(%4,%5) dx=%6 dy=%7 score=%8")
            .arg(t.id)
            .arg(QString::number(predictedCenter.x, 'f', 1)).arg(QString::number(predictedCenter.y, 'f', 1))
            .arg(QString::number(tracks[i].currentCenter.x, 'f', 1)).arg(QString::number(tracks[i].currentCenter.y, 'f', 1))
            .arg(tracks[i].dx).arg(tracks[i].dy)
            .arg(QString::number(tracks[i].score, 'f', 3));
    }

    // 重复占用检测：两个 Track 当前中心过近 -> 较低分者判重复失败
    for (int i = 0; i < N; ++i) {
        if (!tracks[i].ok) continue;
        for (int j = i + 1; j < N; ++j) {
            if (!tracks[j].ok) continue;
            const double ddx = tracks[i].currentCenter.x - tracks[j].currentCenter.x;
            const double ddy = tracks[i].currentCenter.y - tracks[j].currentCenter.y;
            const double dist = std::sqrt(ddx * ddx + ddy * ddy);
            if (dist < p_.minTrackCenterDistance) {
                const int keep = (tracks[i].score >= tracks[j].score) ? i : j;
                const int rej  = (keep == i) ? j : i;
                tracks[rej].ok = false;
                tracks[rej].failReason = "duplicate";
                qDebug().noquote() << QString(
                    "Duplicate local match: Track[%1] conflicts with Track[%2] distance=%3 "
                    "scoreA=%4 scoreB=%5 -> Track[%6] rejected")
                    .arg(i).arg(j)
                    .arg(QString::number(dist, 'f', 1))
                    .arg(QString::number(tracks[i].score, 'f', 3)).arg(QString::number(tracks[j].score, 'f', 3))
                    .arg(rej);
            }
        }
    }

    int success = 0, failedCnt = 0, firstFailId = -1;
    QString firstFailReason;
    for (int i = 0; i < N; ++i) {
        if (tracks[i].ok) ++success;
        else { ++failedCnt; if (firstFailId < 0) { firstFailId = tracks[i].id; firstFailReason = tracks[i].failReason; } }
    }
    qDebug().noquote() << QString("RefinedMatch summary: success=%1 failed=%2%3")
        .arg(success).arg(failedCnt)
        .arg(failedCnt ? QString(" failedTrack=%1 reason=%2").arg(firstFailId).arg(firstFailReason)
                       : QString(""));
}

// 合并 use[i] 为 true 的 Track 的 maskPatch 到 finalMask（边界裁剪 + bitwise OR）
cv::Mat AlignToReference::ComposeFinalMask(const QVector<LocalMatchResult>& tracks,
                                            const QVector<bool>& use, int H, int W) const
{
    cv::Mat finalMask(H, W, CV_8UC1, cv::Scalar(0));
    for (int i = 0; i < (int)tracks.size(); ++i) {
        if (i >= use.size() || !use[i]) continue;
        const LocalTrack& t = localTracks_[i];
        const LocalMatchResult& r = tracks[i];
        const int dstX = t.maskRect.x + r.dx;
        const int dstY = t.maskRect.y + r.dy;
        PasteMaskPatch(finalMask, t.maskPatch, dstX, dstY);
    }
    return finalMask;
}

// 计算 use[i] 为 true 的 Track 移动后 Mask 联合包围盒
cv::Rect AlignToReference::MaskUnionRect(const QVector<LocalMatchResult>& tracks,
                                         const QVector<bool>& use, int W, int H) const
{
    int ux1 = W, uy1 = H, ux2 = 0, uy2 = 0;
    for (int i = 0; i < (int)tracks.size(); ++i) {
        if (i >= use.size() || !use[i]) continue;
        const LocalTrack& t = localTracks_[i];
        const LocalMatchResult& r = tracks[i];
        const int dstX = t.maskRect.x + r.dx;
        const int dstY = t.maskRect.y + r.dy;
        ux1 = qMin(ux1, dstX);
        uy1 = qMin(uy1, dstY);
        ux2 = qMax(ux2, dstX + t.maskRect.width);
        uy2 = qMax(uy2, dstY + t.maskRect.height);
    }
    return cv::Rect(ux1, uy1, qMax(1, ux2 - ux1), qMax(1, uy2 - uy1));
}

// 人工辅助模式（A 键）：只预测草稿，允许部分成功。失败的 Track 直接跳过。
QImage AlignToReference::MakeAssistMaskFor(const QImage& imgB, int* outSuccess, int* outTotal) const
{
    const int N = localTracks_.size();
    if (outSuccess) *outSuccess = 0;
    if (outTotal)  *outTotal = N;

    if (!ready_ || imgB.isNull()) return QImage();

    cv::Mat bgrB = QImageToBgrMat(imgB);
    cv::Mat grayB = ToGray(bgrB);
    const int W = grayB.cols, H = grayB.rows;
    if (grayB.size() != refGrayA_.size()) {
        qDebug() << "MakeAssistMaskFor size mismatch: ref=" << refGrayA_.cols << "x" << refGrayA_.rows
                 << " cur=" << W << "x" << H;
        return QImage();
    }

    QVector<LocalMatchResult> tracks(N);
    int gdx = 0, gdy = 0;
    RunTwoStageMatch(grayB, grayB.size(), tracks, gdx, gdy);

    int success = 0;
    for (int i = 0; i < N; ++i)
        if (tracks[i].ok) ++success;
    if (outSuccess) *outSuccess = success;

    // A 键日志：输出预测成功数与当前 assistMaskRadius
    qDebug() << "AssistMask: predicted="
             << success << "/" << N
             << "radius=" << p_.assistMaskRadius;

    if (success == 0) return QImage();

    // 关键：A 键只继承"预测出来的位置"，绝不继承上一张 Reference 的 maskPatch 尺寸 / 形状。
    // 对每个成功 Track，以 refCenter + 预测 dx/dy 的新中心为圆心，重画固定半径 assistMaskRadius 的标准圆。
    // 不调用 ComposeFinalMask() / PasteMaskPatch() / dilate()，因此不会搬上一张 Mask，也不会被放大。
    cv::Mat finalMask(H, W, CV_8UC1, cv::Scalar(0));
    for (int i = 0; i < N; ++i) {
        if (!tracks[i].ok) continue;
        const LocalTrack& t = localTracks_[i];
        const LocalMatchResult& r = tracks[i];
        const int cx = qRound(t.refCenter.x + r.dx);
        const int cy = qRound(t.refCenter.y + r.dy);
        cv::circle(finalMask, cv::Point(cx, cy), p_.assistMaskRadius,
                   cv::Scalar(255), -1, cv::LINE_8);
    }
    return GrayMatToQImage(finalMask);
}

// 从最终 Mask 重新提取 Component 中心（Q 流程唯一真相来源）
int AlignToReference::ExtractMaskCenters(const QImage& maskGray, int minComponentArea, QVector<QPointF>& centers)
{
    centers.clear();
    if (maskGray.isNull()) return 0;
    cv::Mat m = ToGrayMaskU8(maskGray);
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(m, labels, stats, centroids, 8, CV_32S);
    if (n <= 1) return 0;
    int count = 0;
    for (int i = 1; i < n; ++i) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < minComponentArea) continue;
        const double cx = centroids.at<double>(i, 0);
        const double cy = centroids.at<double>(i, 1);
        centers.append(QPointF(cx, cy));
        ++count;
    }
    return count;
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

QImage AlignToReference::GrayMatToQImage(const cv::Mat& gray) const
{
    QImage out(gray.cols, gray.rows, QImage::Format_Grayscale8);
    for (int y = 0; y < gray.rows; ++y) {
        memcpy(out.scanLine(y), gray.ptr(y), gray.cols);
    }
    return out;
}
