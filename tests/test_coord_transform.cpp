#include <gtest/gtest.h>
#include "CoordTransform.h"

TEST(CoordTransformTest, WorkspaceBoundsCheck)
{
    CoordTransform ct;
    ct.setWorkspaceBounds(-500, 500, -500, 500);

    EXPECT_TRUE(ct.isInWorkspace(0, 0));
    EXPECT_TRUE(ct.isInWorkspace(-499, 499));
    EXPECT_FALSE(ct.isInWorkspace(600, 0));
    EXPECT_FALSE(ct.isInWorkspace(0, -501));
}

TEST(CoordTransformTest, NoCalibReturnsInvalid)
{
    CoordTransform ct;
    WorldCoordinate wc = ct.pixelToWorld(960, 540, 0);
    EXPECT_FALSE(wc.valid);
}

TEST(CoordTransformTest, FullyCalibratedCheck)
{
    CoordTransform ct;
    EXPECT_FALSE(ct.isFullyCalibrated());

    ct.setCameraParam(HalconCpp::HTuple(0.012));  // has cam param
    ct.setCalibPose(HalconCpp::HTuple(0.0));
    EXPECT_FALSE(ct.isFullyCalibrated());         // still missing hand-eye

    ct.setHandEyePose(0, 0, 0, 0, 0, 0);
    EXPECT_TRUE(ct.isFullyCalibrated());
}

TEST(CoordTransformTest, UndistortPixel)
{
    CoordTransform ct;
    CameraIntrinsic K;
    K.fx = 2000.0; K.fy = 2000.0;
    K.cx = 960.0; K.cy = 540.0;
    K.valid = true;
    ct.setIntrinsic(K);

    QPointF result = ct.undistortPixel(960, 540);
    EXPECT_NEAR(result.x(), 0.0, 0.001);
    EXPECT_NEAR(result.y(), 0.0, 0.001);

    QPointF offset = ct.undistortPixel(2960, 540);
    EXPECT_NEAR(offset.x(), 1.0, 0.01);
}
