#include "dircon_util.h"

using Eigen::MatrixXd;
using Eigen::VectorXd;
using drake::solvers::Constraint;
using drake::solvers::Binding;

namespace drake{
namespace systems {
namespace trajectory_optimization{
namespace dircon {

void checkConstraints(const solvers::MathematicalProgram* prog) {
  for (auto const& binding : prog->generic_constraints()) {
    double tol = 1e-6;
    auto y = prog->EvalBindingAtSolution(binding);
    auto c = binding.evaluator();
    bool isSatisfied = (y.array() >= c->lower_bound().array() - tol).all() &&
                       (y.array() <= c->upper_bound().array() + tol).all();
    if (!isSatisfied) {
      std::cout << "Constraint violation: " << c->get_description() << std::endl;
      MatrixXd tmp(y.size(),3);
      tmp << c->lower_bound(), y, c->upper_bound();
      std::cout << tmp << std::endl;
    }
  }
}

//form a quadratic approximation of the cost
// cost \approx 1/2 z^T*Q*z + w^T*z + c
// return value is the constant term (c)
double secondOrderCost(const solvers::MathematicalProgram* prog, VectorXd& x,
  MatrixXd& Q, VectorXd& w) {

  int num_vars = prog->num_vars();
  Q = Eigen::MatrixXd::Zero(num_vars, num_vars);
  w = Eigen::MatrixXd::Zero(num_vars, 1);
  double c = 0;

  for (auto const& binding : prog->GetAllCosts()) {
    //evaluate cost
    auto variables = binding.variables();
    if (variables.size() == 0)
      continue;
    AutoDiffVecXd y_val = math::initializeAutoDiff(VectorXd::Zero(1), variables.size());
    VectorXd x_binding(variables.size());
    for (int i=0; i < variables.size(); i++) {
      x_binding(i) = x(prog->FindDecisionVariableIndex(variables(i)));
    }
    AutoDiffVecXd x_val = math::initializeAutoDiff(x_binding);
    binding.evaluator()->Eval(x_val, y_val);
    MatrixXd gradient_x = math::autoDiffToGradientMatrix(y_val);
    VectorXd y = math::autoDiffToValueMatrix(y_val);
    c += y(0); //costs are length 1
    for (int i = 0; i < variables.size(); i++) {
      w(prog->FindDecisionVariableIndex(variables(i))) = gradient_x(0,i);
    }


    // forward differencing for Hessian
    double dx = 1e-8;
    AutoDiffVecXd y_hessian = math::initializeAutoDiff(VectorXd::Zero(1), variables.size());
    for (int i = 0; i < variables.size(); i++) {
      x_val(i) += dx;
      binding.evaluator()->Eval(x_val, y_hessian);
      x_val(i) -= dx;
      MatrixXd gradient_hessian = math::autoDiffToGradientMatrix(y_hessian);
      for (int j=0; j <= i; j++) {
        int ind_i = prog->FindDecisionVariableIndex(variables(i));
        int ind_j = prog->FindDecisionVariableIndex(variables(j));
        Q(ind_i,ind_j) += (gradient_hessian(0,j)-gradient_x(0,j))/dx;
        Q(ind_j,ind_i) += (gradient_hessian(0,j)-gradient_x(0,j))/dx;
      }
    }

    // // Central differencing for Hessian
    // double dx = 1e-8;
    // AutoDiffVecXd y_hessian_p = math::initializeAutoDiff(VectorXd::Zero(1), variables.size());
    // AutoDiffVecXd y_hessian_m = math::initializeAutoDiff(VectorXd::Zero(1), variables.size());
    // for (int i = 0; i < variables.size(); i++) {
    //   x_val(i) -= dx/2;
    //   binding.evaluator()->Eval(x_val, y_hessian_m);
    //   x_val(i) += dx;
    //   binding.evaluator()->Eval(x_val, y_hessian_p);
    //   x_val(i) -= dx/2;

    //   MatrixXd gradient_hessian_p = math::autoDiffToGradientMatrix(y_hessian_p);
    //   MatrixXd gradient_hessian_m = math::autoDiffToGradientMatrix(y_hessian_m);

    //   for (int j=0; j <= i; j++) {
    //     Q(prog->FindDecisionVariableIndex(variables(i)),
    //       prog->FindDecisionVariableIndex(variables(j))) += 
    //       (gradient_hessian_p(j)-gradient_hessian_m(j))/dx;
    //   }
    // }

  }
  return c;
}


// Evaluate all constraints and construct a linearization of them
void linearizeConstraints(const solvers::MathematicalProgram* prog, VectorXd& x,
  VectorXd& y, MatrixXd& A, VectorXd& lb, VectorXd& ub) {


  int num_constraints = 0;
  int num_vars = prog->num_vars();

  // First, count constraints
  num_constraints += countConstraints(prog, prog->linear_constraints());
  num_constraints += countConstraints(prog, prog->linear_equality_constraints());
  num_constraints += countConstraints(prog, prog->lorentz_cone_constraints());
  num_constraints += countConstraints(prog, prog->generic_constraints());

  //Initialize data storage
  lb.resize(num_constraints);
  ub.resize(num_constraints);
  y.resize(num_constraints);
  A = Eigen::MatrixXd::Zero(num_constraints, num_vars);

  int constraint_index = 0;
  constraint_index = updateConstraints(prog, prog->linear_constraints(), x, y, A, lb, ub, constraint_index);
  constraint_index = updateConstraints(prog, prog->linear_equality_constraints(), x, y, A, lb, ub, constraint_index);
  constraint_index = updateConstraints(prog, prog->lorentz_cone_constraints(), x, y, A, lb, ub, constraint_index);
  constraint_index = updateConstraints(prog, prog->generic_constraints(), x, y, A, lb, ub, constraint_index);
}

template <typename Derived>
int countConstraints(const solvers::MathematicalProgram* prog, const std::vector<Binding<Derived>>& constraints) {
  int n = 0;
  for (auto const& binding : constraints) {
    n += binding.evaluator()->num_constraints();
  }
  return n;
}

template <typename Derived>
int updateConstraints(const solvers::MathematicalProgram* prog, const std::vector<Binding<Derived>>& constraints,
      VectorXd& x, VectorXd& y, MatrixXd& A, VectorXd& lb, VectorXd& ub, int constraint_index) {

  for (auto const& binding : constraints) {
    auto const& c = binding.evaluator();
    int n = c->num_constraints();
    lb.segment(constraint_index, n) = c->lower_bound();
    ub.segment(constraint_index, n) = c->upper_bound();

    //evaluate constraint
    auto variables = binding.variables();
    AutoDiffVecXd y_val = math::initializeAutoDiff(VectorXd::Zero(c->num_constraints()), variables.size());
    VectorXd x_binding(variables.size());
    for (int i=0; i < variables.size(); i++) {
      x_binding(i) = x(prog->FindDecisionVariableIndex(variables(i)));
    }
    AutoDiffVecXd x_val = math::initializeAutoDiff(x_binding);
    binding.evaluator()->Eval(x_val, y_val);
    MatrixXd dx = math::autoDiffToGradientMatrix(y_val);

    y.segment(constraint_index, n) = math::autoDiffToValueMatrix(y_val);
    for (int i = 0; i < variables.size(); i++) {
      A.block(constraint_index, prog->FindDecisionVariableIndex(variables(i)),n,1) = dx.col(i);
    }

    constraint_index += n;
  }

  return constraint_index;
}



}
}
}
}