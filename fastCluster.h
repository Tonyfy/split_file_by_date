/*
��Ŀ¼�µĴ���ͼƬ �����������ڽ��з��࣬������Ĵ�����ȻҲ�ܰ����ļ��������ڵȽ��з��ࡣ

Author��zhao
Date��2016��1��4�� 20:34:56
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
