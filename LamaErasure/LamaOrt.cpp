#include "LamaOrt.h"

#include "ConfigReader.h"
#include <algorithm>
#include <QDebug>

float LamaOrt::Clamp01(float v)
{
    return std::min(1.0f, std::max(0.0f, v));
}

LamaOrt::LamaOrt(QObject* parenr)
    : env_(ORT_LOGGING_LEVEL_WARNING, "lama")
{
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(1);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    onnx_path = ConfigReader::Instance()->paths().onnxPath.toStdWString();
    session_ = Ort::Session(env_, onnx_path.c_str(), so);

    // Netron: inputs: image, mask  outputs: output
    inputNameStr_ = { "image", "mask" };
    outputNameStr_ = { "output" };

    inputNames_.clear();
    for (auto& s : inputNameStr_) inputNames_.push_back(s.c_str());

    outputNames_.clear();
    for (auto& s : outputNameStr_) outputNames_.push_back(s.c_str());
}

QImage LamaOrt::ToRgb32(const QImage& img)
{
    return img.convertToFormat(QImage::Format_RGB32);
}

QImage LamaOrt::ToGray8(const QImage& img)
{
    return img.convertToFormat(QImage::Format_Grayscale8);
}

QImage LamaOrt::ScaleIgnoreAspect(const QImage& img, int w, int h, Qt::TransformationMode mode)
{
    return img.scaled(w, h, Qt::IgnoreAspectRatio, mode);
}

void LamaOrt::BinarizeInPlace(QImage& gray8, int thresh)
{
    const int W = gray8.width();
    const int H = gray8.height();
    for (int y = 0; y < H; ++y) 
    {
        uchar* p = gray8.scanLine(y);
        for (int x = 0; x < W; ++x) 
            p[x] = (p[x] > thresh) ? 255 : 0;
    }
}

// 检测3*3邻域(radius = 1)
QImage LamaOrt::DilateGray8(const QImage& binGray8, int radius)
{
    if (radius <= 0) return binGray8;

    const int W = binGray8.width();
    const int H = binGray8.height();
    QImage dst = binGray8.copy();

    for (int y = 0; y < H; ++y) 
    {
        uchar* drow = dst.scanLine(y);
        const int y0 = std::max(0, y - radius);
        const int y1 = std::min(H - 1, y + radius);

        for (int x = 0; x < W; ++x) 
        {
            uchar v = 0;
            const int x0 = std::max(0, x - radius);
            const int x1 = std::min(W - 1, x + radius);

            for (int yy = y0; yy <= y1 && v == 0; ++yy) 
            {
                const uchar* srow = binGray8.constScanLine(yy);
                for (int xx = x0; xx <= x1; ++xx)
                {
                    if (srow[xx] == 255) { v = 255; break; }
                }
            }
            drow[x] = v;
        }
    }
    return dst;
}

void LamaOrt::MaskToFloatHW(const QImage& binGray8, std::vector<float>& outHW)
{
    const int W = binGray8.width();
    const int H = binGray8.height();
    outHW.assign(size_t(W) * H, 0.0f);

    for (int y = 0; y < H; ++y) {
        const uchar* pm = binGray8.constScanLine(y);
        for (int x = 0; x < W; ++x) {
            outHW[size_t(y) * W + x] = (pm[x] == 255) ? 1.0f : 0.0f;
        }
    }
}

void LamaOrt::ImageToFloatCHW_HoleZero01(const QImage& rgb32_512,
    const std::vector<float>& maskHW,
    std::vector<float>& outCHW)
{
    const int W = rgb32_512.width();
    const int H = rgb32_512.height();
    outCHW.assign(size_t(3) * W * H, 0.0f);

    for (int y = 0; y < H; ++y) 
    {
        const QRgb* row = reinterpret_cast<const QRgb*>(rgb32_512.constScanLine(y));
        for (int x = 0; x < W; ++x) 
        {
            const int hw = y * W + x;
            const bool hole = (maskHW[size_t(hw)] > 0.5f);

            // 如果是洞，都设置成0
            const QRgb px = row[x];
            const float r = hole ? 0.0f : (float(qRed(px)) / 255.0f);
            const float g = hole ? 0.0f : (float(qGreen(px)) / 255.0f);
            const float b = hole ? 0.0f : (float(qBlue(px)) / 255.0f);

            outCHW[size_t(hw) + size_t(0) * W * H] = r;
            outCHW[size_t(hw) + size_t(1) * W * H] = g;
            outCHW[size_t(hw) + size_t(2) * W * H] = b;
        }
    }
}

std::vector<Ort::Value> LamaOrt::RunOrt(const std::vector<float>& imageCHW,
    const std::vector<float>& maskHW,
    int w, int h)
{
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

    std::array<int64_t, 4> imgShape{ 1, 3, h, w };
    std::array<int64_t, 4> mskShape{ 1, 1, h, w };

    Ort::Value imgTensor = Ort::Value::CreateTensor<float>(
        mem, const_cast<float*>(imageCHW.data()), imageCHW.size(), imgShape.data(), imgShape.size());

    Ort::Value mskTensor = Ort::Value::CreateTensor<float>(
        mem, const_cast<float*>(maskHW.data()), maskHW.size(), mskShape.data(), mskShape.size());

    std::array<Ort::Value, 2> inputs{ std::move(imgTensor), std::move(mskTensor) };

    try {
        return session_.Run(
            Ort::RunOptions{ nullptr },
            inputNames_.data(), inputs.data(), inputs.size(),
            outputNames_.data(), outputNames_.size()
        );
    }
    catch (const Ort::Exception& e) {
        qDebug() << "ORT Run exception:" << e.what();
        return {};
    }
}

bool LamaOrt::OutputIs01(const float* out, size_t n)
{
    float mx = out[0];
    for (size_t i = 1; i < n; ++i) mx = std::max(mx, out[i]);
    return (mx <= 1.5f);
}

QImage LamaOrt::OutCHWToRgb32(const float* out, int w, int h, bool out01)
{
    auto clampToU8 = [&](float v)->uchar {
        if (out01) v *= 255.0f;
        v = std::min(255.0f, std::max(0.0f, v));
        return (uchar)(v + 0.5f);
        };

    QImage outImg(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(outImg.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int hw = y * w + x;
            const uchar r = clampToU8(out[size_t(hw) + size_t(0) * w * h]);
            const uchar g = clampToU8(out[size_t(hw) + size_t(1) * w * h]);
            const uchar b = clampToU8(out[size_t(hw) + size_t(2) * w * h]);
            row[x] = qRgb(r, g, b);
        }
    }
    return outImg;
}

QImage LamaOrt::CompositeByMask(const QImage& baseRgb32,
    const QImage& fillRgb32,
    const QImage& binMaskGray8)
{
    const int W = baseRgb32.width();
    const int H = baseRgb32.height();

    QImage out(W, H, QImage::Format_RGB32);
    for (int y = 0; y < H; ++y) {
        const QRgb* bRow = reinterpret_cast<const QRgb*>(baseRgb32.constScanLine(y));
        const QRgb* fRow = reinterpret_cast<const QRgb*>(fillRgb32.constScanLine(y));
        const uchar* mRow = binMaskGray8.constScanLine(y);
        QRgb* oRow = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < W; ++x) {
            oRow[x] = (mRow[x] == 255) ? fRow[x] : bRow[x];
        }
    }
    return out;
}

QImage LamaOrt::CompositeByMaskScaledUp(const QImage& baseOrigRgb32,
    const QImage& fill512Rgb32,
    const QImage& maskOrigGray8)
{
    const int origW = baseOrigRgb32.width();
    const int origH = baseOrigRgb32.height();

    // 原尺寸 mask 二值化
    QImage mOrigBin = maskOrigGray8.convertToFormat(QImage::Format_Grayscale8);
    BinarizeInPlace(mOrigBin, 127);

    // 512结果放大回原尺寸
    QImage fillUp = ScaleIgnoreAspect(fill512Rgb32, origW, origH, Qt::SmoothTransformation);

    // 原尺寸合成（洞区用 fillUp，非洞区保留原图）
    return CompositeByMask(baseOrigRgb32, fillUp, mOrigBin);
}

QImage LamaOrt::processCore(const QImage& src, const QImage& mask)
{
    if (src.isNull() || mask.isNull()) return QImage();

    const int origW = src.width();
    const int origH = src.height();
    const int W = 512, H = 512;

    // 1) 原图统一格式（原尺寸）
    QImage srcOrig = ToRgb32(src);
    QImage mOrig = ToGray8(mask);

    // 2) 缩放到 512 作为推理输入
    QImage img512 = ScaleIgnoreAspect(srcOrig, W, H, Qt::SmoothTransformation);
    QImage m512 = ScaleIgnoreAspect(mOrig, W, H, Qt::FastTransformation);

    // 3) mask 二值化 + 轻微膨胀（覆盖边缘残留）
    BinarizeInPlace(m512, 127);
    const int dilateR = 4;
    if (dilateR > 0) m512 = DilateGray8(m512, dilateR);

    // 4) 准备输入张量
    std::vector<float> maskHW;
    MaskToFloatHW(m512, maskHW);

    std::vector<float> imageCHW;
    ImageToFloatCHW_HoleZero01(img512, maskHW, imageCHW);

    // 5) ORT 推理
    auto outputs = RunOrt(imageCHW, maskHW, W, H);
    if (outputs.empty()) return QImage();

    float* out = outputs[0].GetTensorMutableData<float>();
    const size_t N = size_t(3) * W * H;
    const bool out01 = OutputIs01(out, N);

    // 6) out(CHW) -> RGB32(512)
    QImage out512 = OutCHWToRgb32(out, W, H, out01);

    // 7) 先在 512 上合成（洞区用 out，非洞区用 img512）
    QImage final512 = CompositeByMask(img512, out512, m512);

    // 8) 还原原尺寸并用“原尺寸 mask”再合成一次
    // return CompositeByMaskScaledUp(srcOrig, final512, mOrig);

    // 将回调放在接受者线程中做操作

    // 8) 还原原尺寸
    QImage result = ScaleIgnoreAspect(final512, src.width(), src.height(), Qt::SmoothTransformation);
    return result;
}

QImage LamaOrt::Run(const QImage& src, const QImage& mask)
{

    return processCore(src, mask);
}

void LamaOrt::RunAsync(const QImage& src, const QImage& mask, ImageCallback cb)
{
    // 在当前线程执行擦除
    QImage result = processCore(src, mask);

    // 线程安全的回调分发
    QObject* context = sender();
    if (!context) context = this->parent();

    if (context && cb) 
    {
        QMetaObject::invokeMethod(context, [cb, result]() 
            {
                cb(result);
            });
    }
    else if (cb) 
    {
        cb(result);
    }
}