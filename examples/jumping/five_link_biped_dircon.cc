#include <chrono>
#include <thread>

#include <gflags/gflags.h>
#include "attic/multibody/rigidbody_utils.h"

#include "drake/lcm/drake_lcm.h"
#include "drake/solvers/mathematical_program.h"
#include "drake/solvers/constraint.h"
#include "drake/solvers/snopt_solver.h"
#include "drake/solvers/solve.h"
#include "drake/solvers/solution_result.h"
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/framework/diagram.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/primitives/trajectory_source.h"
#include "drake/systems/rendering/multibody_position_to_geometry_pose.h"
#include "drake/geometry/geometry_visualization.h"
#include "drake/multibody/plant/multibody_plant.h"
#include "drake/multibody/parsing/parser.h"
#include "drake/multibody/rigid_body_plant/drake_visualizer.h"
#include "drake/multibody/parsers/urdf_parser.h"


#include "common/find_resource.h"
#include "multibody/multibody_utils.h"
#include "multibody/visualization_utils.h"
#include "systems/primitives/subvector_pass_through.h"
#include "solvers/optimization_utils.h"
#include "systems/trajectory_optimization/dircon_position_data.h"
#include "systems/trajectory_optimization/dircon_kinematic_data_set.h"
#include "systems/trajectory_optimization/hybrid_dircon.h"
#include "systems/trajectory_optimization/dircon_opt_constraints.h"

#include "examples/jumping/traj_logger.h"

DEFINE_double(realtime_factor, .5,
				"Playback speed.  See documentation for "
				"Simulator::set_target_realtime_rate() for details.");
DEFINE_double(gravity, 9.81,
				"Gravity acceleration constant");
DEFINE_double(mu_static, 0.7, "The static coefficient of friction");
DEFINE_double(mu_kinetic, 0.7, "The dynamic coefficient of friction");
DEFINE_double(v_tol, 0.01, "The maximum slipping speed allowed during stiction (m/s)");
DEFINE_double(height, 0.75, "The jump height wrt to the torso COM (m)");
DEFINE_int64(knot_points, 10, "Number of knot points per mode");

DEFINE_double(max_duration, 1, "Maximum trajectory duration (s)");
DEFINE_bool(load_previous_traj, false, "Set to true if loading in a previous trajectory");
DEFINE_string(traj_folder, "", "Set to true if loading in a previous trajectory");
// Simulation parameters.
DEFINE_double(timestep, 1e-5, "The simulator time step (s)");

using drake::multibody::MultibodyPlant;
using drake::multibody::Body;
using drake::multibody::Parser;
using drake::geometry::SceneGraph;
using drake::systems::DiagramBuilder;
using Eigen::Vector3d;
using Eigen::VectorXd;
using Eigen::MatrixXd;
using Eigen::Matrix3Xd;
using std::vector;
using drake::trajectories::PiecewisePolynomial;
using drake::solvers::SolutionResult;
using std::cout;
using std::endl;

namespace dairlib{

using systems::trajectory_optimization::HybridDircon;
using systems::trajectory_optimization::DirconDynamicConstraint;
using systems::trajectory_optimization::DirconKinematicConstraint;
using systems::trajectory_optimization::DirconOptions;
using systems::trajectory_optimization::DirconKinConstraintType;
using systems::SubvectorPassThrough;

namespace examples{
namespace jumping{

drake::trajectories::PiecewisePolynomial<double> run_traj_opt(MultibodyPlant<double>* plant,
													PiecewisePolynomial<double> init_x_traj,
													PiecewisePolynomial<double> init_u_traj,
													vector<PiecewisePolynomial<double>> init_l_traj,
													vector<PiecewisePolynomial<double>> init_lc_traj,
													vector<PiecewisePolynomial<double>> init_vc_traj){

	auto positions_map = multibody::makeNameToPositionsMap(*plant);
	auto velocities_map = multibody::makeNameToVelocitiesMap(*plant);

	// Start of constraint specification
	const Body<double>& left_lower_leg = plant->GetBodyByName("left_lower_leg");
	const Body<double>& right_lower_leg = plant->GetBodyByName("right_lower_leg");

	Vector3d pt;
	pt << 0, 0, -0.4;
	bool isXZ = true;

	auto leftFootConstraint = DirconPositionData<double>(*plant, left_lower_leg,
														 pt, isXZ);
	auto rightFootConstraint = DirconPositionData<double>(*plant, right_lower_leg,
			  											 pt, isXZ);

	Vector3d normal;
	normal << 0, 0, 1;

	// Specifies that the foot has to be on the ground with normal/friction specified by normal/mu
	leftFootConstraint.addFixedNormalFrictionConstraints(normal, FLAGS_mu_static);
	rightFootConstraint.addFixedNormalFrictionConstraints(normal, FLAGS_mu_static);

	// Constraint for each contact mode
	std::vector<DirconKinematicData<double>*> mode_one_constraints;
	mode_one_constraints.push_back(&leftFootConstraint);
	mode_one_constraints.push_back(&rightFootConstraint);
	auto mode_one_dataset = DirconKinematicDataSet<double>(*plant, &mode_one_constraints);

	// allow x pos to be some constant decision variable instead of 0
	auto mode_one_options = DirconOptions(mode_one_dataset.countConstraints());
	mode_one_options.setConstraintRelative(0, true); //left foot
	mode_one_options.setConstraintRelative(0 + (mode_one_dataset.countConstraints() / 2), true); //right foot

	// no foot contact constraints
	std::vector<DirconKinematicData<double>*> mode_two_constraints; 
	auto mode_two_dataset = DirconKinematicDataSet<double>(*plant, &mode_two_constraints);
	auto mode_two_options = DirconOptions(mode_two_dataset.countConstraints());

	auto mode_three_options = DirconOptions(mode_one_dataset.countConstraints());
	mode_three_options.setConstraintRelative(0, true); //left foot
	mode_three_options.setConstraintRelative(0 + (mode_one_dataset.countConstraints() / 2), true); //right foot

	// Specifying parameters for optimization problem
	std::vector<int> timesteps;
	timesteps.push_back(FLAGS_knot_points);
	timesteps.push_back(FLAGS_knot_points);
	timesteps.push_back(FLAGS_knot_points);
	std::vector<double> min_dt;
	min_dt.push_back(.01);
	min_dt.push_back(.01);
	min_dt.push_back(.01);
	std::vector<double> max_dt;
	max_dt.push_back(.3);
	max_dt.push_back(.3);
	max_dt.push_back(.3);

	// Add contact modes and contact decision variables to single vector for DIRCON
	std::vector<DirconKinematicDataSet<double>*> contact_mode_list;
	contact_mode_list.push_back(&mode_one_dataset);
	contact_mode_list.push_back(&mode_two_dataset);
	contact_mode_list.push_back(&mode_one_dataset);

	std::vector<DirconOptions> options_list;
	options_list.push_back(mode_one_options);
	options_list.push_back(mode_two_options);
	options_list.push_back(mode_three_options);

	// Trajectory Optimization Setup
	auto trajopt = std::make_shared<HybridDircon<double>>(*plant, 
													timesteps,
													min_dt,
													max_dt,
													contact_mode_list,
													options_list);

	trajopt->AddDurationBounds(FLAGS_max_duration, 1.5*FLAGS_max_duration);
	trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(),
							"Print file", "five_link_biped_snopt.out");
	trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(),
							"Major iterations limit", 1000);	
	trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(),
							"Iterations limit", 100000);	
	trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(),
							"Minor iterations limit", 5000);

	// if(FLAGS_load_previous_traj){
	// 	DRAKE_DEMAND(FLAGS_traj_folder != "");

	// 	init_x_traj = loadStateTrajToPP(FLAGS_traj_folder, 3);
	// 	init_u_traj = loadInputTrajToPP(FLAGS_traj_folder);
	// }

	// Set initial guesses
	for (uint j = 0; j < timesteps.size(); j++) {
		trajopt->drake::systems::trajectory_optimization::MultipleShooting::SetInitialTrajectory(init_u_traj, init_x_traj);
		trajopt->SetInitialForceTrajectory(j, 
								init_l_traj[j],
								init_lc_traj[j],
								init_vc_traj[j]);
	}

	// Set linear constraints
	int n_modes = 3;
	auto x0 = trajopt->initial_state();
	auto x_mid_point = trajopt->state(FLAGS_knot_points*n_modes/2);
	auto xf = trajopt->final_state();
	int n = plant->num_positions();

	Eigen::VectorXd fixed_initial_conds(14);
	fixed_initial_conds << 0, 0.778109, 0, -.3112, -.231, 0.427, 0.4689,
            0, 0, 0, 0, 0, 0, 0;
	trajopt->AddLinearConstraint(x0 == fixed_initial_conds);
	// trajopt->AddLinearConstraint(x0(positions_map["left_hip_pin"]) == -0.3112);
	// trajopt->AddLinearConstraint(x0(positions_map["right_hip_pin"]) == -0.231);
	// trajopt->AddLinearConstraint(x0(positions_map["left_knee_pin"]) == 0.427);
	// trajopt->AddLinearConstraint(x0(positions_map["right_knee_pin"]) == 0.4689);
	// trajopt->AddLinearConstraint(x0(positions_map["planar_z"]) == 0.7768);
	trajopt->AddLinearConstraint(x_mid_point(positions_map["planar_x"]) == (x0(positions_map["planar_x"])));
	// trajopt->AddLinearConstraint(xf(positions_map["planar_x"]) == (x0(positions_map["planar_x"])));
	trajopt->AddLinearConstraint(x_mid_point(positions_map["planar_z"]) == (x0(positions_map["planar_z"]) + FLAGS_height));
	trajopt->AddLinearConstraint(xf(positions_map["planar_z"]) == (x0(positions_map["planar_z"])));
	trajopt->AddLinearConstraint(x0.tail(n) == VectorXd::Zero(n));
	trajopt->AddLinearConstraint(xf.tail(n) == VectorXd::Zero(n));

	auto x = trajopt->state();

	trajopt->AddConstraintToAllKnotPoints(x(positions_map["planar_roty"]) >= -0.25);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["planar_roty"]) <= 0.25);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["planar_x"]) >= -0.5);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["planar_x"]) <= 0.5);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["left_knee_pin"]) >= 0.05);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["right_knee_pin"]) >= 0.05);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["left_knee_pin"]) <= 2);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["right_knee_pin"]) <= 2);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["left_hip_pin"]) >= -1.5);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["right_hip_pin"]) >= -1.5);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["left_hip_pin"]) <= 1.5);
	trajopt->AddConstraintToAllKnotPoints(x(positions_map["right_hip_pin"]) <= 1.5);
	
	// Define cost function
	const double R = 1;
	auto u = trajopt->input();
	MatrixXd Q = MatrixXd::Zero(2*n, 2*n);
	for (int i=0; i < n; i++) {
		Q(i+n, i+n) = 1;
	}
	trajopt->AddRunningCost(u.transpose()*R*u);
	trajopt->AddRunningCost(x.transpose()*Q*x);
	
	// Solve the traj optimization problem
	auto start = std::chrono::high_resolution_clock::now();
	const auto result = Solve(*trajopt, trajopt->initial_guess());
	auto finish = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = finish - start;
	std::cout << "Solve time:" << elapsed.count() << std::endl;
	std::cout << "Cost:" << result.get_optimal_cost() << std::endl;
	std::cout << "Solve result: " << result.get_solution_result() << std::endl;

	// Rerun optimization if solver reached iteration limits
	if(result.get_solution_result() == -6){
		auto pp_xtraj = trajopt->ReconstructStateTrajectory(result);
		std::vector<MatrixXd> rand_pertubation;
		for(size_t i = 0; i < pp_xtraj.get_segment_times().size(); ++i){
			rand_pertubation.push_back(
										Eigen::VectorXd::Random(plant->num_positions() + 
																plant->num_velocities())
										);
		}
		auto rand_x_pertubation = PiecewisePolynomial<double>::ZeroOrderHold(pp_xtraj.get_segment_times(), 
																			rand_pertubation);
		run_traj_opt(plant,
					pp_xtraj + rand_x_pertubation,
					// init_u_traj,
					trajopt->ReconstructInputTrajectory(result),
					init_l_traj,
					init_lc_traj,
					init_vc_traj
					);
	}


	writePPTrajToFile(trajopt->ReconstructStateTrajectory(result), "examples/jumping/saved_trajs/", "states");
	writePPTrajToFile(trajopt->ReconstructInputTrajectory(result), "examples/jumping/saved_trajs/", "inputs");
	// saveAllDecisionVars(result, "saved_trajs", "decision_vars");
	writeTimeTrajToFile(trajopt->ReconstructStateTrajectory(result), "examples/jumping/state_traj.txt");
	writeTimeTrajToFile(trajopt->ReconstructInputTrajectory(result), "examples/jumping/input_traj.txt");
	return trajopt->ReconstructStateTrajectory(result);
}

void print_state_map(MultibodyPlant<double>* plant){
	//output initial states
	auto positions_map = multibody::makeNameToPositionsMap(*plant);
	auto velocities_map = multibody::makeNameToVelocitiesMap(*plant);
	for (auto const& element : positions_map)
		cout << element.first << " = " << element.second << endl;
	for (auto const& element : velocities_map)
		cout << element.first << " = " << element.second << endl;

	return;
}

int doMain(int argc, char* argv[]){
	gflags::ParseCommandLineFlags(&argc, &argv, true);
	DiagramBuilder<double> builder;
	
	// Initialize the plant
	MultibodyPlant<double> plant;
	SceneGraph<double>& scene_graph = *(builder.AddSystem<SceneGraph>());
	Parser parser(&plant, &scene_graph);
	std::string full_name = FindResourceOrThrow("examples/jumping/five_link_biped.urdf");
	// std::string full_name = FindResourceOrThrow("examples/PlanarWalker/PlanarWalker.urdf");
	parser.AddModelFromFile(full_name);
	plant.mutable_gravity_field().set_gravity_vector(-FLAGS_gravity * Eigen::Vector3d::UnitZ());
	plant.WeldFrames(
			plant.world_frame(), 
			plant.GetFrameByName("base"),
			drake::math::RigidTransform<double>()
			);

	plant.Finalize();

	// print_state_map(&plant);

	// Generate guesses for states, inputs, and contact forces
	std::srand(time(0));  // Initialize random number generator.
	// Eigen::VectorXd x_0 = Eigen::VectorXd::Zero(plant.num_positions() + 
	// 											plant.num_velocities()
	// 											);
	Eigen::VectorXd x_0(14);
	x_0 << 0, 0.7768, 0, -0.3112, -0.231, 0.427, 0.4689,
			0, 0, 0, 0, 0, 0, 0;


	Eigen::VectorXd init_l_vec(2);
	init_l_vec << 0, 15*FLAGS_gravity;
	int num_forces = 4;
	int num_timesteps = FLAGS_knot_points;

	int n_modes = 3;

	// Initial states, forces, and constraints
	std::vector<MatrixXd> init_x; // states
	std::vector<MatrixXd> init_u; // forces
	std::vector<PiecewisePolynomial<double>> init_l_traj; // contact forces at knot points
	std::vector<PiecewisePolynomial<double>> init_lc_traj; // contact forces at collocation points
	std::vector<PiecewisePolynomial<double>> init_vc_traj; // velocity constraint at collocation points

	double time_constant = 0.2;

	std::vector<double> init_time;
	for (int i = 0; i < n_modes*num_timesteps - (n_modes - 1); ++i){
		init_time.push_back(i*time_constant);
		init_x.push_back(x_0);
		init_u.push_back(VectorXd::Random(num_forces));
	}

	auto init_x_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time, init_x);
	auto init_u_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time, init_u);

	for (int j = 0; j < n_modes; ++j){
		std::vector<MatrixXd> init_l_j;
		std::vector<MatrixXd> init_lc_j;
		std::vector<MatrixXd> init_vc_j;
		std::vector<double> init_time_j;
		for (int i = 0; i < num_timesteps; ++i) {
			init_time_j.push_back(i*time_constant);
			init_l_j.push_back(init_l_vec);
			init_lc_j.push_back(init_l_vec);
			init_vc_j.push_back(VectorXd::Zero(2));
		}

		auto init_l_traj_j = PiecewisePolynomial<double>::ZeroOrderHold(init_time_j, init_l_j);
		auto init_lc_traj_j = PiecewisePolynomial<double>::ZeroOrderHold(init_time_j, init_l_j);
		auto init_vc_traj_j = PiecewisePolynomial<double>::ZeroOrderHold(init_time_j, init_l_j);
	
		init_l_traj.push_back(init_l_traj_j);
		init_lc_traj.push_back(init_lc_traj_j);
		init_vc_traj.push_back(init_vc_traj_j);
	}
	// End of initalization

	drake::trajectories::PiecewisePolynomial<double> optimal_traj = run_traj_opt(&plant,
																				init_x_traj,
																				init_u_traj,
																				init_l_traj,
																				init_lc_traj,
																				init_vc_traj
																				);
	// const drake::trajectories::PiecewisePolynomial<double> pp_xtraj = optimal_traj->ReconstructStateTrajectory(result);
	multibody::connectTrajectoryVisualizer(	&plant, 
											&builder, 
											&scene_graph,
											optimal_traj
											);

	auto diagram = builder.Build();

	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(2));
		drake::systems::Simulator<double> simulator(*diagram);

		simulator.set_target_realtime_rate(FLAGS_realtime_factor);
		simulator.Initialize();
		simulator.AdvanceTo(optimal_traj.end_time());
	}

	return 0;
}

}  // namespace jumping
}  // namespace examples
}  // namespace dairlib

int main(int argc, char* argv[]) { 
	return dairlib::examples::jumping::doMain(argc, argv); 
}
