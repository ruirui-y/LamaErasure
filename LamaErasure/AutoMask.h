#ifndef AUTOMASK_H
#define AUTOMASK_H

#include <QObject>
#include <QImage>
#include <vector>
#include <array>

#include <opencv2/opencv.hpp>

#include "singletion.h"
#include "GlobalStruct.h"

class AutoMask  : public QObject, public Singleton<AutoMask>
{
	Q_OBJECT

public:
	friend class Singleton<AutoMask>;

public:
	AutoMask(QObject *parent = 0);
	~AutoMask();

private:
	struct CircleCand {
		cv::Point2f c;
		float r = 0.f;
		double score = 0.0; // 单个圆像贴纸的分
	};

	cv::Mat QImageToBgrMat(const QImage& img) const;
	QImage GrayMatToQImage(const cv::Mat& gray8) const;

public:
	QImage AutoMaskFromStickers(const QImage& srcRgb, MaskParams mask_params);
};

#endif // AUTOMASK_H