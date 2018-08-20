#ifndef CLQR_CONTROLLER_H
#define CLQR_CONTROLLER_H

#include "drake/systems/framework/leaf_system.h"
#include "drake/systems/framework/basic_vector.h"
#include "drake/systems/framework/context.h"
#include "drake/systems/controllers/linear_quadratic_regulator.h"
#include "drake/systems/primitives/linear_system.h"
#include "drake/multibody/rigid_body_plant/rigid_body_plant.h"
#include "drake/multibody/rigid_body_tree.h"
#include "drake/math/autodiff.h"
#include "drake/math/autodiff_gradient.h"

#include "examples/Cassie/cassie_utils.h"
#include "systems/controllers/affine_controller.h"
#include "multibody/rbp_utils.h"

using std::vector;
using Eigen::VectorXd;
using Eigen::VectorXi;
using Eigen::Matrix;
using Eigen::MatrixXd;
using Eigen::Matrix3Xd;
using Eigen::Dynamic;
using Eigen::HouseholderQR;
using drake::VectorX;
using drake::AutoDiffXd;
using drake::AutoDiffVecXd;
using drake::math::initializeAutoDiff;
using drake::math::autoDiffToGradientMatrix;
using drake::math::autoDiffToValueMatrix;
using drake::systems::RigidBodyPlant;
using drake::systems::Context;
using drake::systems::controllers::LinearQuadraticRegulator;
using drake::systems::BasicVector;
using drake::systems::LeafSystem;
using drake::systems::InputPortDescriptor;
using drake::systems::OutputPort;
using drake::systems::ContinuousState;
using drake::systems::Context;
using drake::systems::kNoOutput;

using dairlib::multibody::utils::CalcTimeDerivativesUsingLambda;

namespace dairlib{
namespace systems{

class ClqrController : public AffineController {

  public: 
    ClqrController(const RigidBodyPlant<double>& plant,
                   const RigidBodyPlant<AutoDiffXd>& plant_autodiff,
                   VectorXd x0,
                   VectorXd u0,
                   VectorXd lambda0, 
                   MatrixXd Q,
                   MatrixXd R,
                   double fixed_point_tolerance = 1e-6);

    int GetNumPositions(){ return num_positions_;}
    int GetNumVelocities(){ return num_velocities_;}
    int GetNumStates(){ return num_states_;}
    int GetNumEfforts(){ return num_efforts_;}
    MatrixXd GetQ(){ return Q_;}
    MatrixXd GetR(){ return R_;}
    MatrixXd GetK(){ return K_;}
    VectorXd GetKVec(){ return MatToVec(K_);}
    void SetQ(MatrixXd Q){ Q_ = Q;}
    void SetR(MatrixXd R){ R_ = R;}
    void SetK(MatrixXd K){ K_ = K;}

  private:
    MatrixXd computeF();
    MatrixXd computeK();
    const RigidBodyTree<double>& tree_;
    const RigidBodyPlant<double>& plant_;
    const RigidBodyPlant<AutoDiffXd>& plant_autodiff_;
    VectorXd x0_;
    VectorXd u0_;
    VectorXd lambda0_;
    MatrixXd J_contact_;
    int num_positions_;
    int num_velocities_;
    int num_states_;
    int num_efforts_;
    MatrixXd Q_;
    MatrixXd R_;
    MatrixXd F_;
    MatrixXd K_;
    double fixed_point_tolerance_;
};

}// namespace systems
}//namespace dairlib




#endif
