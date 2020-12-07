//
// Created by brian on 11/7/20.
//
#include <drake/multibody/plant/multibody_plant.h>
#include <drake/math/autodiff_gradient.h>
#include <drake/common/autodiff.h>

#include "coriolis_matrix_calculator.h"


using drake::math::autoDiffToGradientMatrix;

using drake::AutoDiffXd;
using drake::AutoDiffVecXd;
using drake::AutoDiffd;

namespace dairlib::multibody {
     CoriolisMatrixCalculator::CoriolisMatrixCalculator(
            drake::multibody::MultibodyPlant<AutoDiffXd>& plant)
            : plant_(plant),
              n_q_(plant.num_positions()),
              n_v_(plant.num_velocities()){
    }

     void CoriolisMatrixCalculator::CalcCoriolisAutoDiff(std::unique_ptr<drake::systems::Context<AutoDiffXd>> &context,
                                                        MatrixX<AutoDiffXd> &C) const {
        DRAKE_ASSERT(C.rows() == n_v_);
        DRAKE_ASSERT(C.cols() == n_v_);
        AutoDiffVecXd Cv = AutoDiffVecXd::Zero(n_v_, 1);
        plant_.CalcBiasTerm(*context, &Cv);
        auto jac = autoDiffToGradientMatrix(Cv);
        C = 0.5*jac.rightCols(n_v_);
    }
}
