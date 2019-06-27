/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   testOnlineAlignment.cpp
 * @brief  Unit tests for Online Alignment class.
 * @author Sandro Berchier, Luca Carlone
 */


#include <gflags/gflags.h>
#include <glog/logging.h>
#include "gtest/gtest.h"

#include "ImuFrontEnd-definitions.h"
#include "ImuFrontEnd.h"
#include "OnlineGravityAlignment.h"
#include "utils/ThreadsafeImuBuffer.h"
#include "ETH_parser.h"
#include "test_config.h"

// Add last, since it redefines CHECK, which is first defined by glog.
#include <CppUnitLite/TestHarness.h>

using namespace VIO;

static const double tol_GB = 2e-4;
static const double tol_TB = 1e-7;
static const double tol_OGA = 1e-3;

/* -------------------------------------------------------------------------- */
//class OnlineAlignmentTestData : public ::testing::Test {
class OnlineAlignmentTestData {
 public:
  AlignmentPoses estimated_poses_;
  AlignmentPims pims_;
  std::vector<double> delta_t_poses_;
  ImuParams imu_params_;
  ImuBias imu_bias_;
  Vector3 bias_acc_;
  Vector3 bias_gyr_;
  OnlineAlignmentTestData(ETHDatasetParser &dataset,
                      const std::string data_path,
                      const int n_begin_data,
                      const int n_frames_data) {
      // Load IMU data and compute pre-integrations
      std::string imu_name = "imu0";
      dataset.parseImuData(data_path,
                          imu_name);

      // Set IMU params
      imu_params_.acc_walk_ = 1.0;
      imu_params_.acc_noise_ = 1.0;
      imu_params_.gyro_walk_ = 1.0;
      imu_params_.gyro_noise_ = 1.0;
      imu_params_.n_gravity_ << 0.0, 0.0, 0.0; // This is needed for online alignment
      imu_params_.imu_integration_sigma_ = 1.0;
      bias_acc_ = Vector3(0.0, 0.0, 0.0);
      bias_gyr_ = Vector3 (0.0, 0.0, 0.0);
      imu_bias_ = ImuBias(bias_acc_, bias_gyr_);

      // Load ground-truth poses
      std::string gt_name = "gt0";
      dataset.parseGTdata(data_path,
                          gt_name);
      GroundTruthData gtData();

      // Get GT poses and IMU pims
      Timestamp timestamp_last_frame;
      Timestamp timestamp_frame_k;
      ImuMeasurements imu_meas;

      // Variables for online alignment
      estimated_poses_.clear();
      pims_.clear();
      delta_t_poses_.clear();

      // Extract first element in the map
      std::map<long long, gtNavState>::iterator it;
      it = dataset.gtData_.mapToGt_.begin();
      for (int i = 0; i < n_begin_data; i++) {
        it++; // Move iterator to desired spot
      }
      timestamp_last_frame = it->first;
      gtsam::Pose3 gt_pose_k = it->second.pose();
      estimated_poses_.push_back(gt_pose_k);

      it++; // Move to the second one
      while (it != dataset.gtData_.mapToGt_.end())
      {
        // Get GT information
        timestamp_frame_k = it->first;
        gt_pose_k = it->second.pose();

        // Get PIM information
        dataset.imuData_.imu_buffer_.getImuDataInterpolatedUpperBorder(
              timestamp_last_frame,
              timestamp_frame_k,
              &imu_meas.timestamps_,
              &imu_meas.measurements_);
        ImuFrontEnd imu_frontend(imu_params_, imu_bias_);
        const auto& pim = imu_frontend.preintegrateImuMeasurements(
                    imu_meas.timestamps_, imu_meas.measurements_);

        // Buffer for online alignment
        estimated_poses_.push_back(gt_pose_k);
        delta_t_poses_.push_back(UtilsOpenCV::NsecToSec(
                  timestamp_frame_k - timestamp_last_frame));
        pims_.push_back(pim);
        if (pims_.size() > n_frames_data) {
          break;
        }
        // Move to next element in map
        timestamp_last_frame = timestamp_frame_k;
        it++;
      }
  };
};

/* -------------------------------------------------------------------------- */
TEST(testOnlineAlignment, GyroscopeBiasEstimation) {  
  // Construct ETH Parser and get data
  std::string reason = "test of gyroscope estimation";
  ETHDatasetParser dataset(reason);
  static const std::string data_path(DATASET_PATH + std::string("/ForOnlineAlignment/gyro_bias/"));
  int n_begin= 1;
  int n_frames = 5;
  OnlineAlignmentTestData test_data(dataset, data_path,
                          n_begin, n_frames);

  // Initialize OnlineAlignment
  gtsam::Vector3 gyro_bias = test_data.imu_bias_.gyroscope();
  CHECK_DOUBLE_EQ(gyro_bias.norm(), 0.0);

  // Construct online alignment class with dummy gravity vector
  gtsam::Vector3 n_gravity(0.0, 0.0, 0.0);
  OnlineGravityAlignment initial_alignment(
                      test_data.estimated_poses_, 
                      test_data.delta_t_poses_,
                      test_data.pims_, 
                      n_gravity);

  // Compute Gyroscope Bias
  CHECK(initial_alignment.estimateGyroscopeBiasOnly(&gyro_bias));

  // Final test check against real bias in data
  gtsam::Vector3 real_gyro_bias(0.0001, 0.0002, 0.0003);
  DOUBLES_EQUAL(real_gyro_bias.norm(), gyro_bias.norm(), tol_GB);
}

/* -------------------------------------------------------------------------- */
TEST(testOnlineAlignment, CreateTangentBasis) {
  for (int i=0; i<20; i++) {
    // Create random vector (this is not unit vector!)
    gtsam::Vector3 random_vector = UtilsOpenCV::RandomVectorGenerator(1.0);

    // Create tangent basis to random vector
    gtsam::Matrix tangent_basis = 
            OnlineGravityAlignment::createTangentBasis(random_vector);

    // Check size is corrrect
    CHECK_EQ(tangent_basis.cols(), 2);
    CHECK_EQ(tangent_basis.rows(), 3);

    // Check product of matrix columns with random vector
    gtsam::Vector3 basis_vec_y(tangent_basis(0, 0),
                              tangent_basis(1, 0),
                              tangent_basis(2, 0));
    gtsam::Vector3 basis_vec_z(tangent_basis(0, 1),
                              tangent_basis(1, 1),
                              tangent_basis(2, 1));

    // Check that vector product is zero (orthogonal)
    DOUBLES_EQUAL(gtsam::dot(basis_vec_y, basis_vec_z), 0.0, tol_TB);
    DOUBLES_EQUAL(gtsam::dot(basis_vec_y, random_vector), 0.0, tol_TB);
    DOUBLES_EQUAL(gtsam::dot(basis_vec_z, random_vector), 0.0, tol_TB);
  }
}

// TODO(Sandro): Push to new branch
// TODO(Sandro): Terminate this test and implemenation
/* -------------------------------------------------------------------------- */
TEST(testOnlineAlignment, OnlineGravityAlignment) {  
  // Construct ETH Parser and get data
  std::string reason = "test of alignment estimation";
  ETHDatasetParser dataset(reason);
  static const std::string data_path(DATASET_PATH + std::string("/ForOnlineAlignment/alignment/"));
  int n_begin= 1;
  int n_frames = 40;
  gtsam::Vector3 real_init_vel(0.1, 0.2, -0.05);
  OnlineAlignmentTestData test_data(dataset, data_path,
                          n_begin, n_frames);

  // Initialize OnlineAlignment
  gtsam::Vector3 gyro_bias = test_data.imu_bias_.gyroscope();
  CHECK_DOUBLE_EQ(gyro_bias.norm(), 0.0);
  gtsam::Vector3 g_iter;
  gtsam::NavState init_navstate;

  // Construct online alignment class with world gravity vector
  gtsam::Vector3 n_gravity(0.0, 0.0, -9.81);
  OnlineGravityAlignment initial_alignment(
                      test_data.estimated_poses_, 
                      test_data.delta_t_poses_,
                      test_data.pims_, 
                      n_gravity);

  // Compute Gyroscope Bias
  CHECK(initial_alignment.alignVisualInertialEstimates(&gyro_bias, &g_iter,
                                                 &init_navstate));

  // Final test checks
  DOUBLES_EQUAL(g_iter.norm(), n_gravity.norm(), tol_OGA);
  DOUBLES_EQUAL(g_iter.x(), n_gravity.x(), tol_OGA);
  DOUBLES_EQUAL(g_iter.y(), n_gravity.y(), tol_OGA);
  DOUBLES_EQUAL(g_iter.z(), n_gravity.z(), tol_OGA);
  EXPECT(assert_equal(gtsam::Pose3(), init_navstate.pose(), tol_OGA));
  DOUBLES_EQUAL(real_init_vel.norm(),
            init_navstate.velocity().norm(), tol_OGA);
  DOUBLES_EQUAL(real_init_vel.x(),
            init_navstate.velocity().x(), tol_OGA);
  DOUBLES_EQUAL(real_init_vel.y(),
            init_navstate.velocity().y(), tol_OGA);
  DOUBLES_EQUAL(real_init_vel.z(),
            init_navstate.velocity().z(), tol_OGA);
}

/* ************************************************************************* */
int main(int argc, char *argv[]) {
  // Initialize Google's flags library.
  google::ParseCommandLineFlags(&argc, &argv, true);
  // Initialize Google's logging library.
  google::InitGoogleLogging(argv[0]);
  TestResult tr; 
  return TestRegistry::runAllTests(tr);
}
/* ************************************************************************* */
