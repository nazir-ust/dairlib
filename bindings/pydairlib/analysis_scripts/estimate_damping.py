import sys

import lcm
import matplotlib
import matplotlib.pyplot as plt
import numpy as np
from numpy.linalg import inv
import process_lcm_log
import pathlib
from pydrake.multibody.parsing import Parser
from pydrake.multibody.plant import AddMultibodyPlantSceneGraph
from pydrake.multibody.tree import JacobianWrtVariable
from pydrake.systems.framework import DiagramBuilder
from pydrake.solvers import mathematicalprogram as mp
from pydrake.solvers.osqp import OsqpSolver
import pydairlib.lcm_trajectory
import pydairlib.multibody
from pydairlib.multibody.kinematic import DistanceEvaluator
from pydairlib.cassie.cassie_utils import *
from pydairlib.common import FindResourceOrThrow


def main():
  global l_knee_spring_idx, r_knee_spring_idx, l_heel_spring_idx, r_heel_spring_idx
  global plant, context, world, l_toe_frame, r_toe_frame
  global front_contact_disp, rear_contact_disp
  global l_knee_idx, r_knee_idx, l_heel_idx, r_heel_idx
  global sample_times
  global nq, nv, nx, nu
  global filename
  global t_u_slice
  global t_slice
  global pos_map
  global vel_map
  global act_map
  global l_loop_closure, r_loop_closure
  global x_datatypes, u_datatypes

  builder = DiagramBuilder()
  plant, scene_graph = AddMultibodyPlantSceneGraph(builder, 0.0)

  # Parser(plant).AddModelFromFile(
  #   FindResourceOrThrow(
  #     "examples/Cassie/urdf/cassie_v2.urdf"))
  # pydairlib.cassie.cassie_utils.addCassieMultibody(plant, scene_graph, False,
  #   "examples/Cassie/urdf/cassie_v2.urdf", True, True)
  pydairlib.cassie.cassie_utils.addCassieMultibody(plant, scene_graph, False,
    "examples/Cassie/urdf/cassie_v2.urdf", False, False)
  plant.Finalize()

  # relevant MBP parameters
  nq = plant.num_positions()
  nv = plant.num_velocities()
  nx = plant.num_positions() + plant.num_velocities()
  nu = plant.num_actuators()

  l_toe_frame = plant.GetBodyByName("toe_left").body_frame()
  r_toe_frame = plant.GetBodyByName("toe_right").body_frame()
  world = plant.world_frame()
  context = plant.CreateDefaultContext()

  front_contact_disp = np.array((-0.0457, 0.112, 0))
  rear_contact_disp = np.array((0.088, 0, 0))

  pos_map = pydairlib.multibody.makeNameToPositionsMap(plant)
  vel_map = pydairlib.multibody.makeNameToVelocitiesMap(plant)
  act_map = pydairlib.multibody.makeNameToActuatorsMap(plant)

  l_knee_spring_idx = pos_map["knee_joint_left"]
  r_knee_spring_idx = pos_map["knee_joint_right"]
  l_heel_spring_idx = pos_map["ankle_spring_joint_left"]
  r_heel_spring_idx = pos_map["ankle_spring_joint_right"]
  l_knee_idx = pos_map["knee_left"]
  r_knee_idx = pos_map["knee_right"]
  l_heel_idx = pos_map["ankle_joint_left"]
  r_heel_idx = pos_map["ankle_joint_right"]

  x_datatypes = pydairlib.multibody.createStateNameVectorFromMap(plant)
  u_datatypes = pydairlib.multibody.createActuatorNameVectorFromMap(plant)

  l_loop_closure = LeftLoopClosureEvaluator(plant)
  r_loop_closure = RightLoopClosureEvaluator(plant)

  filename = sys.argv[1]
  controller_name = sys.argv[2]
  log = lcm.EventLog(filename, "r")
  path = pathlib.Path(filename).parent
  filename = filename.split("/")[-1]
  joint_name = filename[7:filename.find('_sine')]

  matplotlib.rcParams["savefig.directory"] = path

  x, u_meas, t_x, u, t_u, contact_info, contact_info_locs, t_contact_info, \
  osc_debug, fsm, estop_signal, switch_signal, t_controller_switch, t_pd, kp, kd, cassie_out, u_pd, t_u_pd, \
  osc_output, full_log = process_lcm_log.process_log(log, pos_map, vel_map, act_map, controller_name)

  # Will need to manually select the data range
  t_start = t_x[1000]
  t_end = t_x[-1000]
  t_start_idx = np.argwhere(np.abs(t_x - t_start) < 1e-3)[0][0]
  t_end_idx = np.argwhere(np.abs(t_x - t_end) < 1e-3)[0][0]
  t_slice = slice(t_start_idx, t_end_idx)
  start_time_idx = np.argwhere(np.abs(t_u - t_start) < 1e-3)[0][0]
  end_time_idx = np.argwhere(np.abs(t_u - t_end) < 1e-3)[0][0]
  t_u_slice = slice(start_time_idx, end_time_idx)
  # sample_times = [46.0, 58.5, 65.0, 70.2, 74.8, 93.1, 98.7]

  # import pdb; pdb.set_trace()
  # plot_state(x, t_x, u, t_u, x_datatypes, u_datatypes)
  # plot_state(x, t_x, u, t_u, u_meas, x_datatypes, u_datatypes, act_map[joint_name + '_motor'])
  # plt.show()
  xdot = estimate_xdot_with_filtering(x, t_x)

  solve_individual_joint(x, xdot, t_x, u_meas, pos_map[joint_name], act_map[joint_name + '_motor'])
  # solve_with_lambda(x, xdot, t_x, u_meas)
  plt.show()

  # solve_with_lambda(x, t_x, u_meas)


def estimate_xdot_with_filtering(x, t_x):
  xdot = np.zeros((x.shape))

  vdot = np.diff(x[:, -plant.num_velocities():], axis=0, prepend=x[0:1, -nv:])
  dt = np.diff(t_x, axis=0, prepend=t_x[0] - 1e-4)
  for i in range(plant.num_velocities()):
    vdot[:, i] = vdot[:, i] / dt

  filter = 50
  idx = int(filter / 2)
  for i in range(idx, dt.shape[0] - idx):
    vdot[i, :] = np.average(vdot[i - idx:i+idx, :])

  # plt.plot(t_x[t_slice], vdot[t_slice])
  # plt.show()
  return np.hstack((x[:, :nv], vdot))


def plot_force_residual(t_x, x, xdot, u_meas, joint_idx, act_idx):

  pass


def solve_individual_joint(x, xdot, t_x, u_meas, joint_idx, act_idx):
  n_samples = 1000

  x_samples = []
  u_samples = []
  xdot_samples = []
  t_samples = []

  nvars = 1
  tau_res = np.zeros((n_samples, nv))
  tau_res_wo_damping = np.zeros((n_samples, nv))
  tau_res_wo_springs = np.zeros((n_samples, nv))
  generalized_force = np.zeros((n_samples, nv))
  Bu_force = np.zeros((n_samples, nv))
  Cv_force = np.zeros((n_samples, nv))
  g_force = np.zeros((n_samples, nv))
  J_lambda = np.zeros((n_samples, nv))
  J_lambda_spring = np.zeros((n_samples, nv))
  K_force = np.zeros((n_samples, nv))
  Jv = np.zeros((n_samples, 2))

  A = np.zeros((n_samples, nvars))
  b = np.zeros(n_samples)
  for i in range(n_samples):
    t = t_x[50] + 1e-2 * i
    ind = np.argwhere(np.abs(t_x - t) < 1e-3)[0][0]
    x_samples.append(x[ind, :])
    xdot_samples.append(xdot[ind, :])
    u_samples.append(u_meas[ind, :])
    t_samples.append(t)
    plant.SetPositionsAndVelocities(context, x[ind, :])

    M = plant.CalcMassMatrixViaInverseDynamics(context)
    M_inv = inv(M)
    B = plant.MakeActuationMatrix()
    g = plant.CalcGravityGeneralizedForces(context)
    Cv = plant.CalcBiasTerm(context)
    J_l_loop_closure = l_loop_closure.EvalFullJacobian(context)
    J_r_loop_closure = r_loop_closure.EvalFullJacobian(context)
    J = np.vstack((J_l_loop_closure, J_r_loop_closure))
    JdotV_l_loop_closure = l_loop_closure.EvalFullJacobianDotTimesV(context)
    JdotV_r_loop_closure = r_loop_closure.EvalFullJacobianDotTimesV(context)
    JdotV = np.vstack((JdotV_l_loop_closure, JdotV_r_loop_closure))
    JdotV = np.reshape(JdotV, (2,))

    A[i, 0] = -x[ind, nq + joint_idx]
    b[i] = M[joint_idx, joint_idx] * xdot[ind, nq + joint_idx] - u_meas[ind, act_idx]

    qdot = x[ind, nq:]
    qddot = xdot[ind, nq:]
    K = np.zeros((nq, nq))
    K[l_knee_spring_idx, l_knee_spring_idx] = 1500
    K[r_knee_spring_idx, r_knee_spring_idx] = 1500
    K[l_heel_spring_idx, l_heel_spring_idx] = 1250
    K[r_heel_spring_idx, r_heel_spring_idx] = 1250
    # K[l_knee_spring_idx, l_knee_spring_idx] = 1000
    # K[r_knee_spring_idx, r_knee_spring_idx] = 1000
    # K[l_heel_spring_idx, l_heel_spring_idx] = 1000
    # K[r_heel_spring_idx, r_heel_spring_idx] = 1000
    K = -K
    D = np.zeros((nv, nv))
    # D[joint_idx, joint_idx] = -2.0/3

    # Compute force residuals
    lambda_implicit =            inv(J @ M_inv @ J.T) @ (- J @ M_inv @ (-Cv + g + B @ u_meas[ind] + K@x[ind, :nq] + D@qdot) - JdotV)
    lambda_implicit_wo_damping = inv(J @ M_inv @ J.T) @ (- J @ M_inv @ (-Cv + g + B @ u_meas[ind] + K@x[ind, :nq])          - JdotV)
    lambda_implicit_wo_spring =  inv(J @ M_inv @ J.T) @ (- J @ M_inv @ (-Cv + g + B @ u_meas[ind] + D@qdot)                 - JdotV)
    lambda_implicit_spring =  inv(J @ M_inv @ J.T) @ (- J @ M_inv @( K @ x[ind, :nq]))
    tau_res[i] =            M @ qddot + Cv - B @ u_meas[ind] - g - J.T @ lambda_implicit            - K@x[ind, :nq] - D@qdot
    tau_res_wo_damping[i] = M @ qddot + Cv - B @ u_meas[ind] - g - J.T @ lambda_implicit_wo_damping - K@x[ind, :nq]
    tau_res_wo_springs[i] = M @ qddot + Cv - B @ u_meas[ind] - g - J.T @ lambda_implicit_wo_spring  - D@qdot

    Jv[i] = J@qdot

    generalized_force[i] = M @ qddot
    Bu_force[i] = B@u_meas[ind]
    Cv_force[i] = Cv
    g_force[i] = g
    J_lambda[i] = J.T @ lambda_implicit
    J_lambda_spring[i] = J.T @ lambda_implicit_spring
    K_force[i] = K @ x[ind, :nq]

  x_samples = np.array(x_samples)
  xdot_samples = np.array(xdot_samples)
  u_samples = np.array(u_samples)

  plt.figure("force contribution")

  plt.plot(t_samples, generalized_force[:, joint_idx])
  plt.plot(t_samples, Bu_force[:, joint_idx])
  plt.plot(t_samples, Cv_force[:, joint_idx])
  plt.plot(t_samples, g_force[:, joint_idx])
  plt.plot(t_samples, J_lambda[:, joint_idx])
  plt.plot(t_samples, K_force[:, joint_idx])
  plt.plot(t_samples, tau_res[:, joint_idx])
  plt.plot(t_samples, J_lambda_spring[:, joint_idx])
  plt.legend(['Mqddot', 'Bu', 'Cv', 'g', 'J.T lambda', "J.T lambda_spring", 'Kq', 'residual'])

  plt.figure("Jv")

  plt.plot(t_samples, Jv)


  plt.figure("force residual position x-axis: " + filename)
  plt.plot(x_samples[:, joint_idx], tau_res[:, joint_idx], 'b.')
  # plt.plot(x_samples[:, joint_idx], tau_res_wo_damping[:, joint_idx], 'r.')
  # plt.plot(x_samples[:, joint_idx], tau_res_wo_springs[:, joint_idx], 'g.')
  plt.xlabel('joint position (rad)')
  plt.ylabel('generalized force error (Nm)')

  plt.figure("force residual velocity x-axis: " + filename)
  plt.plot(x_samples[:, nq + joint_idx], tau_res[:, joint_idx], 'b.')
  plt.xlabel('joint velocity (rad/s)')
  plt.ylabel('generalized force error (Nm)')
  # plt.plot(x_samples[:, nq + joint_idx], tau_res_wo_springs[:, joint_idx], 'g.')
  # plt.plot(x_samples[:, nq + joint_idx], tau_res_wo_damping[:, joint_idx], 'r.')
  # plt.legend()


  # plt.figure("force res vs time: " + filename)
  # plt.plot(t_samples, tau_res[:, joint_idx], '-')
  # plt.plot(t_samples, x_samples[:, nq + joint_idx], 'r-')
  # plt.xlabel('time (s)')

  prog = mp.MathematicalProgram()
  x_vars = prog.NewContinuousVariables(nvars, "sigma")
  prog.AddL2NormCost(A, b, x_vars)
  solver = OsqpSolver()
  result = solver.Solve(prog, None, None)
  print("LSTSQ cost: ", result.get_optimal_cost())
  print("Solution result: ", result.get_solution_result())
  sol = result.GetSolution()
  print(sol)

def solve_with_lambda(x, xdot, t_x, u_meas):
  # n_samples = len(sample_times)
  n_samples = 1000
  n_k = 4
  n_damping_vars = 23 - 7
  nvars = n_k + n_damping_vars

  A = np.zeros((n_samples * nv, nvars))
  b = np.zeros(n_samples * nv)

  x_samples = []
  u_samples = []
  xdot_samples = []
  t_samples = []

  for i in range(n_samples):
    # delta_t = 1e-2 * i
    t = t_x[10] + 1e-2 * i
    x_ind = np.argwhere(np.abs(t_x - t) < 1e-3)[0][0]
    x_samples.append(x[x_ind, :])
    xdot_samples.append(xdot[x_ind, :])
    u_samples.append(u_meas[x_ind, :])
    t_samples.append(t)
    plant.SetPositionsAndVelocities(context, x[x_ind, :])

    M = plant.CalcMassMatrixViaInverseDynamics(context)
    M_inv = np.linalg.inv(M)
    B = plant.MakeActuationMatrix()
    g = plant.CalcGravityGeneralizedForces(context)
    Cv = plant.CalcBiasTerm(context)

    J_l_loop_closure = l_loop_closure.EvalFullJacobian(context)
    J_r_loop_closure = r_loop_closure.EvalFullJacobian(context)
    J = np.vstack((J_l_loop_closure, J_r_loop_closure))

    row_start = i * (nv)
    row_end = (i+1) * (nv)

    lambda_i_wo_vars = - np.linalg.inv(J @ M_inv @ J.T) @ (J @ M_inv) @ (B @ u_meas[x_ind] + g - Cv)
    lambda_i_w_vars = - np.linalg.inv(J @ M_inv @ J.T) @ (J @ M_inv)

    # Spring indices
    A[row_start + l_knee_idx, 0] = x[x_ind, l_knee_spring_idx]
    A[row_start + r_knee_idx, 1] = x[x_ind, r_knee_spring_idx]
    A[row_start + l_heel_idx, 2] = x[x_ind, l_heel_spring_idx]
    A[row_start + r_heel_idx, 3] = x[x_ind, r_heel_spring_idx]
    A[row_start : row_end, 0] += (J.T @ lambda_i_w_vars)[:, l_knee_idx] * x[x_ind, l_knee_spring_idx]
    A[row_start : row_end, 1] += (J.T @ lambda_i_w_vars)[:, r_knee_idx] * x[x_ind, r_knee_spring_idx]
    A[row_start : row_end, 2] += (J.T @ lambda_i_w_vars)[:, l_heel_idx] * x[x_ind, l_heel_spring_idx]
    A[row_start : row_end, 3] += (J.T @ lambda_i_w_vars)[:, r_heel_idx] * x[x_ind, r_heel_spring_idx]
    # Damping indices
    A[row_start:row_end, -n_damping_vars:] = np.diag([x[x_ind, nq:]])
    A[row_start:row_end, -n_damping_vars:] = np.diag((J.T @ lambda_i_w_vars) @ x[x_ind, nq:])

    # Lambda indices
    b[row_start:row_end] = M @ xdot[x_ind, -nv:] + Cv - B @ u_meas[x_ind] - g - J.T @ lambda_i_wo_vars


  x_samples = np.array(x_samples)
  xdot_samples = np.array(xdot_samples)
  plot_samples = False
  if plot_samples:
    plt.plot(t_samples, x_samples[:, pos_map['toe_left']])
    plt.plot(t_samples, x_samples[:, nq + vel_map['toe_leftdot']])
    plt.plot(t_samples, xdot_samples[:, nq + vel_map['toe_leftdot']])
    plt.show()

  prog = mp.MathematicalProgram()
  x_vars = prog.NewContinuousVariables(nvars, "sigma")
  prog.AddL2NormCost(A, b, x_vars)
  solver_id = mp.ChooseBestSolver(prog)
  print(solver_id.name())
  solver = mp.MakeSolver(solver_id)
  result = solver.Solve(prog, None, None)
  print("LSTSQ cost: ", result.get_optimal_cost())
  print("Solution result: ", result.get_solution_result())
  sol = result.GetSolution()
  k_sol = sol[0:n_k]
  d_sol = sol[n_k:n_k + n_damping_vars]

  D = np.diag(d_sol)
  f_samples = []
  for i in range(n_samples):
    plant.SetPositionsAndVelocities(context, x_samples[i, :])

    M = plant.CalcMassMatrixViaInverseDynamics(context)
    M_inv = np.linalg.inv(M)
    B = plant.MakeActuationMatrix()
    g = plant.CalcGravityGeneralizedForces(context)
    Cv = plant.CalcBiasTerm(context)
    J_l_loop_closure = l_loop_closure.EvalFullJacobian(context)
    J_r_loop_closure = r_loop_closure.EvalFullJacobian(context)
    J = np.vstack((J_l_loop_closure, J_r_loop_closure))

    lambda_implicit = np.li(J @ M_inv @ J.T)

    f_samples.append(M@xdot_samples[i, nq:] + B@u_samples[i] + Cv - g - J.T @ (J @ M_inv @ J.T) @ (J @ M_inv) @ (B @ u_meas[x_ind] + g - Cv + D @ x_samples[i, nv:]))

  f_samples = np.array(f_samples)
  plt.figure(3)
  plt.plot(t_samples, f_samples)

  print("K: ", k_sol)
  print("D: ", d_sol)
  import pdb; pdb.set_trace()

def plot_state(x, t_x, u, t_u, u_meas, x_datatypes, u_datatypes, act_idx):
  pos_indices = slice(0, nq)
  vel_indices = slice(nq, nx)
  u_indices = act_idx
  # overwrite
  # pos_indices = [pos_map["knee_joint_right"], pos_map["ankle_spring_joint_right"]]
  # pos_indices = tuple(slice(x) for x in pos_indices)

  plt.figure("positions: " + filename)
  # plt.plot(t_x[t_slice], x[t_slice, pos_map["knee_joint_right"]])
  # plt.plot(t_x[t_slice], x[t_slice, pos_map["ankle_spring_joint_right"]])
  plt.plot(t_x[t_slice], x[t_slice, pos_indices])
  plt.legend(x_datatypes[pos_indices])
  plt.figure("velocities: " + filename)
  plt.plot(t_x[t_slice], x[t_slice, vel_indices])
  plt.legend(x_datatypes[vel_indices])
  # plt.plot(sample_times, np.zeros((len(sample_times),)), 'k*')
  # plt.figure("efforts: " + filename)
  # plt.plot(t_u[t_u_slice], u[t_u_slice, u_indices])
  # plt.legend(u_datatypes[u_indices])
  plt.figure("efforts meas: " + filename)
  plt.plot(t_x[t_slice], u_meas[t_slice, u_indices])
  plt.legend(u_datatypes[u_indices])


if __name__ == "__main__":
  main()