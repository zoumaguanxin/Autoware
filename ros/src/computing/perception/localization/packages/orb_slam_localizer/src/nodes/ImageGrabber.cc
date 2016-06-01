/*
 * ImageGrabber.cc
 *
 *  Created on: May 31, 2016
 *      Author: sujiwo
 */

#include "ImageGrabber.h"


using namespace std;
using ORB_SLAM2::Frame;
namespace enc = sensor_msgs::image_encodings;



ImageGrabber::ImageGrabber(ORB_SLAM2::System* pSLAM) :
	mpSLAM(pSLAM),
	doStop (false),
	doDebayer (false)
{
	// External localization
	extFrame1 = (string)mpSLAM->fsSettings["ExternalLocalization.frame1"];
	extFrame2 = (string)mpSLAM->fsSettings["ExternalLocalization.frame2"];
	cout << "External Reference: from " << extFrame1 << " to " << extFrame2 << endl;

	offsetKeyframe = (int)mpSLAM->fsSettings["ExternalLocalization.OffsetKeyframes"];

	// Initialize TF
	tf::Transform tfT;
	tfT.setIdentity();
	mTfBr.sendTransform(tf::StampedTransform(tfT,ros::Time::now(), "/ORB_SLAM/World", "/ORB_SLAM/Camera"));
}


void ImageGrabber::GrabImage(const sensor_msgs::ImageConstPtr& msg)
{
//	cout << "IMG" << endl;
	// Time record
	ros::Time rT1, rT2;
	double rtd;

	rT1 = ros::Time::now();

	// Copy the ros image message to cv::Mat.
	cv_bridge::CvImageConstPtr cv_ptr;
	try
	{
		cv_ptr = cv_bridge::toCvShare(msg);
	}
	catch (cv_bridge::Exception& e)
	{
		ROS_ERROR("cv_bridge exception: %s", e.what());
		return;
	}

	cv::Mat image;
	// Check if we need debayering
	if (enc::isBayer(msg->encoding)) {
		int code=-1;
		if (msg->encoding == enc::BAYER_RGGB8 ||
			msg->encoding == enc::BAYER_RGGB16) {
//			cout << "BGR2BGR" << endl;
			code = cv::COLOR_BayerBG2BGR;
		}
		else if (msg->encoding == enc::BAYER_BGGR8 ||
				 msg->encoding == enc::BAYER_BGGR16) {
			code = cv::COLOR_BayerRG2BGR;
		}
		else if (msg->encoding == enc::BAYER_GBRG8 ||
				 msg->encoding == enc::BAYER_GBRG16) {
			code = cv::COLOR_BayerGR2BGR;
		}
		else if (msg->encoding == enc::BAYER_GRBG8 ||
				 msg->encoding == enc::BAYER_GRBG16) {
			code = cv::COLOR_BayerGB2BGR;
		}
		cv::cvtColor(cv_ptr->image, image, code);
	}
	else
		image = cv_ptr->image;

	// Do Resizing and cropping here
	cv::resize(image, image,
		cv::Size(
			(int)mpSLAM->fsSettings["Camera.WorkingResolution.Width"],
			(int)mpSLAM->fsSettings["Camera.WorkingResolution.Height"]
		));
	image = image(
		cv::Rect(
			(int)mpSLAM->fsSettings["Camera.ROI.x0"],
			(int)mpSLAM->fsSettings["Camera.ROI.y0"],
			(int)mpSLAM->fsSettings["Camera.ROI.width"],
			(int)mpSLAM->fsSettings["Camera.ROI.height"]
		)).clone();

	mpSLAM->TrackMonocular(image,cv_ptr->header.stamp.toSec());

	// Reinsert TF publisher. Original ORB-SLAM2 removes it.
	Frame &cframe = mpSLAM->getTracker()->mCurrentFrame;
	if (!cframe.mTcw.empty()) {
		tf::Transform tfTcw = FramePose(&cframe);
		mTfBr.sendTransform(tf::StampedTransform(tfTcw,ros::Time::now(), "/ORB_SLAM/World", "/ORB_SLAM/Camera"));

		// Here, we use offset of external localization from the keyframe
		if (mpSLAM->getTracker()->mbOnlyTracking==true) {
			ORB_SLAM2::KeyFrame *ckf = cframe.mpReferenceKF;
//			cout << "Keyframe localized: " << ckf->mnId << endl;
			try {
				tf::Transform locRef = localizeByReference(tfTcw);
				mTfBr.sendTransform(tf::StampedTransform(locRef, ros::Time::now(), "/ORB_SLAM/World", "/ORB_SLAM/ExtCamera"));
			} catch (...) {}
		}

	} else { }

	rT2 = ros::Time::now();
	rtd = (rT2-rT1).toSec();
	cout << "Timed: " << rtd << endl;
}


void ImageGrabber::externalLocalizerGrab()
{
	ros::Rate fps((int)mpSLAM->fsSettings["Camera.fps"] * 2);

	while (ros::ok()) {

		if (doStop == true)
			break;

		try {
			extListener.lookupTransform (extFrame1, extFrame2, ros::Time(0), extPose);
			unique_lock<mutex> lock(ORB_SLAM2::KeyFrame::extPoseMutex);
			tfToCV (extPose, ORB_SLAM2::KeyFrame::extEgoPosition, ORB_SLAM2::KeyFrame::extEgoOrientation);

		} catch (tf::TransformException &e) {

			unique_lock<mutex> lock(ORB_SLAM2::KeyFrame::extPoseMutex);
			ORB_SLAM2::KeyFrame::extEgoPosition.release();
			ORB_SLAM2::KeyFrame::extEgoOrientation.release();

		}

		fps.sleep();
	}
}


tf::Transform ImageGrabber::localizeByReference (const tf::Transform &tfOrb, ORB_SLAM2::KeyFrame *kf)
{
	ORB_SLAM2::KeyFrame *kOffset = mpSLAM->getMap()->offsetKeyframe(kf, offsetKeyframe);
	if (kOffset==NULL)
		throw std::out_of_range("No offset keyframe found");

	cv::Mat kfPos = kf->GetCameraCenter();
	if (kf->extPosition.empty() or kOffset->extPosition.empty())
		throw std::out_of_range("External reference of keyframe not found");

	double offDistO = cv::norm(kfPos - kOffset->GetCameraCenter());
	double offDistE = cv::norm(kf->extPosition - kOffset->extPosition);
	double scale = offDistE / offDistO;

	tf::Transform flipAxes;
	flipAxes.setOrigin(tf::Vector3(0, 0, 0));
	flipAxes.setRotation (tf::Quaternion(M_PI/2, 0, -M_PI/2).normalize());

	tf::Transform kfTr = KeyFramePoseToTf(kf);
	tf::Transform extRef = getKeyFrameExtPose(kf);

	tf::Transform orbRel = kfTr.inverse() * tfOrb;
	tf::Transform scaledRel = orbRel;
	scaledRel.setOrigin(orbRel.getOrigin() * scale);
	scaledRel = flipAxes * scaledRel;

	return extRef * scaledRel;
}


tf::Transform ImageGrabber::localizeByReference(const tf::Transform &tfOrb)
{
	ORB_SLAM2::KeyFrame *kfNear = mpSLAM->getMap()->getNearestKeyFrame(
		tfOrb.getOrigin().x(),
		tfOrb.getOrigin().y(),
		tfOrb.getOrigin().z());
	if (kfNear==NULL)
		throw std::out_of_range("No keyframe found");
	return localizeByReference (tfOrb, kfNear);
}


tf::Transform ImageGrabber::localizeByReference(Frame *sframe)
{
//	const tf::Transform
}