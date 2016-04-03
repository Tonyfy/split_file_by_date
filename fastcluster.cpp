#include <iostream>
#include <fstream>

using namespace std;

bool comppics(picture x, picture y)
{

	for (int i = 0; i < x.date.size();)
	{
		if (x.date.at(i) == y.date.at(i))
		{
			i++;
		}
		else
		{
			return x.date.at(i) < y.date.at(i);
		}
	}
	return false;
}

void regressionsplit(picture& pic1,picture& pic2, int& i,int s, picsInoneTime& tmp, 
	std::vector<picsInoneTime>& picsOT)
{
	while (i < s + 1)
	{
		if (pic2.date.at(i) != pic1.date.at(i))
		{
			picsOT.push_back(tmp);
			tmp.pic.clear();
			tmp.pic.push_back(pic2);
			i += s + 1;
		}
		else
		{
			i++;
			regressionsplit(pic1, pic2, i, s, tmp, picsOT);
			if (i == s + 1)
			{
				tmp.pic.push_back(pic2);
				i += s + 1;
				break;
			}
		}
	}

}
void splitpicsOntime(std::vector<picture>& pics, int rule, 
	std::vector<picsInoneTime> & picsOT)
{
	assert((rule == 0) || (rule == 1) || (rule == 2) 
		|| (rule == 3) || (rule == 4) || (rule == 5));
	picsOT.clear();
	if (pics.size() < 2)
	{
		picsInoneTime tmp;
		tmp.pic.push_back(pics[0]);
		picsOT.push_back(tmp);
	}
	sort(pics.begin(), pics.end(), comppics);

	picsInoneTime tmp;
	tmp.pic.push_back(pics[0]);
	for (int i = 1; i < pics.size(); i++)
	{
		int ss = 0;
		regressionsplit(pics[i - 1], pics[i], ss, rule, tmp, picsOT);
	}
	picsOT.push_back(tmp);

}
