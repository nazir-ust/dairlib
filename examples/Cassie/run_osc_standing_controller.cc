#include <drake/common/yaml/yaml_read_archive.h>
#include <gflags/gflags.h>

#include "dairlib/lcmt_robot_input.hpp"
#include "dairlib/lcmt_robot_output.hpp"
#include "examples/Cassie/cassie_utils.h"
#include "examples/Cassie/osc/standing_com_traj.h"
#include "multibody/kinematic/kinematic_evaluator_set.h"
#include "multibody/multibody_utils.h"
#include "systems/controllers/osc/operational_space_control.h"
#include "systems/framework/lcm_driven_loop.h"
#include "systems/robot_lcm_systems.h"
#include "yaml-cpp/yaml.h"
//#include "drake/common/yaml/yaml_read_archive.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/lcm/lcm_publisher_system.h"

namespace dairlib {

using std::cout;
using std::endl;

using Eigen::Matrix3d;
using Eigen::MatrixXd;
using Eigen::Vector3d;
using Eigen::VectorXd;

using drake::geometry::SceneGraph;
using drake::multibody::MultibodyPlant;
using drake::multibody::Parser;
using drake::systems::DiagramBuilder;
using drake::systems::TriggerType;
using drake::systems::lcm::LcmPublisherSystem;
using drake::systems::lcm::LcmSubscriberSystem;
using drake::systems::lcm::TriggerTypeSet;

using systems::controllers::ComTrackingData;
using systems::controllers::JointSpaceTrackingData;
using systems::controllers::RotTaskSpaceTrackingData;
using systems::controllers::TransTaskSpaceTrackingData;

DEFINE_string(channel_x, "CASSIE_STATE_SIMULATION",
              "LCM channel for receiving state. "
              "Use CASSIE_STATE_SIMULATION to get state from simulator, and "
              "use CASSIE_STATE_DISPATCHER to get state from state estimator");
DEFINE_string(channel_u, "CASSIE_INPUT",
              "The name of the channel which publishes command");
DEFINE_bool(print_osc, false, "whether to print the osc debug message or not");
DEFINE_double(cost_weight_multiplier, 0.001,
              "A cosntant times with cost weight of OSC traj tracking");
DEFINE_double(height, .89, "The desired height (m)");
DEFINE_string(gains_filename, "examples/Cassie/osc/osc_standing_gains.yaml", "Filepath containing gains");

// Currently the controller runs at the rate between 500 Hz and 200 Hz, so the
// publish rate of the robot state needs to be less than 500 Hz. Otherwise, the
// performance seems to degrade due to this. (Recommended publish rate: 200 Hz)
// Maybe we need to update the lcm driven loop to clear the queue of lcm message
// if it's more than one message?

struct OSCStandingGains {
  int rows;
  int cols;
  std::vector<double> CoMKp;
  std::vector<double> CoMKd;
  std::vector<double> PelvisRotKp;
  std::vector<double> PelvisRotKd;
  std::vector<double> CoMW;
  std::vector<double> PelvisW;


  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(DRAKE_NVP(rows));
    a->Visit(DRAKE_NVP(cols));
    a->Visit(DRAKE_NVP(CoMKp));
    a->Visit(DRAKE_NVP(CoMKd));
    a->Visit(DRAKE_NVP(PelvisRotKp));
    a->Visit(DRAKE_NVP(PelvisRotKd));
    a->Visit(DRAKE_NVP(CoMW));
    a->Visit(DRAKE_NVP(PelvisW));
  }
};

int DoMain(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // Build Cassie MBP
  drake::multibody::MultibodyPlant<double> plant_w_springs(0.0);
  addCassieMultibody(&plant_w_springs, nullptr, true /*floating base*/,
                     "examples/Cassie/urdf/cassie_v2.urdf",
                     true /*spring model*/, false /*loop closure*/);
  plant_w_springs.Finalize();
  // Build fix-spring Cassie MBP
  drake::multibody::MultibodyPlant<double> plant_wo_springs(0.0);
  addCassieMultibody(&plant_wo_springs, nullptr, true,
                     "examples/Cassie/urdf/cassie_fixed_springs.urdf", false,
                     false);
  plant_wo_springs.Finalize();

  // Get contact frames and position (doesn't matter whether we use
  // plant_w_springs or plant_wo_springs because the contact frames exit in both
  // plants)
  auto left_toe = LeftToeFront(plant_wo_springs);
  auto left_heel = LeftToeRear(plant_wo_springs);
  auto right_toe = RightToeFront(plant_wo_springs);
  auto right_heel = RightToeRear(plant_wo_springs);

  // Build the controller diagram
  DiagramBuilder<double> builder;

  drake::lcm::DrakeLcm lcm_local("udpm://239.255.76.67:7667?ttl=0");
  OSCStandingGains result;
  const YAML::Node& root =
      YAML::LoadFile(FindResourceOrThrow(FLAGS_gains_filename));
  drake::yaml::YamlReadArchive(root).Accept(&result);

  MatrixXd K_p_com = Eigen::Map<
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(
      result.CoMKp.data(), result.rows, result.cols);
  MatrixXd K_d_com = Eigen::Map<
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(
      result.CoMKd.data(), result.rows, result.cols);
  MatrixXd K_p_pelvis = Eigen::Map<
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(
      result.PelvisRotKp.data(), result.rows, result.cols);
  MatrixXd K_d_pelvis = Eigen::Map<
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(
      result.PelvisRotKd.data(), result.rows, result.cols);
 MatrixXd W_com = Eigen::Map<
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(
      result.CoMW.data(), result.rows, result.cols);
  MatrixXd W_pelvis = Eigen::Map<
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(
      result.PelvisW.data(), result.rows, result.cols);
  std::cout << "COM Kp: \n" << K_p_com << std::endl;
  std::cout << "COM Kd: \n" << K_d_com << std::endl;
  std::cout << "Pelvis Rot Kp: \n" << K_p_pelvis << std::endl;
  std::cout << "Pelvis Rot Kd: \n" << K_d_pelvis << std::endl;
  std::cout << "COM W: \n" << W_com << std::endl;
  std::cout << "Pelvis W: \n" << W_pelvis << std::endl;

  // Create state receiver.
  auto state_receiver =
      builder.AddSystem<systems::RobotOutputReceiver>(plant_w_springs);

  // Create command sender.
  auto command_pub =
      builder.AddSystem(LcmPublisherSystem::Make<dairlib::lcmt_robot_input>(
          FLAGS_channel_u, &lcm_local, TriggerTypeSet({TriggerType::kForced})));
  auto command_sender =
      builder.AddSystem<systems::RobotCommandSender>(plant_w_springs);

  builder.Connect(command_sender->get_output_port(0),
                  command_pub->get_input_port());

  // Create osc debug sender.
  auto osc_debug_pub =
      builder.AddSystem(LcmPublisherSystem::Make<dairlib::lcmt_osc_output>(
          "OSC_DEBUG", &lcm_local, TriggerTypeSet({TriggerType::kForced})));

  // Create desired center of mass traj
  std::vector<std::pair<const Vector3d, const drake::multibody::Frame<double>&>>
      feet_contact_points = {left_toe, left_heel, right_toe, right_heel};
  auto com_traj_generator = builder.AddSystem<cassie::osc::StandingComTraj>(
      plant_w_springs, feet_contact_points, FLAGS_height);
  builder.Connect(state_receiver->get_output_port(0),
                  com_traj_generator->get_input_port_state());

  // Create Operational space control
  auto osc = builder.AddSystem<systems::controllers::OperationalSpaceControl>(
      plant_w_springs, plant_wo_springs, false, FLAGS_print_osc);

  // Distance constraint
  multibody::KinematicEvaluatorSet<double> evaluators(plant_wo_springs);
  auto left_loop = LeftLoopClosureEvaluator(plant_wo_springs);
  auto right_loop = RightLoopClosureEvaluator(plant_wo_springs);
  evaluators.add_evaluator(&left_loop);
  evaluators.add_evaluator(&right_loop);
  osc->AddKinematicConstraint(&evaluators);
  // Soft constraint
  // We don't want w_contact_relax to be too big, cause we want tracking
  // error to be important
  double w_contact_relax = 20000;
  osc->SetWeightOfSoftContactConstraint(w_contact_relax);
  // Friction coefficient
  double mu = 0.8;
  osc->SetContactFriction(mu);
  // Add contact points (The position doesn't matter. It's not used in OSC)
  auto left_toe_evaluator = multibody::WorldPointEvaluator(
      plant_wo_springs, left_toe.first, left_toe.second, Matrix3d::Identity(),
      Vector3d::Zero(), {1, 2});
  osc->AddContactPoint(&left_toe_evaluator);
  auto left_heel_evaluator = multibody::WorldPointEvaluator(
      plant_wo_springs, left_heel.first, left_heel.second, Matrix3d::Identity(),
      Vector3d::Zero(), {0, 1, 2});
  osc->AddContactPoint(&left_heel_evaluator);
  auto right_toe_evaluator = multibody::WorldPointEvaluator(
      plant_wo_springs, right_toe.first, right_toe.second, Matrix3d::Identity(),
      Vector3d::Zero(), {1, 2});
  osc->AddContactPoint(&right_toe_evaluator);
  auto right_heel_evaluator = multibody::WorldPointEvaluator(
      plant_wo_springs, right_heel.first, right_heel.second,
      Matrix3d::Identity(), Vector3d::Zero(), {0, 1, 2});
  osc->AddContactPoint(&right_heel_evaluator);
  // Cost
  int n_v = plant_wo_springs.num_velocities();
  MatrixXd Q_accel = 0.01 * MatrixXd::Identity(n_v, n_v);
  osc->SetAccelerationCostForAllJoints(Q_accel);
  // Center of mass tracking
  // Weighting x-y higher than z, as they are more important to balancing
  ComTrackingData center_of_mass_traj("com_traj", K_p_com, K_d_com,
                                      W_com * FLAGS_cost_weight_multiplier,
                                      plant_w_springs, plant_wo_springs);
  osc->AddTrackingData(&center_of_mass_traj);
  // Pelvis rotation tracking
  RotTaskSpaceTrackingData pelvis_rot_traj(
      "pelvis_rot_traj", K_p_pelvis, K_d_pelvis,
      W_pelvis * FLAGS_cost_weight_multiplier, plant_w_springs,
      plant_wo_springs);
  pelvis_rot_traj.AddFrameToTrack("pelvis");
  VectorXd pelvis_desired_quat(4);
  pelvis_desired_quat << 1, 0, 0, 0;
  osc->AddConstTrackingData(&pelvis_rot_traj, pelvis_desired_quat);
  /*// Left hip yaw joint tracking
  MatrixXd W_hip_yaw = 20 * MatrixXd::Identity(1, 1);
  MatrixXd K_p_hip_yaw = 200 * MatrixXd::Identity(1, 1);
  MatrixXd K_d_hip_yaw = 160 * MatrixXd::Identity(1, 1);
  JointSpaceTrackingData left_hip_yaw_traj(
      "left_hip_yaw_traj", K_p_hip_yaw, K_d_hip_yaw,
      W_hip_yaw * FLAGS_cost_weight_multiplier, &plant_w_springs,
      &plant_wo_springs);
  left_hip_yaw_traj.AddJointToTrack("hip_yaw_left", "hip_yaw_leftdot");
  osc->AddConstTrackingData(&left_hip_yaw_traj, VectorXd::Zero(1));
  // right hip yaw joint tracking
  JointSpaceTrackingData right_hip_yaw_traj(
      "right_hip_yaw_traj", K_p_hip_yaw, K_d_hip_yaw,
      W_hip_yaw * FLAGS_cost_weight_multiplier, &plant_w_springs,
      &plant_wo_springs);
  right_hip_yaw_traj.AddJointToTrack("hip_yaw_right", "hip_yaw_rightdot");
  osc->AddConstTrackingData(&right_hip_yaw_traj, VectorXd::Zero(1));*/
  // Build OSC problem
  osc->Build();
  // Connect ports
  builder.Connect(state_receiver->get_output_port(0),
                  osc->get_robot_output_input_port());
  builder.Connect(osc->get_osc_output_port(),
                  command_sender->get_input_port(0));
  builder.Connect(osc->get_osc_debug_port(), osc_debug_pub->get_input_port());
  builder.Connect(com_traj_generator->get_output_port(0),
                  osc->get_tracking_data_input_port("com_traj"));

  // Create the diagram
  auto owned_diagram = builder.Build();
  owned_diagram->set_name(("osc standing controller"));

  // Run lcm-driven simulation
  systems::LcmDrivenLoop<dairlib::lcmt_robot_output> loop(
      &lcm_local, std::move(owned_diagram), state_receiver, FLAGS_channel_x,
      true);
  loop.Simulate();

  return 0;
}

}  // namespace dairlib

int main(int argc, char* argv[]) { return dairlib::DoMain(argc, argv); }
