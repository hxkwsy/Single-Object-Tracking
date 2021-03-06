// BpKalmanFusionTracking.cpp: 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include <opencv2/opencv.hpp>
#include <opencv2/core/operations.hpp>
#include <iostream>
#include <ctype.h>
#include <vector>
#include <fstream>
#include <stdlib.h>

using namespace cv;
using namespace std;
using namespace ml;

Mat image;
bool backprojMode = false;
bool selectObject = false;
int trackObject = 0;
bool showHist = true;
Point origin;
Rect selection;
int vmin = 10, vmax = 256, smin = 30;
RotatedRect trackBox;

//计算搜索区直方图
int hsize2 = 16;
float hranges[] = { 0, 180 };
const float* phranges2 = hranges;
Mat histimg2 = Mat::zeros(200, 320, CV_8UC3);

float Threshold = 0.6;

//定义vector，存储点坐标
int point_num = 0;
vector<Point2f> vec;
Mat trainingData_In, trainingData_Out;

//神经网络
Ptr<ANN_MLP> ann;
Mat NN_result;
Point2f nn_point;

// 卡尔曼
RNG rng;
const float T = 0.9;        //采样周期
float v_x = 1.0, v_y = 1.0, a_x = 0.1, a_y = 0.1;
const int stateNum = 6;		//状态数，包括（x, y, dx, dy, d(dx), d(dy)）坐标、速度（每次移动的距离）、加速度
const int measureNum = 2;   //测量值2×1向量(x,y)
KalmanFilter KF( stateNum, measureNum, 0 );
Mat measurement = Mat::zeros( measureNum, 1, CV_32F );
Point2f pre_point;    //神经网络+Kalman的最优估计点

void diff_Mat( Mat in_Image, Mat out_Image );
Point2f neural_networks( Mat in_trainData, Mat out_trainData );
Point2f kalman_filter( Point2f nn_point );

/***************鼠标操作函数************************/
static void onMouse(int event, int x, int y, int, void*)
{
	if (selectObject)
	{
		selection.x = MIN(x, origin.x);
		selection.y = MIN(y, origin.y);
		selection.width = std::abs(x - origin.x);
		selection.height = std::abs(y - origin.y);

		selection &= Rect(0, 0, image.cols, image.rows);
	}

	switch (event)
	{
	case CV_EVENT_LBUTTONDOWN:
		origin = Point(x, y);
		selection = Rect(x, y, 0, 0);
		selectObject = true;
		break;
	case CV_EVENT_LBUTTONUP:
		selectObject = false;
		if (selection.width > 0 && selection.height > 0)
			trackObject = -1;
		break;
	}
}

const char* keys =
{
	"{1|  | 0 | camera number}"
};

int main(int argc, const char** argv)
{
	VideoCapture cap;
	//cap.open( 0 );
	//cap.open("E:/视频库/soccer.avi");	//测试库
	cap.open("C:/Users/18016/Desktop/ObjectTracking/learnopencv-master/tracking/videos/chaplin.mp4");	//测试库

	Rect trackWindow, pre_trackWindow;
	int hsize = 16;
	float hranges[] = { 0, 180 };
	const float* phranges = hranges;
	CommandLineParser parser(argc, argv, keys);

	namedWindow("CamShift Demo", 0);
	setMouseCallback("CamShift Demo", onMouse, 0);
	createTrackbar("Vmin", "CamShift Demo", &vmin, 256, 0);
	createTrackbar("Vmax", "CamShift Demo", &vmax, 256, 0);
	createTrackbar("Smin", "CamShift Demo", &smin, 256, 0);

	trainingData_In = Mat( 3, 4, CV_32FC1 );
	trainingData_Out = Mat( 3, 2, CV_32FC1 );

	// 构建神经网络
	Mat layerSizes = ( Mat_<int>( 1, 3 ) << 4, 9, 2 );
	ann = ANN_MLP::create( );
	ann->setLayerSizes( layerSizes );
	ann->setActivationFunction( ANN_MLP::SIGMOID_SYM, 1, 1 );
	ann->setTermCriteria( TermCriteria( TermCriteria::MAX_ITER + TermCriteria::EPS, 1000, FLT_EPSILON ) );
	ann->setTrainMethod( ANN_MLP::BACKPROP, 0.1, 0.1 );

	// 卡尔曼滤波器初始化
	KF.transitionMatrix = ( Mat_<float>( 6, 6 ) << 1, 0, T, 0, ( T*T ) / 2, 0,
		                                           0, 1, 0, T, 0, ( T*T ) / 2,
		                                           0, 0, 1, 0, T, 0,
		                                           0, 0, 0, 1, 0, T,
		                                           0, 0, 0, 0, 1, 0,
		                                           0, 0, 0, 0, 0, 1 );

	setIdentity( KF.measurementMatrix );						//测量矩阵H  
	setIdentity( KF.processNoiseCov, Scalar::all( 1e-5 ) );		//系统噪声方差矩阵Q    
	setIdentity( KF.measurementNoiseCov, Scalar::all( 1e-1 ) );	//测量噪声方差矩阵R    
	setIdentity( KF.errorCovPost, Scalar::all( 1 ) );			//P(k)

	// 初始值
	KF.statePost = ( Mat_<float>( 6, 1 ) << 0, 0, v_x, v_y, a_x, a_y );

	Mat frame, hsv, hue, mask, hist, histimg = Mat::zeros(200, 320, CV_8UC3), backproj;
	bool paused = false;

	for (;; ){
		if (!paused){
			cap >> frame;
			if (frame.empty())
				break;
		}

		frame.copyTo(image);

		if (!paused){
			cvtColor(image, hsv, COLOR_BGR2HSV);

			if (trackObject){
				int _vmin = vmin, _vmax = vmax;

				inRange(hsv, Scalar(0, smin, MIN(_vmin, _vmax)), Scalar(180, 256, MAX(_vmin, _vmax)), mask);

				int ch[] = { 0, 0 };
				hue.create(hsv.size(), hsv.depth());
				mixChannels(&hsv, 1, &hue, 1, ch, 1);

				// 构建目标模型的颜色直方图
				if (trackObject < 0){
					Mat roi(hue, selection), maskroi(mask, selection);
					calcHist(&roi, 1, 0, maskroi, hist, 1, &hsize, &phranges);
					normalize(hist, hist, 0, 255, CV_MINMAX);

					trackWindow = selection;
					trackObject = 1;

					histimg = Scalar::all(0);
					int binW = histimg.cols / hsize;
					Mat buf(1, hsize, CV_8UC3);
					for (int i = 0; i < hsize; i++)
						buf.at<Vec3b>(i) = Vec3b(saturate_cast<uchar>(i*180. / hsize), 255, 255);
					cvtColor(buf, buf, CV_HSV2BGR);

					for (int i = 0; i < hsize; i++){
						int val = saturate_cast<int>(hist.at<float>(i)*histimg.rows / 255);
						rectangle(histimg, Point(i*binW, histimg.rows), Point((i + 1)*binW, histimg.rows - val), Scalar(buf.at<Vec3b>(i)), -1, 8);
					}
				}
				//-------------------------------------------------------------------
				// 构建搜索窗的颜色直方图
				//-------------------------------------------------------------------
				Mat hist2;
				Mat roi_rect(hue, trackWindow), maskroi_rect(mask, trackWindow);
				calcHist(&roi_rect, 1, 0, maskroi_rect, hist2, 1, &hsize2, &phranges2);
				normalize(hist2, hist2, 0, 255, CV_MINMAX);

				histimg2 = Scalar::all(0);
				int binW2 = histimg2.cols / hsize2;
				Mat buf2(1, hsize2, CV_8UC3);
				for (int i = 0; i < hsize2; i++)
					buf2.at<Vec3b>(i) = Vec3b(saturate_cast<uchar>(i*180. / hsize2), 255, 255);
				cvtColor(buf2, buf2, CV_HSV2BGR);

				for (int i = 0; i < hsize2; i++){
					int val = saturate_cast<int>(hist2.at<float>(i)*histimg2.rows / 255);
					rectangle(histimg2, Point(i*binW2, histimg2.rows), Point((i + 1)*binW2, histimg2.rows - val), Scalar(buf2.at<Vec3b>(i)), -1, 8);
				}
				//imshow("目标模型的颜色直方图", histimg );
				//imshow("搜索窗口的颜色直方图", histimg2);
				//---------------------------------------------------------------------
				// 用巴氏系数计算两直方图的相似性
				//---------------------------------------------------------------------
				double dSimilarity = compareHist(hist, hist2, CV_COMP_BHATTACHARYYA);
				cout << "similarity = " << dSimilarity << endl;

				//---------------------------------------------------------------------
				//当巴氏系数小于阈值S时，未发生遮挡
				//---------------------------------------------------------------------
				if ( dSimilarity < Threshold) {
					calcBackProject( &hue, 1, 0, hist, backproj, &phranges );
					backproj &= mask;

					trackBox = CamShift( backproj, trackWindow, TermCriteria( CV_TERMCRIT_EPS | CV_TERMCRIT_ITER, 10, 1 ) );

					if ( trackWindow.area( ) <= 1 ) {
						int cols = backproj.cols, rows = backproj.rows, r = ( MIN( cols, rows ) + 5 ) / 6;
						trackWindow = Rect( trackWindow.x - r, trackWindow.y - r, trackWindow.x + r, trackWindow.y + r ) & Rect( 0, 0, cols, rows );
					}
					if ( backprojMode )
						cvtColor( backproj, image, COLOR_GRAY2BGR );

					// 跟踪的时候以椭圆为代表目标
					ellipse( image, trackBox, Scalar( 0, 0, 255 ), 3, CV_AA );
					circle( image, trackBox.center, 3, Scalar( 0, 0, 255 ), 3 );

					// 跟踪的时候以矩形框为代表
					//Point2f vertex[4];
					//trackBox.points(vertex);
					//for (int i = 0; i < 4; i++)
					//	line(image, vertex[i], vertex[(i + 1) % 4], Scalar(0, 0, 255), 2, 8, 0);

				}
				//---------------------------------------------------------------------
				//当巴氏系数于阈值S时，目标发生遮挡，以预测位置代替真实位置进行跟踪
				//---------------------------------------------------------------------
				else {
					int x = pre_point.x;
					int y = pre_point.y;
					int w = selection.width;
					int h = selection.height;

					pre_trackWindow = Rect( x - w / 2, y - h / 2, w, h );
					calcBackProject( &hue, 1, 0, hist2, backproj, &phranges );
					backproj &= mask;

					trackBox = CamShift( backproj, pre_trackWindow, TermCriteria( CV_TERMCRIT_EPS | CV_TERMCRIT_ITER, 10, 1 ) );
					ellipse( image, trackBox, Scalar( 0, 255, 255 ), 3, CV_AA );

					// 更新搜索窗
					trackWindow = pre_trackWindow;

				}

				//神经网络+Kalman预测	
				point_num++;
				vec.push_back( trackBox.center );
				if ( point_num >= 6 ) {
					diff_Mat( trainingData_In, trainingData_Out );
					nn_point = neural_networks( trainingData_In, trainingData_Out );
					pre_point = kalman_filter( nn_point );

					vec.erase( vec.begin( ) );  // 删除容器中的第一个点
				}
			}
		}
		else if (trackObject < 0)
			paused = false;

		if (selectObject && selection.width > 0 && selection.height > 0){
			Mat roi(image, selection);
			bitwise_not(roi, roi);
		}
		imshow("CamShift Demo", image);

		char c = (char)waitKey(30);
		if (c == 27) break;

		switch (c)
		{
		case 'b':
			backprojMode = !backprojMode;
			break;
		case 'c':
			trackObject = 0;
			histimg = Scalar::all(0);
			break;
		case 'h':
			showHist = !showHist;
			if (!showHist)
				destroyWindow("Histogram");
			else
				namedWindow("Histogram", 1);
			break;
		case 'p':
			paused = !paused;
			break;
		default:
			;
		}
	}
	return 0;
}

/**
 * 构建差值矩阵
 **/
void diff_Mat( Mat in_Image, Mat out_Image ) {

	int count = 0;
	vector<Point2f> in_diff;
	vector<Point2f> out_diff;

	// vec中存储的点坐标（六对）
	//for ( vector<Point2f>::iterator it = vec.begin( ); it != vec.end( ); it++ ) {
	//	cout << *it << endl;
	//}
	cout << endl;
	// 构建输入样本差值矩阵(三行四列，共四对差值，由五对点坐标产生，其中每行两对差值)
	for ( vector<Point2f>::iterator it = vec.begin( ); it != vec.end( ) - 2; it++ ) {
		float dete_x = ( *( it + 1 ) ).x - ( *it ).x;
		float dete_y = ( *( it + 1 ) ).y - ( *it ).y;
		in_diff.push_back( Point2f( dete_x, dete_y ) );
		if ( count > 0 && count < 4 )
			in_diff.push_back( Point2f( dete_x, dete_y ) );
		count++;
	}
	// vector转换到Mat---浅拷贝
	//Mat test_Image = Mat( 3, 4, CV_32FC1, (float*)in_diff.data( ) );
	// vector转换到Mat---深拷贝
	memcpy( in_Image.data, in_diff.data( ), 2 * in_diff.size( ) * sizeof( float ) );
	//cout << endl << "输入差值矩阵：" << endl << in_Image << endl << endl;

	//构建输出样本差值矩阵(三行两列，共三对差值，由四对点坐标产生，其中每行一对差值)
	for ( vector<Point2f>::iterator it = vec.begin( ) + 2; it != vec.end( ) - 1; it++ ) {
		float dete_x = ( *( it + 1 ) ).x - ( *it ).x;
		float dete_y = ( *( it + 1 ) ).y - ( *it ).y;
		out_diff.push_back( Point2f( dete_x, dete_y ) );
	}
	//out_Image = Mat( 3, 2, CV_32FC1, (float*)out_diff.data( ) );
	memcpy( out_Image.data, out_diff.data( ), 2 * out_diff.size( ) * sizeof( float ) );
	//cout << endl << "输出差值矩阵：" << endl << out_Image << endl << endl;

}

/**
 * 神经网络训练及预测
 **/
Point2f neural_networks( Mat in_trainData, Mat out_trainData ) {
	// 创建训练数据，由构建的差值矩阵按行训练，也就是每次输入两对差值，输出一对差值，即
	Ptr<TrainData> tData = TrainData::create( in_trainData, ROW_SAMPLE, out_trainData );
	ann->train( tData );

	// 构建当前预测差值矩阵
	vector<Point2f> pre_diff;
	for ( vector<Point2f>::iterator it = vec.begin( ) + 3; it != vec.end( ) - 1; it++ ) {
		//cout << *it << endl;
		float dete_x = ( *( it + 1 ) ).x - ( *it ).x;
		float dete_y = ( *( it + 1 ) ).y - ( *it ).y;
		pre_diff.push_back( Point2f( dete_x, dete_y ) );
	}
	Mat sampleMat = Mat( 1, 4, CV_32FC1, (float*)pre_diff.data( ) );
	Mat predictMat = Mat( 1, 2, CV_32FC1 );
	ann->predict( sampleMat, predictMat );
	//cout << endl << "神经网络预测差值矩阵：" << endl << sampleMat << endl;
	//cout << endl << "神经网络预测结果矩阵：" << endl << predictMat << endl;
	Point2f pre_point4;
	Point2f pre_point1 = vec.at( 3 );
	Point2f pre_point2 = vec.at( 4 );
	Point2f pre_point3 = vec.at( 5 );
	// 神经网络预测的下一时刻的点坐标，保留到小数点后一位
	pre_point4.x = vec.back( ).x + floor( predictMat.at<float>( 0 ) * 10 + 0.5 ) / 10; 
	pre_point4.y = vec.back( ).y + floor( predictMat.at<float>( 0 ) * 10 + 0.5 ) / 10;
	//cout << endl << "pre_point1: " << pre_point1 << endl;
	//cout << "pre_point2: " << pre_point2 << endl;
	//cout << "pre_point3: " << pre_point3 << endl;
	//circle( image, pre_point4, 3, Scalar( 255, 0, 0 ), 3 );
	cout << "神经网络预测值pre_point = " << pre_point4 << endl;

	return pre_point4;
}

/**
 * 卡尔曼预测
 **/
Point2f kalman_filter( Point2f nn_point ) {
	KF.statePost.at<float>( 0 ) = vec.back().x;
	KF.statePost.at<float>( 1 ) = vec.back( ).y;
	//cout << "statePost=" << KF.statePost << endl << endl;

	// 状态方程计算  
	Point2f klm_point;
	Mat prediction = KF.predict( );
	klm_point.x = floor( prediction.at<float>( 0 ) * 10 + 0.5 ) / 10;
	klm_point.y = floor( prediction.at<float>( 1 ) * 10 + 0.5 ) / 10;
	cout << "状态方程计算值klm_point = " << klm_point << endl;
	circle( image, klm_point, 3, Scalar( 0, 255, 0 ), 3 );
	//计算测量值 = 神经网络预测值  
	measurement.at<float>( 0 ) = (float)nn_point.x;
	measurement.at<float>( 1 ) = (float)nn_point.y;

	//更新---最优估计值
	Point2f cor_point;
	KF.correct( measurement );
	cor_point.x = floor( KF.statePost.at<float>( 0 ) * 10 + 0.5 ) / 10;
	cor_point.y = floor( KF.statePost.at<float>( 1 ) * 10 + 0.5 ) / 10;
	//输出结果  
	circle( image, cor_point, 3, Scalar( 255, 255, 255 ), 3 );
	cout << "融合最优估计值cor_point = " << cor_point << endl << endl;

	return cor_point;
}
