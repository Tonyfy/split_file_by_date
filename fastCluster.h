/*
将目录下的大量图片 按照拍摄日期进行分类，做后序的处理，当然也能按照文件创建日期等进行分类。

Author：zhao
Date：2016年1月4日 20:34:56
*/
#include <iostream>
#include <vector>
#include <array>

struct picture
{
	std::array<int, 6> date;
	std::string filepath;
	int orien;   //see getImgOrientation() in exif.h about the shooting angle type;
	std::string filename;
};

struct picsInoneTime
{
	std::vector<picture> pic;
};

bool comppics(picture x,picture y);

void splitpicsOntime(std::vector<picture>& pics,int rule, std::vector<picsInoneTime>& picsOT);

void regressionsplit(picture& pic1, picture& pic2,int& i, int s, picsInoneTime& tmp, std::vector<picsInoneTime> & picsOT);
