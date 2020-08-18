/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2020, KU Leuven
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Jeroen De Maeyer */

/** This file tests the implementation of constriants inheriting from
 * the ompl::base::Constraint class in the file /detail/ompl_constraint.h/cpp.
 * These are used to create an ompl::base::ConstrainedStateSpace to plan with path constraints.
 *
 *  NOTE q = joint positions (The variable is so common that it's nice to have a short name in tests.)
 **/

#include <moveit/ompl_interface/detail/ompl_constraints.h>

#include <memory>
#include <string>
#include <iostream>
#include <limits>

#include <gtest/gtest.h>
#include <Eigen/Dense>
#include <eigen_conversions/eigen_msg.h>

#include <moveit/robot_model/robot_model.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit_msgs/Constraints.h>
#include <moveit/robot_state/conversions.h>
#include <moveit/utils/robot_model_test_utils.h>

#include <ompl/util/Exception.h>
#include <ompl/base/spaces/RealVectorStateSpace.h>
#include <ompl/base/spaces/constraint/ProjectedStateSpace.h>
#include <ompl/base/ConstrainedSpaceInformation.h>

/** \brief Number of times to run a test that uses randomly generated input. **/
constexpr int NUM_RANDOM_TESTS{ 100 };

/** \brief For failing tests, some extra print statements are useful. **/
constexpr bool VERBOSE{ false };

/** \brief Select a robot link at (num_dofs - different_link_offset) to test another link than the end-effector. **/
constexpr unsigned int DIFFERENT_LINK_OFFSET{ 2 };

/** \brief Allowed error when comparing Jacobian matrix error.
 *
 * High tolerance because of high finite difference error.
 * (And it is the L1-norm over the whole matrix difference.)
 **/
constexpr double JAC_ERROR_TOLERANCE{ 1e-4 };

/** \brief Helper function to create a specific position constraint.
 *
 * These constraints are fixed for the fanuc robot dimensions for now.
 * This function should take input so we can change them to something specific to the robot's workspace.
 *
 * **/
moveit_msgs::PositionConstraint createPositionConstraint(std::string& base_link, std::string& ee_link)
{
  shape_msgs::SolidPrimitive box_constraint;
  box_constraint.type = shape_msgs::SolidPrimitive::BOX;
  box_constraint.dimensions = { 0.05, 0.4, 0.05 }; /* use -1 to indicate no constraints. */

  geometry_msgs::Pose box_pose;
  box_pose.position.x = 0.9;
  box_pose.position.y = 0.0;
  box_pose.position.z = 0.2;
  box_pose.orientation.w = 1.0;

  moveit_msgs::PositionConstraint position_constraint;
  position_constraint.header.frame_id = base_link;
  position_constraint.link_name = ee_link;
  position_constraint.constraint_region.primitives.push_back(box_constraint);
  position_constraint.constraint_region.primitive_poses.push_back(box_pose);

  return position_constraint;
}
/** \brief Helper function to create a specific orientation constraint. **/
moveit_msgs::OrientationConstraint createOrientationConstraint(std::string& base_link, std::string& ee_link,
                                                               const geometry_msgs::Quaternion& nominal_orientation)
{
  moveit_msgs::OrientationConstraint oc;
  oc.header.frame_id = base_link;
  oc.link_name = ee_link;
  oc.orientation = nominal_orientation;
  oc.absolute_x_axis_tolerance = 0.3;
  oc.absolute_y_axis_tolerance = 0.3;
  oc.absolute_z_axis_tolerance = 0.3;

  return oc;
}

/** \brief Robot indepentent test class implementing all tests
 *
 * All tests are implemented in a generic test fixture, so it is
 * easy to run them on different robots.
 *
 * based on
 * https://stackoverflow.com/questions/38207346/specify-constructor-arguments-for-a-google-test-fixture/38218657
 * (answer by PiotrNycz)
 *
 * It is implemented this way to avoid the ros specific test framework
 * outside moveit_ros.
 *
 * (This is an (uglier) alternative to using the rostest framework
 * and reading the robot settings from the parameter server.
 * Then we have several rostest launch files that load the parameters
 * for a specific robot and run the same compiled tests for all robots.)
 * */
class ConstraintTestBaseClass : public testing::Test
{
protected:
  ConstraintTestBaseClass(const std::string& robot_name, const std::string& group_name)
    : robot_name_(robot_name), group_name_(group_name)
  {
  }

  void SetUp() override
  {
    // load robot
    robot_model_ = moveit::core::loadTestingRobotModel(robot_name_);
    robot_state_ = std::make_shared<robot_state::RobotState>(robot_model_);
    robot_state_->setToDefaultValues();  // avoid uninitialized memory in robot state
    joint_model_group_ = robot_state_->getJointModelGroup(group_name_);

    // extract useful parameters for tests
    num_dofs_ = joint_model_group_->getVariableCount();
    ee_link_name_ = joint_model_group_->getLinkModelNames().back();
    base_link_name_ = robot_model_->getRootLinkName();
  };

  void TearDown() override
  {
  }

  /** \brief Robot forward kinematics. **/
  const Eigen::Isometry3d fk(const Eigen::VectorXd& q, const std::string& link_name) const
  {
    robot_state_->setJointGroupPositions(joint_model_group_, q);
    return robot_state_->getGlobalLinkTransform(link_name);
  }

  Eigen::VectorXd getRandomState()
  {
    robot_state_->setToRandomPositions(joint_model_group_);
    Eigen::VectorXd joint_positions;
    robot_state_->copyJointGroupPositions(joint_model_group_, joint_positions);
    return joint_positions;
  }

  Eigen::MatrixXd numericalJacobianPosition(const Eigen::VectorXd& q, const std::string& link_name) const
  {
    const double h{ 1e-6 }; /* step size for numerical derivation */

    Eigen::MatrixXd jacobian = Eigen::MatrixXd::Zero(3, num_dofs_);

    // helper matrix for differentiation.
    Eigen::MatrixXd m_helper = h * Eigen::MatrixXd::Identity(num_dofs_, num_dofs_);

    for (std::size_t dim{ 0 }; dim < num_dofs_; ++dim)
    {
      Eigen::Vector3d pos = fk(q, link_name).translation();
      Eigen::Vector3d pos_plus_h = fk(q + m_helper.col(dim), link_name).translation();
      Eigen::Vector3d col = (pos_plus_h - pos) / h;
      jacobian.col(dim) = col;
    }
    return jacobian;
  }

  Eigen::MatrixXd numericalJacobianOrientation(const Eigen::VectorXd& q, const std::string& link_name) const
  {
    const double h{ 1e-6 }; /* step size for numerical derivation */

    Eigen::MatrixXd jacobian = Eigen::MatrixXd::Zero(3, num_dofs_);

    // helper matrix for differentiation.
    Eigen::MatrixXd m_helper = h * Eigen::MatrixXd::Identity(num_dofs_, num_dofs_);

    for (std::size_t dim{ 0 }; dim < num_dofs_; ++dim)
    {
      // Eigen::AngleAxisd aa{ fk(q, link_name).rotation().transpose() * constraint_->getTargetOrientation() };
      // Eigen::AngleAxisd aa_plus_h{ fk(q + m_helper.col(dim), link_name).rotation().transpose() *
      //                              constraint_->getTargetOrientation() };

      Eigen::AngleAxisd aa{ fk(q, link_name).rotation() };
      Eigen::AngleAxisd aa_plus_h{ fk(q + m_helper.col(dim), link_name).rotation() };
      Eigen::Vector3d col = (aa_plus_h.axis() * aa_plus_h.angle() - aa.axis() * aa.angle()) / h;
      jacobian.col(dim) = col;
    }
    return jacobian;
  }

  void testJointLimitConstraints()
  {
    setPositionConstraints();
    auto jlc = std::make_shared<ompl_interface::JointLimitConstraint>(robot_model_, group_name_, num_dofs_);

    auto ci = ompl::base::ConstraintIntersection(num_dofs_, { constraint_, jlc });

    Eigen::VectorXd input = Eigen::VectorXd::Zero(num_dofs_);
    Eigen::VectorXd output(num_dofs_);

    jlc->function(input, output);
    ROS_INFO_STREAM("Constraint error: " << output.transpose());
    EXPECT_LT(output.squaredNorm(), std::numeric_limits<double>::epsilon());

    Eigen::VectorXd input2 = Eigen::VectorXd::Ones(num_dofs_) * (M_PI + 1.23);
    Eigen::VectorXd output2(num_dofs_);

    jlc->function(input2, output2);
    ROS_INFO_STREAM("Constraint error: " << output2.transpose());
    EXPECT_GT(output2.squaredNorm(), 1.23);
  }

  void setPositionConstraints()
  {
    moveit_msgs::Constraints constraint_msgs;
    constraint_msgs.position_constraints.push_back(createPositionConstraint(base_link_name_, ee_link_name_));

    constraint_ = std::make_shared<ompl_interface::PositionConstraint>(robot_model_, group_name_, num_dofs_);
    constraint_->init(constraint_msgs);

    position_constraint_assigend = true;
  }

  void setOrientationConstraints()
  {
    // create path constraints around the default robot state
    robot_state_->setToDefaultValues();
    Eigen::Isometry3d ee_pose = robot_state_->getGlobalLinkTransform(ee_link_name_);
    geometry_msgs::Quaternion ee_orientation;
    tf::quaternionEigenToMsg(Eigen::Quaterniond(ee_pose.rotation()), ee_orientation);

    moveit_msgs::Constraints constraint_msgs;
    constraint_msgs.orientation_constraints.push_back(
        createOrientationConstraint(base_link_name_, ee_link_name_, ee_orientation));

    constraint_ = std::make_shared<ompl_interface::OrientationConstraint>(robot_model_, group_name_, num_dofs_);
    constraint_->init(constraint_msgs);

    position_constraint_assigend = false;
  }

  /** \brief Test position constraints a link that is _not_ the end-effector. **/
  void setPositionConstraintsDifferentLink()
  {
    std::string different_link = joint_model_group_->getLinkModelNames().at(num_dofs_ - DIFFERENT_LINK_OFFSET);

    if (VERBOSE)
      ROS_INFO_STREAM(different_link);

    moveit_msgs::Constraints constraint_msgs;
    constraint_msgs.position_constraints.push_back(createPositionConstraint(base_link_name_, different_link));

    constraint_ = std::make_shared<ompl_interface::PositionConstraint>(robot_model_, group_name_, num_dofs_);
    constraint_->init(constraint_msgs);
  }

  void testJacobian()
  {
    double total_error{ 999.9 };

    for (int i{ 0 }; i < NUM_RANDOM_TESTS; ++i)
    {
      auto q = getRandomState();
      auto jac_exact = constraint_->calcErrorJacobian(q);

      Eigen::MatrixXd jac_approx(3, num_dofs_);
      if (position_constraint_assigend)
      {
        jac_approx = numericalJacobianPosition(q, constraint_->getLinkName());
      }
      else
      {
        jac_approx = numericalJacobianOrientation(q, constraint_->getLinkName());
      }

      if (VERBOSE)
      {
        std::cout << "Analytical jacobian: \n";
        std::cout << jac_exact << std::endl;
        std::cout << "Finite difference jacobian: \n";
        std::cout << jac_approx << std::endl;
      }

      total_error = (jac_exact - jac_approx).lpNorm<1>();
      EXPECT_LT(total_error, JAC_ERROR_TOLERANCE);
    }
  }

  void testOMPLProjectedStateSpaceConstruction()
  {
    auto state_space = std::make_shared<ompl::base::RealVectorStateSpace>(num_dofs_);
    ompl::base::RealVectorBounds bounds(num_dofs_);

    // get joint limits from the joint model group
    auto joint_limits = joint_model_group_->getActiveJointModelsBounds();
    EXPECT_EQ(joint_limits.size(), num_dofs_);
    for (std::size_t i{ 0 }; i < num_dofs_; ++i)
    {
      EXPECT_EQ(joint_limits[i]->size(), (unsigned int)1);
      bounds.setLow(i, joint_limits[i]->at(0).min_position_);
      bounds.setHigh(i, joint_limits[i]->at(0).max_position_);
    }

    state_space->setBounds(bounds);

    // auto jl_con = std::make_shared<ompl_interface::JointLimitConstraint>(robot_model_, group_name_, num_dofs_);
    // auto ci = std::make_shared<ompl::base::ConstraintIntersection>(num_dofs_, { constraint_ });
    // ompl::base::ConstraintIntersectionPtr ci;
    // ci.reset(new ompl::base::ConstraintIntersection(num_dofs_, { constraint_, jl_con }));
    // auto constrained_state_space = std::make_shared<ompl::base::ProjectedStateSpace>(state_space, ci);

    auto constrained_state_space = std::make_shared<ompl::base::ProjectedStateSpace>(state_space, constraint_);
    // constrained_state_space->setStateSamplerAllocator()

    auto constrained_state_space_info =
        std::make_shared<ompl::base::ConstrainedSpaceInformation>(constrained_state_space);

    // TODO(jeroendm) Fix issues with sanity checks.
    // The jacobian test is expected to fail because of the discontinuous constraint derivative.
    // The issue with the state sampler is unresolved.
    // int flags = 1 & ompl::base::ConstrainedStateSpace::CONSTRAINED_STATESPACE_JACOBIAN;
    // flags = flags & ompl::base::ConstrainedStateSpace::CONSTRAINED_STATESPACE_SAMPLERS;
    try
    {
      constrained_state_space->sanityChecks();
    }
    catch (ompl::Exception& ex)
    {
      ROS_ERROR("Sanity checks did not pass: %s", ex.what());
    }
  }

  /** \brief Compare difference with adding joint limits as constraints.
   *
   * Two cases:
   * 1) No joint limits added to constraints
   *   a) projection success (before enforcing bounds)
   *   b) is it still a succes after enforcing the bounds?
   *
   * 2) Add joint limits as constraints
   *   c) projection success
   *
   * **/
  void testOMPLStateSampler()
  {
    // Create the ambient state space
    // ------------------------------
    auto state_space = std::make_shared<ompl::base::RealVectorStateSpace>(num_dofs_);
    ompl::base::RealVectorBounds bounds(num_dofs_);
    // get joint limits from the joint model group
    auto joint_limits = joint_model_group_->getActiveJointModelsBounds();
    EXPECT_EQ(joint_limits.size(), num_dofs_);
    for (std::size_t i{ 0 }; i < num_dofs_; ++i)
    {
      EXPECT_EQ(joint_limits[i]->size(), (unsigned int)1);
      bounds.setLow(i, joint_limits[i]->at(0).min_position_);
      bounds.setHigh(i, joint_limits[i]->at(0).max_position_);
    }
    state_space->setBounds(bounds);

    // Created the constraint models
    //------------------------------
    // position constraints
    setPositionConstraints();  // sets up constraint_
    // joint limit constraints
    auto jl_con = std::make_shared<ompl_interface::JointLimitConstraint>(robot_model_, group_name_, num_dofs_);
    ompl::base::ConstraintIntersectionPtr ci;
    ci.reset(new ompl::base::ConstraintIntersection(num_dofs_, { constraint_, jl_con }));

    // Create the constraint state space
    // ---------------------------------
    auto constrained_state_space = std::make_shared<ompl::base::ProjectedStateSpace>(state_space, ci);
    auto constrained_state_space_info =
        std::make_shared<ompl::base::ConstrainedSpaceInformation>(constrained_state_space);

    // ompl::base::StateSamplerPtr ss = constrained_state_space->allocStateSampler();
    ompl::base::StateSamplerPtr rvss = state_space->allocStateSampler();

    // bool proj_succes{ false };
    // Eigen::VectorXd error(3);
    // int a_counter{ 0 };  // projection succes
    // int b_counter{ 0 };  // constraint satisfaction after enforcing bounds

    // for (int i{ 0 }; i < NUM_RANDOM_TESTS; ++i)
    // {
    //   auto* s1 = constrained_state_space->allocState()->as<ompl::base::ConstrainedStateSpace::StateType>();

    //   // use unconstrained sampling and manually project it to see the difference.
    //   rvss->sampleUniform(s1->getState());
    //   // proj_succes = constraint_->project(s1);
    //   proj_succes = ci->project(s1);
    //   if (proj_succes)
    //   {
    //     a_counter++;

    //     // success, so now enforce the bounds
    //     constrained_state_space->enforceBounds(s1);
    //     Eigen::VectorXd q_clipped = *s1;
    //     constraint_->function(q_clipped, error);
    //     // ROS_INFO_STREAM("Constrained error: " << error.transpose());
    //     if (error.squaredNorm() < constraint_->getTolerance())
    //     {
    //       b_counter++;
    //     }
    //   }
    //   constrained_state_space->freeState(s1);
    // }

    constraint_->setMaxIterations(100);
    ci->setMaxIterations(100);

    ROS_INFO_STREAM("Only position constraints:");
    printSampleSuccessRates(constraint_, constrained_state_space, rvss);
    ROS_INFO_STREAM("With added joint limit constraints:");
    printSampleSuccessRates(ci, constrained_state_space, rvss);
  }

  void printSampleSuccessRates(const ompl::base::ConstraintPtr& con, const ompl::base::ConstrainedStateSpacePtr& css,
                               const ompl::base::StateSamplerPtr& sampler)
  {
    bool proj_succes{ false };
    Eigen::VectorXd error(3);
    int a_counter{ 0 };  // projection succes
    int b_counter{ 0 };  // constraint satisfaction after enforcing bounds

    for (int i{ 0 }; i < NUM_RANDOM_TESTS; ++i)
    {
      auto* s1 = css->allocState()->as<ompl::base::ConstrainedStateSpace::StateType>();

      // use unconstrained sampling and manually project it to see the difference.
      sampler->sampleUniform(s1->getState());
      // proj_succes = constraint_->project(s1);
      proj_succes = con->project(s1);
      if (proj_succes)
      {
        a_counter++;

        // success, so now enforce the bounds
        css->enforceBounds(s1);
        Eigen::VectorXd q_clipped = *s1;
        constraint_->function(q_clipped, error);
        // ROS_INFO_STREAM("Constrained error: " << error.transpose());
        if (error.squaredNorm() < constraint_->getTolerance())
        {
          b_counter++;
        }
      }
      css->freeState(s1);
    }

    ROS_INFO_STREAM("Projection succes: " << a_counter << "/" << NUM_RANDOM_TESTS);
    ROS_INFO_STREAM("Bounds enforcing: " << b_counter << "/" << NUM_RANDOM_TESTS);
  }

protected:
  const std::string robot_name_;
  const std::string group_name_;

  moveit::core::RobotModelPtr robot_model_;
  robot_state::RobotStatePtr robot_state_;
  const robot_state::JointModelGroup* joint_model_group_;

  std::shared_ptr<ompl_interface::BaseConstraint> constraint_;

  std::size_t num_dofs_;
  std::string base_link_name_;
  std::string ee_link_name_;

  bool position_constraint_assigend{ false };
};

/***************************************************************************
 * Run all tests on the Panda robot
 * ************************************************************************/
class PandaConstraintTest : public ConstraintTestBaseClass
{
protected:
  PandaConstraintTest() : ConstraintTestBaseClass("panda", "panda_arm")
  {
  }
};

TEST_F(PandaConstraintTest, InitPositionConstraint)
{
  SCOPED_TRACE("Panda_InitPositionConstraint");

  setPositionConstraints();
  setPositionConstraintsDifferentLink();
}

TEST_F(PandaConstraintTest, PositionConstraintJacobian)
{
  SCOPED_TRACE("Panda_PositionConstraintJacobian");

  setPositionConstraints();
  testJacobian();

  constraint_.reset();
  setPositionConstraintsDifferentLink();
  testJacobian();
}

TEST_F(PandaConstraintTest, PositionConstraintOMPLCheck)
{
  SCOPED_TRACE("Panda_PositionConstraintOMPLCheck");

  setPositionConstraints();
  testOMPLProjectedStateSpaceConstruction();

  constraint_.reset();
  setPositionConstraintsDifferentLink();
  testOMPLProjectedStateSpaceConstruction();
}

TEST_F(PandaConstraintTest, OrientationConstraintCreation)
{
  SCOPED_TRACE("Panda_OrientationConstraintJacobian");

  setOrientationConstraints();
}
/***************************************************************************
 * Run all tests on the Fanuc robot
 * ************************************************************************/
class FanucConstraintTest : public ConstraintTestBaseClass
{
protected:
  FanucConstraintTest() : ConstraintTestBaseClass("fanuc", "manipulator")
  {
  }
};

TEST_F(FanucConstraintTest, testJointLimitConstraints)
{
  testJointLimitConstraints();
}

TEST_F(FanucConstraintTest, InitPositionConstraint)
{
  setPositionConstraints();
  setPositionConstraintsDifferentLink();
}

TEST_F(FanucConstraintTest, PositionConstraintJacobian)
{
  setPositionConstraints();
  testJacobian();

  constraint_.reset();
  setPositionConstraintsDifferentLink();
  testJacobian();
}

TEST_F(FanucConstraintTest, PositionConstraintOMPLCheck)
{
  setPositionConstraints();
  testOMPLProjectedStateSpaceConstruction();

  constraint_.reset();
  setPositionConstraintsDifferentLink();
  testOMPLProjectedStateSpaceConstruction();
}

TEST_F(FanucConstraintTest, testOMPLStateSampler)
{
  testOMPLStateSampler();
}

/***************************************************************************
 * Run all tests on the PR2's left arm
 * ************************************************************************/
class PR2LeftArmConstraintTest : public ConstraintTestBaseClass
{
protected:
  PR2LeftArmConstraintTest() : ConstraintTestBaseClass("pr2", "left_arm")
  {
  }
};

TEST_F(PR2LeftArmConstraintTest, InitPositionConstraint)
{
  setPositionConstraints();
  setPositionConstraintsDifferentLink();
}

TEST_F(PR2LeftArmConstraintTest, PositionConstraintJacobian)
{
  setPositionConstraints();
  testJacobian();

  constraint_.reset();
  setPositionConstraintsDifferentLink();
  testJacobian();
}

TEST_F(PR2LeftArmConstraintTest, PositionConstraintOMPLCheck)
{
  setPositionConstraints();
  testOMPLProjectedStateSpaceConstruction();

  constraint_.reset();
  setPositionConstraintsDifferentLink();
  testOMPLProjectedStateSpaceConstruction();
}

/***************************************************************************
 * MAIN
 * ************************************************************************/
int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}