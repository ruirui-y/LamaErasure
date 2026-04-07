#ifndef LAMA_ORT_H
#define LAMA_ORT_H

#include <onnxruntime_cxx_api.h>
#include <QImage>
#include <array>
#include <string>
#include <vector>
#include <memory>
#include <QObject>
#include "Global.h"

class LamaOrt : public QObject
{
    Q_OBJECT

public:

    using Ptr = QSharedPointer<LamaOrt>;
    explicit LamaOrt(QObject* parenr = 0);
    QImage Run(const QImage& src, const QImage& mask);

public slots:
    void RunAsync(const QImage& src, const QImage& mask, ImageCallback cb);

private:
    static constexpr int kSize = 512;

    static inline float Clamp01(float v);
private:
    QImage processCore(const QImage& src, const QImage& mask);
    QImage ToRgb32(const QImage& img);
    QImage ToGray8(const QImage& img);

    QImage ScaleIgnoreAspect(const QImage& img, int w, int h, Qt::TransformationMode mode);

    void BinarizeInPlace(QImage& gray8, int thresh = 127);                                      // 图像二值化
    QImage DilateGray8(const QImage& binGray8, int radius);                                     // 图像膨胀

    void MaskToFloatHW(const QImage& binGray8, std::vector<float>& outHW);                      // 转换成浮点数组
    void ImageToFloatCHW_HoleZero01(const QImage& rgb32_512, 
        const std::vector<float>& maskHW,
        std::vector<float>& outCHW);                                                            // 0..1，洞区置0

    std::vector<Ort::Value> RunOrt(const std::vector<float>& imageCHW,
        const std::vector<float>& maskHW,
        int w, int h);

    bool OutputIs01(const float* out, size_t n);                                                // mx<=1.5
    QImage OutCHWToRgb32(const float* out, int w, int h, bool out01);

    QImage CompositeByMask(const QImage& baseRgb32,
        const QImage& fillRgb32,
        const QImage& binMaskGray8);                                                            // 255=洞

    QImage CompositeByMaskScaledUp(const QImage& baseOrigRgb32,
        const QImage& fill512Rgb32,
        const QImage& maskOrigGray8);                                                           // 固定512推理的最终合成（原尺寸）

private:
    Ort::Env env_;
    Ort::Session session_{ nullptr };

    // 用 string 管理内存，再把 c_str() 提供给 ORT
    std::vector<std::string> inputNameStr_;
    std::vector<std::string> outputNameStr_;
    std::vector<const char*> inputNames_;
    std::vector<const char*> outputNames_;

    std::wstring onnx_path = L"";
};

#endif // LAMA_ORT_H