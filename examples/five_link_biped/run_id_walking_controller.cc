#include <drake/lcmt_contact_results_for_viz.hpp>
#include <drake/multibody/parsing/parser.h>
#include <gflags/gflags.h>

#include "common/find_resource.h"
#include "dairlib/lcmt_robot_input.hpp"
#include "dairlib/lcmt_robot_output.hpp"
#include "examples/Cassie/osc_jump/basic_trajectory_generator.h"
#include "examples/Cassie/osc_jump/id_jumping_gains.h"
#include "lcm/dircon_saved_trajectory.h"
#include "lcm/lcm_trajectory.h"
#include "multibody/multibody_utils.h"
#include "systems/controllers/osc/operational_space_control.h"
#include "systems/controllers/osc/osc_tracking_data.h"
#include "systems/controllers/time_based_fsm.h"
#include "systems/framework/lcm_driven_loop.h"
#include "systems/robot_lcm_systems.h"
#include "yaml-cpp/yaml.h"

#include "drake/common/yaml/yaml_read_archive.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/lcm/lcm_publisher_system.h"

namespace dairlib {

using std::cout;
using std::endl;
using std::map;
using std::pair;
using std::string;
using std::vector;

using Eigen::Matrix3d;
using Eigen::MatrixXd;
using Eigen::Vector3d;
using Eigen::VectorXd;

using drake::geometry::SceneGraph;
using drake::multibody::Frame;
using drake::multibody::MultibodyPlant;
using drake::multibody::Parser;
using drake::systems::DiagramBuilder;
using drake::systems::TriggerType;
using drake::systems::lcm::LcmPublisherSystem;
using drake::systems::lcm::LcmSubscriberSystem;
using drake::systems::lcm::TriggerTypeSet;
using drake::trajectories::PiecewisePolynomial;
using examples::osc_jump::BasicTrajectoryPassthrough;
using systems::controllers::ComTrackingData;
using systems::controllers::JointSpaceTrackingData;
using systems::controllers::RotTaskSpaceTrackingData;
using systems::controllers::TransTaskSpaceTrackingData;

namespace examples {

DEFINE_string(channel_x, "RABBIT_STATE",
              "The name of the channel which receives state");
DEFINE_string(channel_u, "RABBIT_INPUT",
              "The name of the channel which publishes command");
DEFINE_bool(print_osc, false, "whether to print the osc debug message or not");
DEFINE_string(folder_path, "examples/five_link_biped/saved_trajectories/",
              "Folder path for where the trajectory names are stored");
DEFINE_string(traj_name, "rabbit_walking",
              "File to load saved trajectories from");
DEFINE_double(delay_time, 0.0, "time to wait before executing jump");
DEFINE_double(transition_delay, 0.0,
              "Time to wait after trigger to "
              "transition between FSM states.");
DEFINE_string(gains_filename, "examples/five_link_biped/id_walking_gains.yaml",
              "Filepath containing gains");

int DoMain(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // Build the controller diagram
  DiagramBuilder<double> builder;

  // Built the Cassie MBPs
  drake::multibody::MultibodyPlant<double> plant(0.0);
  SceneGraph<double>& scene_graph = *(builder.AddSystem<SceneGraph>());
  Parser parser(&plant, &scene_graph);
  std::string full_name =
      FindResourceOrThrow("examples/five_link_biped/five_link_biped.urdf");
  parser.AddModelFromFile(full_name);
  plant.WeldFrames(plant.world_frame(), plant.GetFrameByName("base"),
                   drake::math::RigidTransform<double>());
  plant.Finalize();

  auto plant_context = plant.CreateDefaultContext();

  // Get contact frames and position (doesn't matter whether we use
  // plant or plant_wo_springs because the contact frames exit in both
  // plants)
  int nq = plant.num_positions();
  int nv = plant.num_velocities();
  int nx = nq + nv;

  // Create maps for joints
  map<string, int> pos_map = multibody::makeNameToPositionsMap(plant);
  map<string, int> vel_map = multibody::makeNameToVelocitiesMap(plant);
  map<string, int> act_map = multibody::makeNameToActuatorsMap(plant);

  /**** Convert the gains from the yaml struct to Eigen Matrices ****/
  IDJumpingGains gains;
  const YAML::Node& root =
      YAML::LoadFile(FindResourceOrThrow(FLAGS_gains_filename));
  drake::yaml::YamlReadArchive(root).Accept(&gains);

  /**** Get trajectory from optimization ****/
  const DirconTrajectory& dircon_trajectory = DirconTrajectory(
      FindResourceOrThrow(FLAGS_folder_path + FLAGS_traj_name));
  //  const DirconTrajectory& dircon_trajectory = DirconTrajectory(
  //      FindResourceOrThrow(FLAGS_traj_name));

  PiecewisePolynomial<double> state_traj =
      dircon_trajectory.ReconstructStateTrajectory();

  /**** Initialize all the leaf systems ****/
  drake::lcm::DrakeLcm lcm("udpm://239.255.76.67:7667?ttl=0");

  vector<int> fsm_states;
  vector<double> state_durations;
  fsm_states = {0, 1};
  state_durations = {dircon_trajectory.GetStateBreaks(1)(0),
                     dircon_trajectory.GetStateBreaks(2)(0)};
  auto state_receiver = builder.AddSystem<systems::RobotOutputReceiver>(plant);
  auto fsm = builder.AddSystem<systems::TimeBasedFiniteStateMachine>(
      plant, fsm_states, state_durations, 0.0, gains.impact_threshold);
  auto command_pub =
      builder.AddSystem(LcmPublisherSystem::Make<dairlib::lcmt_robot_input>(
          FLAGS_channel_u, &lcm, TriggerTypeSet({TriggerType::kForced})));
  auto command_sender = builder.AddSystem<systems::RobotCommandSender>(plant);
  auto osc = builder.AddSystem<systems::controllers::OperationalSpaceControl>(
      plant, plant, plant_context.get(), plant_context.get(), true,
      FLAGS_print_osc); /*print_tracking_info*/
  auto osc_debug_pub =
      builder.AddSystem(LcmPublisherSystem::Make<dairlib::lcmt_osc_output>(
          "OSC_DEBUG_WALKING", &lcm, TriggerTypeSet({TriggerType::kForced})));
  //  LcmSubscriberSystem* contact_results_sub = builder.AddSystem(
  //      LcmSubscriberSystem::Make<drake::lcmt_contact_results_for_viz>(
  //          "RABBIT_CONTACT_DRAKE", &lcm));

  /**** OSC setup ****/
  // Cost
  MatrixXd Q_accel = gains.w_accel * MatrixXd::Identity(nv, nv);
  osc->SetAccelerationCostForAllJoints(Q_accel);
  // Soft constraint on contacts
  double w_contact_relax = gains.w_soft_constraint;
  osc->SetWeightOfSoftContactConstraint(w_contact_relax);

  // Contact information for OSC
  osc->SetContactFriction(gains.mu);

  Vector3d foot_contact_disp(0, 0, 0);
  auto left_foot_evaluator = multibody::WorldPointEvaluator(
      plant, foot_contact_disp, plant.GetBodyByName("left_foot").body_frame(),
      Matrix3d::Identity(), Vector3d::Zero(), {0, 2});
  auto right_foot_evaluator = multibody::WorldPointEvaluator(
      plant, foot_contact_disp, plant.GetBodyByName("right_foot").body_frame(),
      Matrix3d::Identity(), Vector3d::Zero(), {0, 2});

  osc->AddStateAndContactPoint(0, &left_foot_evaluator);
  osc->AddStateAndContactPoint(1, &right_foot_evaluator);

  // Create maps for joints
  map<string, int> pos_map_wo_spr = multibody::makeNameToPositionsMap(plant);

  std::vector<BasicTrajectoryPassthrough*> joint_trajs;
  std::vector<std::shared_ptr<JointSpaceTrackingData>> joint_tracking_data_vec;

  std::vector<std::string> actuated_joint_names = {
      "left_hip_pin", "right_hip_pin", "left_knee_pin", "right_knee_pin"};

  for (int joint_idx = 0; joint_idx < actuated_joint_names.size();
       ++joint_idx) {
    string joint_name = actuated_joint_names[joint_idx];
    MatrixXd W = gains.JointW[joint_idx] * MatrixXd::Identity(1, 1);
    MatrixXd K_p = gains.JointKp[joint_idx] * MatrixXd::Identity(1, 1);
    MatrixXd K_d = gains.JointKd[joint_idx] * MatrixXd::Identity(1, 1);
    joint_tracking_data_vec.push_back(std::make_shared<JointSpaceTrackingData>(
        joint_name + "_traj", K_p, K_d, W, plant, plant));
    joint_tracking_data_vec[joint_idx]->AddJointToTrack(joint_name,
                                                        joint_name + "dot");
    auto joint_traj = dircon_trajectory.ReconstructJointStateTrajectory(
        pos_map_wo_spr[joint_name]);
    auto joint_traj_generator = builder.AddSystem<BasicTrajectoryPassthrough>(
        joint_traj, joint_name + "_traj");
    joint_trajs.push_back(joint_traj_generator);
    osc->AddTrackingData(joint_tracking_data_vec[joint_idx].get());

    builder.Connect(joint_trajs[joint_idx]->get_output_port(),
                    osc->get_tracking_data_input_port(joint_name + "_traj"));
  }

  // Build OSC problem
  osc->Build();
  std::cout << "Built OSC" << std::endl;

  /*****Connect ports*****/

  // OSC connections
  builder.Connect(fsm->get_output_port_fsm(), osc->get_fsm_input_port());
  builder.Connect(fsm->get_output_port_impact(),
                  osc->get_near_impact_input_port());
  builder.Connect(state_receiver->get_output_port(0),
                  osc->get_robot_output_input_port());
  // FSM connections
  builder.Connect(state_receiver->get_output_port(0),
                  fsm->get_input_port_state());

  // Publisher connections
  builder.Connect(osc->get_osc_output_port(),
                  command_sender->get_input_port(0));
  builder.Connect(command_sender->get_output_port(0),
                  command_pub->get_input_port());
  builder.Connect(osc->get_osc_debug_port(), osc_debug_pub->get_input_port());

  // Run lcm-driven simulation
  // Create the diagram
  auto owned_diagram = builder.Build();
  owned_diagram->set_name(("id walking controller"));

  // Run lcm-driven simulation
  systems::LcmDrivenLoop<dairlib::lcmt_robot_output> loop(
      &lcm, std::move(owned_diagram), state_receiver, FLAGS_channel_x, true);
  loop.Simulate();

  return 0;
}
}  // namespace examples
}  // namespace dairlib

int main(int argc, char* argv[]) {
  return dairlib::examples::DoMain(argc, argv);
}
