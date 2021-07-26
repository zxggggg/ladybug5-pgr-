

#include "stdafx.h"

#ifdef _WIN32

#include <windows.h>

#else

#include <unistd.h>

#endif

#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ladybug.h>
#include <ladybuggeom.h>
#include <ladybugstream.h>
#include <ladybugrenderer.h>
#include <ladybugGPS.h>
#include <ladybugvideo.h>

using namespace std;

bool isHighBitDepth(LadybugDataFormat format)
{
	return (format == LADYBUG_DATAFORMAT_RAW12 ||
		format == LADYBUG_DATAFORMAT_HALF_HEIGHT_RAW12 ||
		format == LADYBUG_DATAFORMAT_COLOR_SEP_JPEG12 ||
		format == LADYBUG_DATAFORMAT_COLOR_SEP_HALF_HEIGHT_JPEG12 ||
		format == LADYBUG_DATAFORMAT_RAW16 ||
		format == LADYBUG_DATAFORMAT_HALF_HEIGHT_RAW16);
}


//=============================================================================
// Global variables
//=============================================================================
unsigned int iFrameFrom = 0;
unsigned int iFrameTo = 0;
string pszInputStream = ".\\LadybugImg-000000.pgr";  /////输入的pgr文件
string pszConfigFile = ".\\CameraConfig.cal";
int iOutputImageWidth = 1616;
int iOutputImageHeight = 1232;
//int iOutputImageWidth = 2048;
//int iOutputImageHeight = 1024;
LadybugOutputImage outputImageType = LADYBUG_PANORAMIC;
LadybugSaveFileFormat outputImageFormat = LADYBUG_FILEFORMAT_JPG;
LadybugColorProcessingMethod colorProcessingMethod = LADYBUG_HQLINEAR;
int iBlendingWidth = 100;
float fFalloffCorrectionValue = 1.0f; 
bool bFalloffCorrectionFlagOn = false;
bool bEnableAntiAliasing = false;
bool bEnableSoftwareRendering = false;
bool bEnableStabilization = false;
LadybugStabilizationParams stabilizationParams = { 6, 100, 0.95 };
LadybugContext context;
LadybugStreamContext readContext;
LadybugStreamHeadInfo streamHeaderInfo;
unsigned int iTextureWidth, iTextureHeight;
unsigned char* arpTextureBuffers[LADYBUG_NUM_CAMERAS] = { NULL, NULL, NULL, NULL, NULL, NULL };
float fFOV = 60.0f;
float fRotX = 0.0f;
float fRotY = 0.0f;
float fRotZ = 0.0f;
int iBitRate = 4000; // in kbps
bool processH264 = false;

//=============================================================================
// Macro Definitions   //宏定义
//=============================================================================
#define _CHECK_ERROR \
    if( error != LADYBUG_OK ) \
{ \
    return error; \
} \

#define _ON_ERROR_BREAK \
    if( error != LADYBUG_OK ) \
{ \
    printf( "Error! Ladybug library reported %s\n", \
    ::ladybugErrorToString( error ) ); \
    break; \
} \

#define _ON_ERROR_CONTINUE \
    if( error != LADYBUG_OK ) \
{ \
    printf( "Error! Ladybug library reported %s\n", \
    ::ladybugErrorToString( error ) ); \
    continue; \
} \

#define _ON_ERROR_EXIT \
    if( error != LADYBUG_OK ) \
{ \
    printf( "Error! Ladybug library reported %s\n", \
    ::ladybugErrorToString( error ) ); \
    cleanupLadybug(); \
    return 0; \
} \


LadybugError initializeLadybug(void)
{
	LadybugError error;
	LadybugImage image;
	char pszTempPath[_MAX_PATH] = { 0 };
	//
	// Create contexts and prepare stream for reading  //创建上下文并准备流以供阅读
	//
	error = ladybugCreateContext(&context);
	_CHECK_ERROR;

	error = ladybugCreateStreamContext(&readContext);
	_CHECK_ERROR;

	error = ladybugInitializeStreamForReading(readContext, pszInputStream.c_str());
	_CHECK_ERROR;

	// Is configuration file specified by the command line option?  //配置文件是否由命令行选项指定？
	if (strlen(pszConfigFile.c_str()) == 0) {
#if _MSC_VER >= 1400 // Is this newer than Visual C++ 8.0?
		char* tempFile = ::_tempnam(NULL, NULL);
		if (tempFile == NULL)
		{
			printf("Error creating temporary file name.\n");
			return LADYBUG_FAILED;
		}
		else
		{
			strncpy(pszTempPath, tempFile, _MAX_PATH);
			free(tempFile);
		}

#else
		strcpy(pszTempPath, "temp.cal");
#endif

		error = ladybugGetStreamConfigFile(readContext, pszTempPath);
		_CHECK_ERROR;
		pszConfigFile = pszTempPath;
		//strncpy(pszConfigFile, pszTempPath, strlen(pszTempPath));//复制
	}

	//
	// Load configuration file   //加载配置文件
	// 
	error = ladybugLoadConfig(context, pszConfigFile.c_str());
	_CHECK_ERROR;

	if (pszTempPath != NULL)
	{
		// Remove the temporary configuration file   //删除临时配置文件
		remove(pszTempPath);
	}

	//
	// Get and display the the stream information  获取并显示流信息
	//
	error = ladybugGetStreamHeader(readContext, &streamHeaderInfo);
	_CHECK_ERROR;

	const float frameRateToUse = streamHeaderInfo.ulLadybugStreamVersion < 7 ? (float)streamHeaderInfo.ulFrameRate : streamHeaderInfo.frameRate;

	printf("--- Stream Information ---\n");
	printf("Stream version : %d\n", streamHeaderInfo.ulLadybugStreamVersion);
	printf("Base S/N: %d\n", streamHeaderInfo.serialBase);
	printf("Head S/N: %d\n", streamHeaderInfo.serialHead);
	printf("Frame rate : %3.2f\n", frameRateToUse);
	printf("--------------------------\n");

	//
	// Set color processing method.
	//
	printf("Setting debayering method...\n");
	error = ladybugSetColorProcessingMethod(context, colorProcessingMethod);
	_CHECK_ERROR;

	// 
	// Set falloff correction value and flag
	//
	error = ladybugSetFalloffCorrectionAttenuation(context, fFalloffCorrectionValue);
	_CHECK_ERROR;
	error = ladybugSetFalloffCorrectionFlag(context, bFalloffCorrectionFlagOn);
	_CHECK_ERROR;

	//
	// read one image from the stream
	//
	error = ladybugReadImageFromStream(readContext, &image);
	_CHECK_ERROR;

	//
	// Allocate the texture buffers that hold the color-processed images for all cameras
	//
	if (colorProcessingMethod == LADYBUG_DOWNSAMPLE4 || colorProcessingMethod == LADYBUG_MONO)
	{
		iTextureWidth = image.uiCols / 2;
		iTextureHeight = image.uiRows / 2;
	}
	else
	{
		iTextureWidth = image.uiCols;
		iTextureHeight = image.uiRows;
	}

	const unsigned int outputBytesPerPixel = isHighBitDepth(streamHeaderInfo.dataFormat) ? 2 : 1;
	for (int i = 0; i < LADYBUG_NUM_CAMERAS; i++)
	{
		arpTextureBuffers[i] = new unsigned char[iTextureWidth * iTextureHeight * 4 * outputBytesPerPixel];
	}

	//
	// Set blending width
	//
	error = ladybugSetBlendingParams(context, iBlendingWidth);
	_CHECK_ERROR;

	//
	// Initialize alpha mask size - this can take a long time if the
	// masks are not present in the current directory.
	//
	printf("Initializing alpha masks (this may take some time)...\n");
	error = ladybugInitializeAlphaMasks(context, iTextureWidth, iTextureHeight);
	_CHECK_ERROR;

	// 
	// Make the rendering engine use the alpha mask
	//
	error = ladybugSetAlphaMasking(context, true);
	_CHECK_ERROR;

	//
	// Enable image sampling anti-aliasing
	//
	if (bEnableAntiAliasing)
	{
		error = ladybugSetAntiAliasing(context, true);
		_CHECK_ERROR;
	}

	//
	// Use ladybugEnableSoftwareRendering() to enable 
	// Ladybug library to render the off-screen image using a bitmap buffer 
	// in system memory. The image rendering process will not be hardware 
	// accelerated.
	//
	if (bEnableSoftwareRendering)
	{
		error = ladybugEnableSoftwareRendering(context, true);
		_CHECK_ERROR;
	}

	if (bEnableStabilization)
	{
		error = ladybugEnableImageStabilization(
			context, bEnableStabilization, &stabilizationParams);
		_CHECK_ERROR;
	}

	//
	// Configure output images in Ladybug liabrary
	//
	printf("Configure output images in Ladybug library...\n");
	error = ladybugConfigureOutputImages(
		context,
		outputImageType);
	_CHECK_ERROR;

	printf("Set off-screen panoramic image size:%dx%d image.\n", iOutputImageWidth, iOutputImageHeight);
	error = ladybugSetOffScreenImageSize(
		context,
		outputImageType,
		iOutputImageWidth,
		iOutputImageHeight);
	_CHECK_ERROR;

	error = ladybugSetSphericalViewParams(
		context,
		fFOV,
		fRotX * 3.14159265f / 180.0f,
		fRotY * 3.14159265f / 180.0f,
		fRotZ * 3.14159265f / 180.0f,
		0.0f,
		0.0f,
		0.0f);
	_CHECK_ERROR;


	return LADYBUG_OK;
}


string int2str(int n) {

	char t[24];
	int i = 0;

	while (n) {
		t[i++] = (n % 10) + '0';
		n /= 10;
	}
	t[i] = 0;

	return string(strrev(t));
}


//
// The Main
//
int main(void)
{
	LadybugError error;

	error = initializeLadybug();//////////激活Ladybug，打开流文件，获取此流中的图像总数，初始内容，加载配置文件temp.cal，初始化 LadybugImage影像尺寸 1616x1232

	/////////生成外参,获取x0,y0,f
	////写入文件
	string ExPara = ".\\ExPara.txt";
	ofstream outf(ExPara);
	string outpathInitParam = ".\\InitParam.txt";
	ofstream fpInitParam(outpathInitParam);
	fpInitParam << "#id x0rectified y0rectified frectified" << endl;

	////////////////////////////输出
	for (int i = 0; i < 6; i++)
	{
		double *Ex = new double[6];
		ladybugGetCameraUnitExtrinsics(context, i, Ex);///获取外参  获取指定相机单元的 6-D 外部向量。
		outf << Ex[0] << "   " << Ex[1] << "   " << Ex[2] << "   " << Ex[3] << "   " << Ex[4] << "   " << Ex[5] << std::endl;
		cout << Ex[0] << "   " << Ex[1] << "   " << Ex[2] << "   " << Ex[3] << "   " << Ex[4] << "   " << Ex[5] << std::endl;
		delete[] Ex;

		///////获取x0,y0,f
		double x0rectified, y0rectified, frectified;
		double x0distorted, y0distorted;
		ladybugGetCameraUnitImageCenter(context, i, &x0rectified, &y0rectified);////获取指定相机单元的校正图像中心。
		ladybugGetCameraUnitFocalLength(context, i, &frectified); ///获取指定相机单元的焦距（以像素为单位）。
		ladybugUnrectifyPixel(context, i, y0rectified, x0rectified, &y0distorted, &x0distorted);////找到校正后对应的图像中心在矫正前的对应点

		fpInitParam << i << " " << x0rectified << " " << y0rectified << " " << frectified << endl;
		cout << i << " " << x0rectified << " " << y0rectified << " " << frectified << endl;
	}

	outf.close();
	fpInitParam.close();



	////通过ladybugRectifyPixel函数读取畸变影像下一点(i,j)对应的纠正后坐标
	//////////////对于0-5相机镜头，获取每个镜头对应的纠正前后对应点坐标
	for (int CamID = 0; CamID < 6; CamID++)
	{
		////////////////打开2个txt文件，分别写入U2D和D2R的坐标

		//////读取文件
		//char c=(char)(CamID+1);
		string str = int2str(CamID);
		string outpathD2U = ".\\D2U_Cam";
		outpathD2U += str;
		outpathD2U += "_1616X1232.txt";
		ofstream fpD2U(outpathD2U);
		//fpD2U.open(outpathD2U.c_str());
		fpD2U << "width/cols = 1616 , height/rows = 1232" << endl;

		string outpathU2D = ".\\U2D_Cam";
		outpathU2D += str;
		outpathU2D += "_1616X1232.txt";
		ofstream fpU2D(outpathU2D);
		//fpU2D.open(outpathU2D.c_str());
		fpU2D << "width/cols = 1616 , height/rows = 1232" << endl;

		int Pidx = 0;  //计数器
		for (int i = 0; i < 1232; i = i + 1) ///height
		{
			for (int j = 0; j < 1616; j = j + 1) ///width
			{
				double dRectifiedx, dRectifiedy;
				ladybugRectifyPixel(context, CamID, i, j, &dRectifiedy, &dRectifiedx);  //////读取畸变影像下一点(i,j)对应的纠正后坐标，D2U
				fpD2U << i << " " << j << " " << dRectifiedy << " " << dRectifiedx << endl;

				double dDistortedx, dDistortedy;
				ladybugUnrectifyPixel(context, CamID, i, j, &dDistortedy, &dDistortedx);  //////读取畸变影像下一点(i,j)对应的纠正后坐标，D2U
				fpU2D << i << " " << j << " " << dDistortedy << " " << dDistortedx << endl;
				Pidx++;
			}///end j
		}///end i

		fpD2U.close();
		fpU2D.close();

	}////0-5相机全部获取完毕

	return 0;
}

