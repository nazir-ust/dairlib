import numpy as np
import lcm
from scipy.integrate import trapz

from pydairlib.common import FindResourceOrThrow
from bindings.pydairlib.parameter_studies.plot_styler import PlotStyler
from pydrake.trajectories import PiecewisePolynomial
import pydairlib.lcm_trajectory
from bindings.pydairlib.analysis_scripts.process_lcm_log import process_log
from pydrake.multibody.plant import AddMultibodyPlantSceneGraph
from pydrake.systems.framework import DiagramBuilder
from pydairlib.cassie.cassie_utils import *
import pydairlib.multibody

1
import matplotlib.pyplot as plt


def main():
  global ps
  global nominal_impact_time
  global impact_time
  global figure_directory
  global data_directory
  global terrain_heights
  global perturbations
  global penetration_allowances
  global threshold_durations
  data_directory = '/home/yangwill/Documents/research/projects/invariant_impacts/data/'
  figure_directory = '/home/yangwill/Documents/research/projects/invariant_impacts/figures/'
  ps = PlotStyler()
  ps.set_default_styling(directory=figure_directory)

  filename = FindResourceOrThrow("examples/Cassie/saved_trajectories/jumping_0.15h_0.3d")
  jumping_traj = pydairlib.lcm_trajectory.DirconTrajectory(filename)
  output_trajs = pydairlib.lcm_trajectory.LcmTrajectory(
    "/home/yangwill/workspace/dairlib/examples/Cassie/saved_trajectories/jumping_0.15h_0.3d_processed")
  lcm_right_foot_traj = output_trajs.GetTrajectory("right_foot_trajectory0")
  right_foot_traj = PiecewisePolynomial.CubicHermite(lcm_right_foot_traj.time_vector, lcm_right_foot_traj.datapoints[0:3], lcm_right_foot_traj.datapoints[3:6])
  for mode in range(1, 3):
    lcm_right_foot_traj = output_trajs.GetTrajectory("right_foot_trajectory" + str(mode))
    right_foot_traj.ConcatenateInTime(PiecewisePolynomial.CubicHermite(lcm_right_foot_traj.time_vector, lcm_right_foot_traj.datapoints[0:3], lcm_right_foot_traj.datapoints[3:6]))\

  input_traj = jumping_traj.ReconstructInputTrajectory()
  state_traj = jumping_traj.ReconstructStateTrajectory()
  nominal_impact_time = jumping_traj.GetStateBreaks(2)[0]

  impact_time = nominal_impact_time + 2.0
  # impact_time = nominal_impact_time

  terrain_heights = np.arange(0.00, 0.030, 0.005)
  penetration_allowances = np.array([1e-5, 1e-4, 1e-3])
  durations = np.arange(0.000, 0.125, 0.025)
  perturbations = np.arange(-0.500, 0.600, 0.100)

  # For MUJOCO
  threshold_durations = np.arange(0.00, 0.11, 0.01)
  # penetration_allowances = np.array([1e-5])

  # Plotting options
  duration = '0.000'
  # duration = 'stiff'


  construct_hardware_torque_plot()
  # plot_vel_discontinuity_example(right_foot_traj)
  # construct_knee_efforts_plot()
  # for d in durations:
  #   load_logs('%.3f' % d)

  # for d in durations:
  #   print('%.3f' % d)
    # count_successful_jumps('%.3f' % d, 'zvel_')
    # count_successful_jumps('%.3f' % d)

  # count_successful_jumps(duration)
  # construct_knee_torque_bands_plot()
  # ps.add_legend(['%.0f (ms)' % (d*1e3) for d in durations])
  # ps.show_fig()


def plot_vel_discontinuity_example(traj):
  times = np.arange(nominal_impact_time - 0.1, nominal_impact_time + 0.1, 0.001)
  accel = np.zeros((times.shape[0], 3))
  pos = np.zeros((times.shape[0], 3))
  vel = np.zeros((times.shape[0], 3))
  vel_err = np.zeros((times.shape[0], 3))
  accum_err = -0.2
  for i in range(times.shape[0]):
    accel[i, :] = traj.EvalDerivative(times[i], 2)[:, 0]
    pos[i, :] = traj.value(times[i])[:, 0]
    vel[i, :] = traj.EvalDerivative(times[i], 1)[:, 0]
    if(times[i] > nominal_impact_time - 0.01):
      accum_err = 0
    vel_err[i, :] = traj.EvalDerivative(times[i] + 0.01, 1)[:, 0] + accum_err
    # vel_err[i, :] = traj.EvalDerivative(times[i] + 0.01, 1)[:, 0]
    # if(np.mod(i, 5) == 0):
    accum_err += 0.1*(vel[i] - vel_err[i])
    # accum_err *= 0.6
  plt.figure("Velocity tracking during impact")
  ps.plot(times - nominal_impact_time, vel[:, 2], xlabel='time since nominal impact time', ylabel='velocity (m/s)', linestyle=ps.blue)
  ps.plot(times - nominal_impact_time, vel_err[:, 2], linestyle=ps.red)
  ps.add_legend(['target velocity', 'actual velocity'])
  ps.save_fig('velocity_tracking_during_impact.png')
  plt.figure("Velocity error")
  ps.plot(times - nominal_impact_time, vel[:, 2] - vel_err[:, 2], xlabel='time since nominal impact time', ylabel='velocity (m/s)', linestyle=ps.yellow)
  ps.add_legend(['feedback error'])
  ps.save_fig('velocity_error_during_impact.png')
  # ps.plot([nominal_impact_time, nominal_impact_time], [-5, 5], '--')

  # plt.legend(['x','y','z'])

def load_logs(duration):
  builder = DiagramBuilder()

  plant_w_spr, scene_graph_w_spr = AddMultibodyPlantSceneGraph(builder, 0.0)
  pydairlib.cassie.cassie_utils.addCassieMultibody(plant_w_spr, scene_graph_w_spr, True,
                                                   "examples/Cassie/urdf/cassie_v2.urdf", False, False)
  plant_w_spr.Finalize()
  controller_channel = 'OSC_JUMPING'

  pos_map = pydairlib.multibody.makeNameToPositionsMap(plant_w_spr)
  vel_map = pydairlib.multibody.makeNameToVelocitiesMap(plant_w_spr)
  act_map = pydairlib.multibody.makeNameToActuatorsMap(plant_w_spr)

  nx = plant_w_spr.num_positions() + plant_w_spr.num_velocities()
  nu = plant_w_spr.num_actuators()

  osc_traj = "com_traj"

  # For full jumping traj
  t_samples = 19000
  u_samples = 9000
  # For mujoco
  # t_samples = 10000
  # u_samples = 5000
  # For pelvis zvel perturbation
  # t_samples = 6000
  # u_samples = 3000
  # parameter_dim = perturbations.shape[0]
  parameter_dim = terrain_heights.shape[0]
  t_matrix = np.zeros((parameter_dim, penetration_allowances.shape[0], t_samples))
  x_matrix = np.zeros((parameter_dim, penetration_allowances.shape[0], t_samples, nx))
  t_u_matrix = np.zeros((parameter_dim, penetration_allowances.shape[0], u_samples))
  u_matrix = np.zeros((parameter_dim, penetration_allowances.shape[0], u_samples, nu))
  t_osc_matrix = np.zeros((parameter_dim, penetration_allowances.shape[0], u_samples))
  osc_yddot_des_matrix = np.zeros((parameter_dim, penetration_allowances.shape[0], u_samples, 3))
  osc_yddot_cmd_matrix = np.zeros((parameter_dim, penetration_allowances.shape[0], u_samples, 3))
  folder_path = '/home/yangwill/Documents/research/projects/cassie/sim/jumping/logs/param_studies/' + duration + '/'
  # folder_path = '/home/yangwill/Documents/research/projects/cassie/sim/jumping/logs/param_studies/mujoco/' + duration + '/'

  for i in range(terrain_heights.shape[0]):
  # for i in range(perturbations.shape[0]):
  # for i in range(threshold_durations.shape[0]):
    for j in range(penetration_allowances.shape[0]):
      # Mujoco logs
      # log_suffix = 'duration_%.3f_mujoco' % threshold_durations[i]
      # Drake logs
      log_suffix = 'height_%.4f-stiff_%.5f' % (terrain_heights[i], penetration_allowances[j])
      # log_suffix = 'pelvis_zvel_%.3f' % perturbations[i]

      log_path = folder_path + 'lcmlog-' + log_suffix

      print(log_path)
      log = lcm.EventLog(log_path, "r")
      x, u_meas, t_x, u, t_u, contact_info, contact_info_locs, t_contact_info, \
      osc_debug, fsm, estop_signal, switch_signal, t_controller_switch, t_pd, kp, kd, cassie_out, u_pd, t_u_pd, \
      osc_output, full_log, t_lcmlog_u = process_log(log, pos_map, vel_map, act_map, controller_channel)

      t_matrix[i, j, :] = t_x[:t_samples]
      x_matrix[i, j, :, :] = x[:t_samples]
      t_u_matrix[i, j, :] = t_u[:u_samples]
      u_matrix[i, j, :, :] = u[:u_samples]
      t_osc_matrix[i, j, :] = osc_debug[osc_traj].t[:u_samples]
      osc_yddot_des_matrix[i, j, :, :] = osc_debug[osc_traj].yddot_des[:u_samples, :]
      osc_yddot_cmd_matrix[i, j, :, :] = osc_debug[osc_traj].yddot_command[:u_samples, :]

  np.save(data_directory + 't_x_' + duration, t_matrix)
  np.save(data_directory + 'x_' + duration, x_matrix)
  np.save(data_directory + 't_u_' + duration, t_u_matrix)
  np.save(data_directory + 'u_' + duration, u_matrix)
  np.save(data_directory + 't_osc_com_' + duration, t_osc_matrix)
  np.save(data_directory + 'osc_com_accel_des_' + duration, osc_yddot_des_matrix)
  np.save(data_directory + 'osc_com_accel_cmd_' + duration, osc_yddot_cmd_matrix)
  return


def count_successful_jumps(duration, param = ''):
  t_matrix = np.load(data_directory + 't_x_' + param + duration + '.npy')
  x_matrix = np.load(data_directory + 'x_' + param + duration + '.npy')
  t_u_matrix = np.load(data_directory + 't_u_' + param + duration + '.npy')
  u_matrix = np.load(data_directory + 'u_' + param + duration + '.npy')
  t_osc_matrix = np.load(data_directory + 't_osc_com_' + duration + '.npy')
  osc_yddot_des_matrix = np.load(data_directory + 'osc_com_accel_des_' + param + duration + '.npy')
  osc_yddot_cmd_matrix = np.load(data_directory + 'osc_com_accel_cmd_' + param + duration + '.npy')

  # steady_state_time = 0.7
  steady_state_time = 3.0
  max_adj_window = 0.100
  accel_error_time = impact_time + max_adj_window
  success_height = 0.75
  z_fb_idx = 6
  successes = np.zeros((t_matrix.shape[0], t_matrix.shape[1]))
  efforts = np.zeros((t_matrix.shape[0], t_matrix.shape[1]))
  max_efforts = np.zeros((t_matrix.shape[0], t_matrix.shape[1]))
  accel_error = np.zeros((t_matrix.shape[0], t_matrix.shape[1]))
  u_slice = slice(0, 10)

  for i in range(terrain_heights.shape[0]):
    # for i in range(perturbations.shape[0]):
    for j in range(penetration_allowances.shape[0]):
      t_idx = np.argwhere(t_matrix[i, j] == steady_state_time)[0, 0]
      t_u_start_idx = np.argwhere(np.abs(t_u_matrix[i, j] - (impact_time - 0.1)) < 2e-3)[0, 0]
      t_u_end_idx = np.argwhere(np.abs(t_u_matrix[i, j] - (impact_time + 0.1)) < 2e-3)[0, 0]
      t_u_eval_idx = np.argwhere(np.abs(t_osc_matrix[i, j] - accel_error_time) < 2e-3)[0, 0]
      # if (x_matrix[i, j, t_idx, z_fb_idx] > (success_height + terrain_heights[i])):
      #   successes[i, j] = 1
      t_u_slice = slice(t_u_start_idx, t_u_end_idx)
      efforts[i, j] = trapz(np.square(np.sum(u_matrix[i, j, t_u_slice, u_slice], axis=1)),
                            t_u_matrix[i, j, t_u_slice])
      max_efforts[i, j] = np.max(u_matrix[i, j, t_u_slice, u_slice])
      accel_error[i, j] = np.linalg.norm(osc_yddot_des_matrix[i, j, t_u_eval_idx, :] - osc_yddot_cmd_matrix[i, j, t_u_eval_idx, :])

  # import pdb; pdb.set_trace()

  # ps.plot(terrain_heights, np.average(max_efforts, axis=1), xlabel='terrain height (m)', ylabel='actuator saturation (% of max)')
  ps.plot(terrain_heights, np.average(accel_error, axis=1), xlabel='terrain height (m)', ylabel='average pelvis acceleration error (% of max)')
  print(np.mean(max_efforts))
  print('mean:' )
  print(np.mean(accel_error))
  print('median:' )
  print(np.median(accel_error))
  print('stdev:' )
  print(np.std(accel_error))
  return


def construct_knee_efforts_plot():
  t_u_no_adjustment = np.load('t_u_0.000.npy')
  t_u_with_adjustment = np.load('t_u_0.100.npy')
  u_no_adjustment = np.load('u_combined_efforts_0.000.npy')
  u_with_adjustment = np.load('u_combined_efforts_0.100.npy')
  t_u_no_adjustment -= impact_time
  t_u_with_adjustment -= impact_time
  t_u_no_adjustment *= 1e3
  t_u_with_adjustment *= 1e3
  ps.plot(t_u_no_adjustment, u_no_adjustment, ylim=[-50, 600], xlabel='time since impact (ms)',
          ylabel='motor torque (Nm)',
          title='combined knee motor torques')
  ps.plot(t_u_with_adjustment, u_with_adjustment, ylim=[-50, 600], xlabel='time since impact (ms)',
          ylabel='motor torque (Nm)',
          title='combined knee motor torques')
  ps.add_legend(['No adjustment', '200 ms window around the nominal impact time'])
  ps.show_fig()


def construct_knee_torque_bands_plot():
  # durations = np.array([0.000, 0.025, 0.050, 0.075, 0.100])
  durations = np.array([0.000, 0.050, 0.100])

  for i in range(durations.shape[0]):
    print(durations[i])
    construct_all_knee_efforts_plot('%.3f' % durations[i], ps.cmap(i))
  ps.save_fig('knee_motor_bands.png')
  ps.add_legend(['%.3f ms' % duration for duration in durations])
  ps.plot(np.zeros(0), np.zeros(0), xlabel='time since impact (ms)', ylabel='motor torque (Nm)')
  ps.show_fig()


def construct_all_knee_efforts_plot(duration, color):
  t_matrix = np.load(data_directory + 't_x_' + duration + '.npy')
  x_matrix = np.load(data_directory + 'x_' + duration + '.npy')
  t_u_matrix = np.load(data_directory + 't_u_' + duration + '.npy')
  u_matrix = np.load(data_directory + 'u_' + duration + '.npy')

  for i in range(terrain_heights.shape[0]):
    for j in range(penetration_allowances.shape[0]):
      # t_idx = np.argwhere(t_matrix[i, j] == 3.0)[0, 0]
      t_u_start_idx = np.argwhere(np.abs(t_u_matrix[i, j] - (impact_time - 0.25)) < 2e-3)[0, 0]
      t_u_end_idx = np.argwhere(np.abs(t_u_matrix[i, j] - (impact_time + 0.5)) < 2e-3)[0, 0]
      t_u_slice = slice(t_u_start_idx, t_u_end_idx)
      # if i == 3 and j == 0:


  low_idx = 0
  high_idx = 5
  t_u_start_idx = np.argwhere(np.abs(t_u_matrix[low_idx, 0] - (impact_time - 0.25)) < 2e-3)[0, 0]
  t_u_end_idx = np.argwhere(np.abs(t_u_matrix[low_idx, 0] - (impact_time + 0.5)) < 2e-3)[0, 0]
  t_u_low_slice = slice(t_u_start_idx, t_u_end_idx)
  t_u_start_idx = np.argwhere(np.abs(t_u_matrix[high_idx, 0] - (impact_time - 0.25)) < 2e-3)[0, 0]
  t_u_end_idx = np.argwhere(np.abs(t_u_matrix[high_idx, 0] - (impact_time + 0.5)) < 2e-3)[0, 0]
  t_u_high_slice = slice(t_u_start_idx, t_u_end_idx)
  # mid = np.sum(u_matrix[4, 0, t_u_slice, 6:8], axis=1)
  ps.plot(t_u_matrix[i, j, t_u_low_slice], np.median(np.sum(u_matrix[:, :, t_u_low_slice, 6:8], axis=3), axis=(0, 1)), linestyle=color,
          grid=False)
  # lower_bound = np.sum(u_matrix[low_idx, 0, t_u_low_slice, 6:8], axis=1)
  # upper_bound = np.sum(u_matrix[high_idx, 0, t_u_high_slice, 6:8], axis=1)
  lower_bound = np.median(np.sum(u_matrix[:, :, t_u_low_slice, 6:8], axis=3), axis=(0, 1)) - np.std(np.sum(u_matrix[:, :, t_u_low_slice, 6:8], axis=3), axis=(0, 1))
  upper_bound = np.median(np.sum(u_matrix[:, :, t_u_low_slice, 6:8], axis=3), axis=(0, 1)) + np.std(np.sum(u_matrix[:, :, t_u_low_slice, 6:8], axis=3), axis=(0, 1))
  import pdb; pdb.set_trace()
  # ps.plot_bands(t_u_matrix[low_idx, 0, t_u_low_slice], t_u_matrix[high_idx, 0, t_u_high_slice], lower_bound,
  #               upper_bound, color=color)
  ps.plot_bands(t_u_matrix[0, 0, t_u_low_slice], t_u_matrix[0, 0, t_u_low_slice], lower_bound,
                upper_bound, color=color)

def construct_hardware_torque_plot():

  builder = DiagramBuilder()

  plant_w_spr, scene_graph_w_spr = AddMultibodyPlantSceneGraph(builder, 0.0)
  pydairlib.cassie.cassie_utils.addCassieMultibody(plant_w_spr, scene_graph_w_spr, True,
                                                   "examples/Cassie/urdf/cassie_v2.urdf", False, False)
  plant_w_spr.Finalize()
  controller_channel = 'OSC_JUMPING'

  pos_map = pydairlib.multibody.makeNameToPositionsMap(plant_w_spr)
  vel_map = pydairlib.multibody.makeNameToVelocitiesMap(plant_w_spr)
  act_map = pydairlib.multibody.makeNameToActuatorsMap(plant_w_spr)


  hardware_impact = 30.0 + nominal_impact_time + 0.09
  # Drake logs
  log_indices = ['12', '14', '15']
  hardware_log_path = '/home/yangwill/Documents/research/projects/cassie/hardware/logs/01_27_21/'
  for log_idx in log_indices:
    log_path = hardware_log_path + 'lcmlog-' + log_idx

    print(log_path)
    log = lcm.EventLog(log_path, "r")
    x, u_meas, t_x, u, t_u, contact_info, contact_info_locs, t_contact_info, \
    osc_debug, fsm, estop_signal, switch_signal, t_controller_switch, t_pd, kp, kd, cassie_out, u_pd, t_u_pd, \
    osc_output, full_log, t_lcmlog_u = process_log(log, pos_map, vel_map, act_map, controller_channel)
    t_u_start_idx = np.argwhere(np.abs(t_u - (hardware_impact - 0.25)) < 2e-3)[0, 0]
    t_u_end_idx = np.argwhere(np.abs(t_u - (hardware_impact + 0.5)) < 2e-3)[0, 0]
    t_u_slice = slice(t_u_start_idx, t_u_end_idx)
    u_indices = slice(6, 8)
    plt.figure("Combined knee motor efforts")
    ps.plot(t_u[t_u_slice] - hardware_impact, np.sum(u[t_u_slice, u_indices], axis=1), xlabel='Time Since Nominal Impact (ms)', ylabel='Combined Knee Motor Torque (Nm)')

  durations = np.arange(0.0, 0.150, 0.05)
  ps.add_legend(['%.0f (ms)' % (d*1e3) for d in durations])
  ps.save_fig('jan_27_hardware_knee_efforts.png')
  ps.show_fig()

if __name__ == '__main__':
  main()