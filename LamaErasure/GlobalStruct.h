#ifndef GLOBAL_STRUCT_H
#define GLOBAL_STRUCT_H

struct MaskParams
{
	int min_r;									// 最小半径
	int max_r;									// 最大半径
	int mask_num;								// 生成的mask个数
	double area;								// mask面积
	double circularity;							// 圆度
	int marginPx;								// 膨胀
};

#endif