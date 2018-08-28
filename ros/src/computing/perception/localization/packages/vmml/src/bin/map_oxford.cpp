/*
 * map_oxford.cpp
 *
 *  Created on: Jul 30, 2018
 *      Author: sujiwo
 */


#include <iostream>
#include <Eigen/Eigen>
#include <Eigen/Geometry>

#include "datasets/OxfordDataset.h"
#include "utilities.h"
#include "Viewer.h"
#include "MapBuilder2.h"

using namespace std;
using namespace Eigen;


MapBuilder2 *builder = NULL;
Viewer *imgViewer;

const double
	translationThrs = 1.0,	// meter
	rotationThrs = 0.04;	// == 2.5 degrees


InputFrame createInputFrame(OxfordDataItem &d)
{
	cv::Mat i=d.getImage();
	// Oxford datasets output is RGB
	cv::cvtColor(i, i, CV_BGR2GRAY, 1);
	return InputFrame(
		i,
		d.groundTruth.position(),
		d.groundTruth.orientation(),
		// Force Keyframe ID using timestamp. This way, we can refer to original
		// image for display purpose
		static_cast<kfid>(d.getTimestamp()));
}


void buildMap (OxfordDataset &dataset)
{
	if (builder==NULL) {
		builder = new MapBuilder2;
		builder->addCameraParam(dataset.getCameraParameter());
	}

	imgViewer = new Viewer(dataset);
	imgViewer->setMap(builder->getMap());

	// Find two initializer frames
	OxfordDataItem d0, d1;
	InputFrame frame0, frame1;
	int iptr;
	for (iptr=0; iptr<dataset.size(); iptr++) {
		d0 = dataset.at(iptr);
		frame0 = createInputFrame(d0);
		auto normcdf = cdf(frame0.image);
		if (normcdf[127]<0.25)
			continue;
		else {
			break;
		}
	}
	for (iptr+=1; iptr<dataset.size(); iptr++) {
		d1 = dataset.at(iptr);
		frame1 = createInputFrame(d1);
		auto normcdf = cdf(frame1.image);
		if (normcdf[127]<0.25)
			continue;
		double runTrans, runRot;
		frame1.getPose().displacement(frame0.getPose(), runTrans, runRot);
		if (runTrans>=translationThrs or runRot>=rotationThrs)
			break;
	}

//	OxfordDataItem d0 = dataset.at(0);
//	OxfordDataItem d1;
//	for (int i=1; i<dataset.size(); i++) {
//		d1 = dataset.at(i);
//		double runTrans, runRot;
//		d1.groundTruth.displacement(d0.groundTruth, runTrans, runRot);
//		if (runTrans>=translationThrs or runRot>=rotationThrs or i>=dataset.size()) {
//			break;
//		}
//	}

//	InputFrame frame0 = createInputFrame(d0);
//	InputFrame frame1 = createInputFrame(d1);

	builder->initialize(frame0, frame1);
	imgViewer->update(d1.getId(), builder->getCurrentKeyFrameId());

	OxfordDataItem *anchor = &d1;

	for (int i=2; i<dataset.size(); i++) {

		OxfordDataItem &dCurrent = dataset.at(i);
		double diffTrans, diffRotn;
		anchor->groundTruth.displacement(dCurrent.groundTruth, diffTrans, diffRotn);
		if (diffTrans<translationThrs and diffRotn<rotationThrs)
			continue;

		// XXX: Store Anchor frame in MapBuilder2 itself
		InputFrame frameCurrent = createInputFrame(dCurrent);
		builder->track(frameCurrent);
		anchor = &dCurrent;
		cerr << anchor->getId() << endl;
		imgViewer->update(dCurrent.getId(), builder->getCurrentKeyFrameId());
	}

	builder->build();
}


int main (int argc, char *argv[])
{
	OxfordDataset oxf(argv[1], "/home/sujiwo/Sources/robotcar-dataset-sdk/models");
	OxfordDataItem m0 = oxf.at(0);
	OxfordDataset oxfSubset;
	oxf.setZoomRatio(0.5);

	double start, howlong;
	if (argc==3) {
		start=stod(argv[2]),
		howlong=stod(argv[3]);
		oxfSubset = oxf.timeSubset(start, howlong);
	}
	else {
		oxfSubset = oxf;
	}

	string stname = oxfSubset.getName();
	buildMap (oxfSubset);
	builder->getMap()->save("/tmp/oxford1.map");
//	oxf.dump();
	return 0;
}