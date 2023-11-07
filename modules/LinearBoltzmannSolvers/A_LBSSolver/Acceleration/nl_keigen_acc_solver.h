#pragma once

#include "framework/math/NonLinearSolver/NonLinearSolver.h"

#include "modules/LinearBoltzmannSolvers/A_LBSSolver/Acceleration/nl_keigen_acc_context.h"

#include <petscsnes.h>

namespace lbs::acceleration
{

class NLKEigenDiffSolver : public chi_math::NonLinearSolver<Mat, Vec, SNES>
{
public:
  typedef std::shared_ptr<NLKEigenDiffContext> NLKEigenDiffContextPtr;

  explicit NLKEigenDiffSolver(NLKEigenDiffContextPtr nlk_diff_context_ptr)
    : chi_math::NonLinearSolver<Mat, Vec, SNES>(nlk_diff_context_ptr)
  {
  }

  virtual ~NLKEigenDiffSolver() override = default;

protected:
  void SetMonitor() override;

  void SetSystemSize() override;
  void SetSystem() override;
  void SetFunction() override;
  void SetJacobian() override;

protected:
  void SetInitialGuess() override;
  void PostSolveCallback() override;
};

} // namespace lbs::acceleration
