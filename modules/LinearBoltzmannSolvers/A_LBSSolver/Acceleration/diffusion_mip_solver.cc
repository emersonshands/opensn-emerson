#include "diffusion_mip_solver.h"
#include "modules/LinearBoltzmannSolvers/A_LBSSolver/Acceleration/acceleration.h"
#include "framework/mesh/MeshContinuum/chi_meshcontinuum.h"
#include "framework/math/SpatialDiscretization/FiniteElement/QuadraturePointData.h"
#include "framework/math/SpatialDiscretization/SpatialDiscretization.h"
#include "modules/LinearBoltzmannSolvers/A_LBSSolver/lbs_structs.h"
#include "framework/chi_runtime.h"
#include "framework/logging/chi_log.h"
#include "framework/utils/chi_timer.h"
#include "framework/console/chi_console.h"
#include <utility>

#define DefaultBCDirichlet                                                                         \
  BoundaryCondition                                                                                \
  {                                                                                                \
    BCType::DIRICHLET,                                                                             \
    {                                                                                              \
      0, 0, 0                                                                                      \
    }                                                                                              \
  }

#define scdouble static_cast<double>

namespace lbs::acceleration
{

DiffusionMIPSolver::DiffusionMIPSolver(std::string text_name,
                                       const chi_math::SpatialDiscretization& sdm,
                                       const chi_math::UnknownManager& uk_man,
                                       std::map<uint64_t, BoundaryCondition> bcs,
                                       MatID2XSMap map_mat_id_2_xs,
                                       const std::vector<UnitCellMatrices>& unit_cell_matrices,
                                       const bool verbose /*=false*/)
  : DiffusionSolver(std::move(text_name),
                    sdm,
                    uk_man,
                    std::move(bcs),
                    std::move(map_mat_id_2_xs),
                    unit_cell_matrices,
                    verbose,
                    /*requires_ghosts=*/false)
{
  using SDM_TYPE = chi_math::SpatialDiscretizationType;
  const auto& PWLD = SDM_TYPE ::PIECEWISE_LINEAR_DISCONTINUOUS;

  if (sdm_.Type() != PWLD)
    throw std::logic_error("lbs::acceleration::DiffusionMIPSolver: can only be"
                           " used with PWLD.");
}

void
DiffusionMIPSolver::AssembleAand_b_wQpoints(const std::vector<double>& q_vector)
{
  const std::string fname = "lbs::acceleration::DiffusionMIPSolver::"
                            "AssembleAand_b_wQpoints";
  if (A_ == nullptr or rhs_ == nullptr or ksp_ == nullptr)
    throw std::logic_error(fname + ": Some or all PETSc elements are null. "
                                   "Check that Initialize has been called.");
  if (options.verbose) Chi::log.Log() << Chi::program_timer.GetTimeString() << " Starting assembly";

#ifdef OPENSN_WITH_LUA
  lua_State* L = Chi::console.GetConsoleState();
#endif
  const auto& source_function = options.source_lua_function;
  const auto& solution_function = options.ref_solution_lua_function;

  const size_t num_groups = uk_man_.unknowns_.front().num_components_;

  VecSet(rhs_, 0.0);

  for (const auto& cell : grid_.local_cells)
  {
    const size_t num_faces = cell.faces_.size();
    const auto& cell_mapping = sdm_.GetCellMapping(cell);
    const size_t num_nodes = cell_mapping.NumNodes();
    const auto cc_nodes = cell_mapping.GetNodeLocations();
    const auto qp_data = cell_mapping.MakeVolumetricQuadraturePointData();

    const auto& xs = mat_id_2_xs_map_.at(cell.material_id_);

    // For component/group
    for (size_t g = 0; g < num_groups; ++g)
    {
      // Get coefficient and nodal src
      const double Dg = xs.Dg[g];
      const double sigr_g = xs.sigR[g];

      std::vector<double> qg(num_nodes, 0.0);
      for (size_t j = 0; j < num_nodes; j++)
        qg[j] = q_vector[sdm_.MapDOFLocal(cell, j, uk_man_, 0, g)];

      // Assemble continuous terms
      for (size_t i = 0; i < num_nodes; i++)
      {
        const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);
        double entry_rhs_i = 0.0; // entry may accumulate over j
        for (size_t j = 0; j < num_nodes; j++)
        {
          const int64_t jmap = sdm_.MapDOF(cell, j, uk_man_, 0, g);
          double entry_aij = 0.0;
          for (size_t qp : qp_data.QuadraturePointIndices())
          {
            entry_aij +=
              Dg * qp_data.ShapeGrad(i, qp).Dot(qp_data.ShapeGrad(j, qp)) * qp_data.JxW(qp);

            entry_aij +=
              sigr_g * qp_data.ShapeValue(i, qp) * qp_data.ShapeValue(j, qp) * qp_data.JxW(qp);

            if (source_function.empty())
              entry_rhs_i +=
                qg[j] * qp_data.ShapeValue(i, qp) * qp_data.ShapeValue(j, qp) * qp_data.JxW(qp);
          } // for qp
          MatSetValue(A_, imap, jmap, entry_aij, ADD_VALUES);
        } // for j

        if (not source_function.empty())
        {
#ifdef OPENSN_WITH_LUA
          for (size_t qp : qp_data.QuadraturePointIndices())
            entry_rhs_i += CallLuaXYZFunction(L, source_function, qp_data.QPointXYZ(qp)) *
                           qp_data.ShapeValue(i, qp) * qp_data.JxW(qp);
#endif
        }

        VecSetValue(rhs_, imap, entry_rhs_i, ADD_VALUES);
      } // for i

      // Assemble face terms
      for (size_t f = 0; f < num_faces; ++f)
      {
        const auto& face = cell.faces_[f];
        const auto& n_f = face.normal_;
        const size_t num_face_nodes = cell_mapping.NumFaceNodes(f);
        const auto fqp_data = cell_mapping.MakeSurfaceQuadraturePointData(f);

        const double hm = HPerpendicular(cell, f);

        typedef chi_mesh::MeshContinuum Grid;

        if (face.has_neighbor_)
        {
          const auto& adj_cell = grid_.cells[face.neighbor_id_];
          const auto& adj_cell_mapping = sdm_.GetCellMapping(adj_cell);
          const auto ac_nodes = adj_cell_mapping.GetNodeLocations();
          const size_t acf = Grid::MapCellFace(cell, adj_cell, f);
          const double hp = HPerpendicular(adj_cell, acf);

          const auto& adj_xs = mat_id_2_xs_map_.at(adj_cell.material_id_);
          const double adj_Dg = adj_xs.Dg[g];

          // Compute kappa
          double kappa = 1.0;
          if (cell.Type() == chi_mesh::CellType::SLAB)
            kappa = fmax(options.penalty_factor * (adj_Dg / hp + Dg / hm) * 0.5, 0.25);
          if (cell.Type() == chi_mesh::CellType::POLYGON)
            kappa = fmax(options.penalty_factor * (adj_Dg / hp + Dg / hm) * 0.5, 0.25);
          if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
            kappa = fmax(options.penalty_factor * 2.0 * (adj_Dg / hp + Dg / hm) * 0.5, 0.25);

          // Assembly penalty terms
          for (size_t fi = 0; fi < num_face_nodes; ++fi)
          {
            const int i = cell_mapping.MapFaceNode(f, fi);
            const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

            for (size_t fj = 0; fj < num_face_nodes; ++fj)
            {
              const int jm = cell_mapping.MapFaceNode(f, fj); // j-minus
              const int jp =
                MapFaceNodeDisc(cell, adj_cell, cc_nodes, ac_nodes, f, acf, fj); // j-plus
              const int64_t jmmap = sdm_.MapDOF(cell, jm, uk_man_, 0, g);
              const int64_t jpmap = sdm_.MapDOF(adj_cell, jp, uk_man_, 0, g);

              double aij = 0.0;
              for (size_t qp : fqp_data.QuadraturePointIndices())
                aij += kappa * fqp_data.ShapeValue(i, qp) * fqp_data.ShapeValue(jm, qp) *
                       fqp_data.JxW(qp);

              MatSetValue(A_, imap, jmmap, aij, ADD_VALUES);
              MatSetValue(A_, imap, jpmap, -aij, ADD_VALUES);
            } // for fj
          }   // for fi

          // Assemble gradient terms
          // For the following comments we use the notation:
          // Dk = 0.5* n dot nabla bk

          // 0.5*D* n dot (b_j^+ - b_j^-)*nabla b_i^-
          for (int i = 0; i < num_nodes; i++)
          {
            const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

            for (int fj = 0; fj < num_face_nodes; fj++)
            {
              const int jm = cell_mapping.MapFaceNode(f, fj); // j-minus
              const int jp =
                MapFaceNodeDisc(cell, adj_cell, cc_nodes, ac_nodes, f, acf, fj); // j-plus
              const int64_t jmmap = sdm_.MapDOF(cell, jm, uk_man_, 0, g);
              const int64_t jpmap = sdm_.MapDOF(adj_cell, jp, uk_man_, 0, g);

              chi_mesh::Vector3 vec_aij;
              for (size_t qp : fqp_data.QuadraturePointIndices())
                vec_aij +=
                  fqp_data.ShapeValue(jm, qp) * fqp_data.ShapeGrad(i, qp) * fqp_data.JxW(qp);
              const double aij = -0.5 * Dg * n_f.Dot(vec_aij);

              MatSetValue(A_, imap, jmmap, aij, ADD_VALUES);
              MatSetValue(A_, imap, jpmap, -aij, ADD_VALUES);
            } // for fj
          }   // for i

          // 0.5*D* n dot (b_i^+ - b_i^-)*nabla b_j^-
          for (int fi = 0; fi < num_face_nodes; fi++)
          {
            const int im = cell_mapping.MapFaceNode(f, fi); // i-minus
            const int ip =
              MapFaceNodeDisc(cell, adj_cell, cc_nodes, ac_nodes, f, acf, fi); // i-plus
            const int64_t immap = sdm_.MapDOF(cell, im, uk_man_, 0, g);
            const int64_t ipmap = sdm_.MapDOF(adj_cell, ip, uk_man_, 0, g);

            for (int j = 0; j < num_nodes; j++)
            {
              const int64_t jmap = sdm_.MapDOF(cell, j, uk_man_, 0, g);

              chi_mesh::Vector3 vec_aij;
              for (size_t qp : fqp_data.QuadraturePointIndices())
                vec_aij +=
                  fqp_data.ShapeValue(im, qp) * fqp_data.ShapeGrad(j, qp) * fqp_data.JxW(qp);
              const double aij = -0.5 * Dg * n_f.Dot(vec_aij);

              MatSetValue(A_, immap, jmap, aij, ADD_VALUES);
              MatSetValue(A_, ipmap, jmap, -aij, ADD_VALUES);
            } // for j
          }   // for fi

        } // internal face
        else
        {
          auto bc = DefaultBCDirichlet;
          if (bcs_.count(face.neighbor_id_) > 0) bc = bcs_.at(face.neighbor_id_);

          if (bc.type == BCType::DIRICHLET)
          {
            const double bc_value = bc.values[0];

            // Compute kappa
            double kappa = 1.0;
            if (cell.Type() == chi_mesh::CellType::SLAB)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYGON)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
              kappa = fmax(options.penalty_factor * 2.0 * Dg / hm, 0.25);

            // Assembly penalty terms
            for (size_t fi = 0; fi < num_face_nodes; ++fi)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t fj = 0; fj < num_face_nodes; ++fj)
              {
                const int jm = cell_mapping.MapFaceNode(f, fj);
                const int64_t jmmap = sdm_.MapDOF(cell, jm, uk_man_, 0, g);

                double aij = 0.0;
                for (size_t qp : fqp_data.QuadraturePointIndices())
                  aij += kappa * fqp_data.ShapeValue(i, qp) * fqp_data.ShapeValue(jm, qp) *
                         fqp_data.JxW(qp);
                double aij_bc_value = aij * bc_value;

                if (not solution_function.empty())
                {
#ifdef OPENSN_WITH_LUA
                  aij_bc_value = 0.0;
                  for (size_t qp : fqp_data.QuadraturePointIndices())
                    aij_bc_value +=
                      kappa * CallLuaXYZFunction(L, solution_function, fqp_data.QPointXYZ(qp)) *
                      fqp_data.ShapeValue(i, qp) * fqp_data.ShapeValue(jm, qp) * fqp_data.JxW(qp);
#endif
                }

                MatSetValue(A_, imap, jmmap, aij, ADD_VALUES);
                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for fi

            // Assemble gradient terms
            // For the following comments we use the notation:
            // Dk = n dot nabla bk

            // D* n dot (b_j^+ - b_j^-)*nabla b_i^-
            for (size_t i = 0; i < num_nodes; i++)
            {
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t j = 0; j < num_nodes; j++)
              {
                const int64_t jmap = sdm_.MapDOF(cell, j, uk_man_, 0, g);

                chi_mesh::Vector3 vec_aij;
                for (size_t qp : fqp_data.QuadraturePointIndices())
                  vec_aij +=
                    fqp_data.ShapeValue(j, qp) * fqp_data.ShapeGrad(i, qp) * fqp_data.JxW(qp) +
                    fqp_data.ShapeValue(i, qp) * fqp_data.ShapeGrad(j, qp) * fqp_data.JxW(qp);
                const double aij = -Dg * n_f.Dot(vec_aij);

                double aij_bc_value = aij * bc_value;

                if (not solution_function.empty())
                {
#ifdef OPENSN_WITH_LUA
                  chi_mesh::Vector3 vec_aij_mms;
                  for (size_t qp : fqp_data.QuadraturePointIndices())
                    vec_aij_mms +=
                      CallLuaXYZFunction(L, solution_function, fqp_data.QPointXYZ(qp)) *
                      (fqp_data.ShapeValue(j, qp) * fqp_data.ShapeGrad(i, qp) * fqp_data.JxW(qp) +
                       fqp_data.ShapeValue(i, qp) * fqp_data.ShapeGrad(j, qp) * fqp_data.JxW(qp));
                  aij_bc_value = -Dg * n_f.Dot(vec_aij_mms);
#endif
                }

                MatSetValue(A_, imap, jmap, aij, ADD_VALUES);
                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for i
          }     // Dirichlet BC
          else if (bc.type == BCType::ROBIN)
          {
            const double aval = bc.values[0];
            const double bval = bc.values[1];
            const double fval = bc.values[2];

            if (std::fabs(bval) < 1.0e-12) continue; // a and f assumed zero

            for (size_t fi = 0; fi < num_face_nodes; fi++)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t ir = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              if (std::fabs(aval) >= 1.0e-12)
              {
                for (size_t fj = 0; fj < num_face_nodes; fj++)
                {
                  const int j = cell_mapping.MapFaceNode(f, fj);
                  const int64_t jr = sdm_.MapDOF(cell, j, uk_man_, 0, g);

                  double aij = 0.0;
                  for (size_t qp : fqp_data.QuadraturePointIndices())
                    aij +=
                      fqp_data.ShapeValue(i, qp) * fqp_data.ShapeValue(j, qp) * fqp_data.JxW(qp);
                  aij *= (aval / bval);

                  MatSetValue(A_, ir, jr, aij, ADD_VALUES);
                } // for fj
              }   // if a nonzero

              if (std::fabs(fval) >= 1.0e-12)
              {
                double rhs_val = 0.0;
                for (size_t qp : fqp_data.QuadraturePointIndices())
                  rhs_val += fqp_data.ShapeValue(i, qp) * fqp_data.JxW(qp);
                rhs_val *= (fval / bval);

                VecSetValue(rhs_, ir, rhs_val, ADD_VALUES);
              } // if f nonzero
            }   // for fi
          }     // Robin BC
        }       // boundary face
      }         // for face
    }           // for g
  }             // for cell

  MatAssemblyBegin(A_, MAT_FINAL_ASSEMBLY);
  MatAssemblyEnd(A_, MAT_FINAL_ASSEMBLY);
  VecAssemblyBegin(rhs_);
  VecAssemblyEnd(rhs_);

  if (options.perform_symmetry_check)
  {
    PetscBool symmetry = PETSC_FALSE;
    MatIsSymmetric(A_, 1.0e-6, &symmetry);
    if (symmetry == PETSC_FALSE) throw std::logic_error(fname + ":Symmetry check failed");
  }

  KSPSetOperators(ksp_, A_, A_);

  if (options.verbose)
    Chi::log.Log() << Chi::program_timer.GetTimeString() << " Assembly completed";

  PC pc;
  KSPGetPC(ksp_, &pc);
  PCSetUp(pc);

  KSPSetUp(ksp_);
}

void
DiffusionMIPSolver::Assemble_b_wQpoints(const std::vector<double>& q_vector)
{
  const std::string fname = "lbs::acceleration::DiffusionMIPSolver::"
                            "AssembleAand_b_wQpoints";
  if (A_ == nullptr or rhs_ == nullptr or ksp_ == nullptr)
    throw std::logic_error(fname + ": Some or all PETSc elements are null. "
                                   "Check that Initialize has been called.");
  if (options.verbose) Chi::log.Log() << Chi::program_timer.GetTimeString() << " Starting assembly";

#ifdef OPENSN_WITH_LUA
  lua_State* L = Chi::console.GetConsoleState();
#endif
  const auto& source_function = options.source_lua_function;
  const auto& solution_function = options.ref_solution_lua_function;

  VecSet(rhs_, 0.0);

  for (const auto& cell : grid_.local_cells)
  {
    const size_t num_faces = cell.faces_.size();
    const auto& cell_mapping = sdm_.GetCellMapping(cell);
    const size_t num_nodes = cell_mapping.NumNodes();
    const auto qp_data = cell_mapping.MakeVolumetricQuadraturePointData();
    const size_t num_groups = uk_man_.unknowns_.front().num_components_;

    const auto& xs = mat_id_2_xs_map_.at(cell.material_id_);

    // For component/group
    for (size_t g = 0; g < num_groups; ++g)
    {
      // Get coefficient and nodal src
      const double Dg = xs.Dg[g];

      std::vector<double> qg(num_nodes, 0.0);
      for (size_t j = 0; j < num_nodes; j++)
        qg[j] = q_vector[sdm_.MapDOFLocal(cell, j, uk_man_, 0, g)];

      // Assemble continuous terms
      for (size_t i = 0; i < num_nodes; i++)
      {
        const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);
        double entry_rhs_i = 0.0; // entry may accumulate over j
        if (source_function.empty())
          for (size_t j = 0; j < num_nodes; j++)
          {
            for (size_t qp : qp_data.QuadraturePointIndices())
            {
              entry_rhs_i +=
                qg[j] * qp_data.ShapeValue(i, qp) * qp_data.ShapeValue(j, qp) * qp_data.JxW(qp);
            } // for qp
          }   // for j
        else
        {
#ifdef OPENSN_WITH_LUA
          for (size_t qp : qp_data.QuadraturePointIndices())
            entry_rhs_i += CallLuaXYZFunction(L, source_function, qp_data.QPointXYZ(qp)) *
                           qp_data.ShapeValue(i, qp) * qp_data.JxW(qp);
#endif
        }

        VecSetValue(rhs_, imap, entry_rhs_i, ADD_VALUES);
      } // for i

      // Assemble face terms
      for (size_t f = 0; f < num_faces; ++f)
      {
        const auto& face = cell.faces_[f];
        const auto& n_f = face.normal_;
        const size_t num_face_nodes = cell_mapping.NumFaceNodes(f);
        const auto fqp_data = cell_mapping.MakeSurfaceQuadraturePointData(f);

        const double hm = HPerpendicular(cell, f);

        if (not face.has_neighbor_)
        {
          auto bc = DefaultBCDirichlet;
          if (bcs_.count(face.neighbor_id_) > 0) bc = bcs_.at(face.neighbor_id_);

          if (bc.type == BCType::DIRICHLET)
          {
            const double bc_value = bc.values[0];

            // Compute kappa
            double kappa = 1.0;
            if (cell.Type() == chi_mesh::CellType::SLAB)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYGON)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
              kappa = fmax(options.penalty_factor * 2.0 * Dg / hm, 0.25);

            // Assembly penalty terms
            for (size_t fi = 0; fi < num_face_nodes; ++fi)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t fj = 0; fj < num_face_nodes; ++fj)
              {
                const int jm = cell_mapping.MapFaceNode(f, fj);

                double aij = 0.0;
                for (size_t qp : fqp_data.QuadraturePointIndices())
                  aij += kappa * fqp_data.ShapeValue(i, qp) * fqp_data.ShapeValue(jm, qp) *
                         fqp_data.JxW(qp);
                double aij_bc_value = aij * bc_value;

                if (not solution_function.empty())
                {
                  aij_bc_value = 0.0;
#ifdef OPENSN_WITH_LUA
                  for (size_t qp : fqp_data.QuadraturePointIndices())
                    aij_bc_value +=
                      kappa * CallLuaXYZFunction(L, solution_function, fqp_data.QPointXYZ(qp)) *
                      fqp_data.ShapeValue(i, qp) * fqp_data.ShapeValue(jm, qp) * fqp_data.JxW(qp);
#endif
                }

                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for fi

            // Assemble gradient terms
            // For the following comments we use the notation:
            // Dk = 0.5* n dot nabla bk

            // 0.5*D* n dot (b_j^+ - b_j^-)*nabla b_i^-
            for (size_t i = 0; i < num_nodes; i++)
            {
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t j = 0; j < num_nodes; j++)
              {
                chi_mesh::Vector3 vec_aij;
                for (size_t qp : fqp_data.QuadraturePointIndices())
                  vec_aij +=
                    fqp_data.ShapeValue(j, qp) * fqp_data.ShapeGrad(i, qp) * fqp_data.JxW(qp) +
                    fqp_data.ShapeValue(i, qp) * fqp_data.ShapeGrad(j, qp) * fqp_data.JxW(qp);
                const double aij = -Dg * n_f.Dot(vec_aij);

                double aij_bc_value = aij * bc_value;

                if (not solution_function.empty())
                {
#ifdef OPENSN_WITH_LUA
                  chi_mesh::Vector3 vec_aij_mms;
                  for (size_t qp : fqp_data.QuadraturePointIndices())
                    vec_aij_mms +=
                      CallLuaXYZFunction(L, solution_function, fqp_data.QPointXYZ(qp)) *
                      (fqp_data.ShapeValue(j, qp) * fqp_data.ShapeGrad(i, qp) * fqp_data.JxW(qp) +
                       fqp_data.ShapeValue(i, qp) * fqp_data.ShapeGrad(j, qp) * fqp_data.JxW(qp));
                  aij_bc_value = -Dg * n_f.Dot(vec_aij_mms);
#endif
                }

                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for i
          }     // Dirichlet BC
          else if (bc.type == BCType::ROBIN)
          {
            const double bval = bc.values[1];
            const double fval = bc.values[2];

            if (std::fabs(bval) < 1.0e-12) continue; // a and f assumed zero

            for (size_t fi = 0; fi < num_face_nodes; fi++)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t ir = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              if (std::fabs(fval) >= 1.0e-12)
              {
                double rhs_val = 0.0;
                for (size_t qp : fqp_data.QuadraturePointIndices())
                  rhs_val += fqp_data.ShapeValue(i, qp) * fqp_data.JxW(qp);
                rhs_val *= (fval / bval);

                VecSetValue(rhs_, ir, rhs_val, ADD_VALUES);
              } // if f nonzero
            }   // for fi
          }     // Robin BC
        }       // boundary face
      }         // for face
    }           // for g
  }             // for cell

  VecAssemblyBegin(rhs_);
  VecAssemblyEnd(rhs_);

  KSPSetOperators(ksp_, A_, A_);

  if (options.verbose)
    Chi::log.Log() << Chi::program_timer.GetTimeString() << " Assembly completed";

  PC pc;
  KSPGetPC(ksp_, &pc);
  PCSetUp(pc);

  KSPSetUp(ksp_);
}

void
DiffusionMIPSolver::AssembleAand_b(const std::vector<double>& q_vector)
{
  const std::string fname = "lbs::acceleration::DiffusionMIPSolver::"
                            "AssembleAand_b";
  if (A_ == nullptr or rhs_ == nullptr or ksp_ == nullptr)
    throw std::logic_error(fname + ": Some or all PETSc elements are null. "
                                   "Check that Initialize has been called.");
  if (options.verbose) Chi::log.Log() << Chi::program_timer.GetTimeString() << " Starting assembly";

  const size_t num_groups = uk_man_.unknowns_.front().num_components_;

  VecSet(rhs_, 0.0);
  for (const auto& cell : grid_.local_cells)
  {
    const size_t num_faces = cell.faces_.size();
    const auto& cell_mapping = sdm_.GetCellMapping(cell);
    const size_t num_nodes = cell_mapping.NumNodes();
    const auto cc_nodes = cell_mapping.GetNodeLocations();
    const auto& unit_cell_matrices = unit_cell_matrices_[cell.local_id_];

    const auto& cell_K_matrix = unit_cell_matrices.K_matrix;
    const auto& cell_M_matrix = unit_cell_matrices.M_matrix;

    const auto& xs = mat_id_2_xs_map_.at(cell.material_id_);

    for (size_t g = 0; g < num_groups; ++g)
    {
      // Get coefficient and nodal src
      const double Dg = xs.Dg[g];
      const double sigr_g = xs.sigR[g];

      std::vector<double> qg(num_nodes, 0.0);
      for (size_t j = 0; j < num_nodes; j++)
        qg[j] = q_vector[sdm_.MapDOFLocal(cell, j, uk_man_, 0, g)];

      // Assemble continuous terms
      for (size_t i = 0; i < num_nodes; i++)
      {
        const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);
        double entry_rhs_i = 0.0;
        for (size_t j = 0; j < num_nodes; j++)
        {
          const int64_t jmap = sdm_.MapDOF(cell, j, uk_man_, 0, g);

          const double entry_aij = Dg * cell_K_matrix[i][j] + sigr_g * cell_M_matrix[i][j];

          entry_rhs_i += qg[j] * cell_M_matrix[i][j];

          MatSetValue(A_, imap, jmap, entry_aij, ADD_VALUES);
        } // for j

        VecSetValue(rhs_, imap, entry_rhs_i, ADD_VALUES);
      } // for i

      // Assemble face terms
      for (size_t f = 0; f < num_faces; ++f)
      {
        const auto& face = cell.faces_[f];
        const auto& n_f = face.normal_;
        const size_t num_face_nodes = cell_mapping.NumFaceNodes(f);

        const auto& face_M = unit_cell_matrices.face_M_matrices[f];
        const auto& face_G = unit_cell_matrices.face_G_matrices[f];
        const auto& face_Si = unit_cell_matrices.face_Si_vectors[f];

        const double hm = HPerpendicular(cell, f);

        typedef chi_mesh::MeshContinuum Grid;

        if (face.has_neighbor_)
        {
          const auto& adj_cell = grid_.cells[face.neighbor_id_];
          const auto& adj_cell_mapping = sdm_.GetCellMapping(adj_cell);
          const auto ac_nodes = adj_cell_mapping.GetNodeLocations();
          const size_t acf = Grid::MapCellFace(cell, adj_cell, f);
          const double hp = HPerpendicular(adj_cell, acf);

          const auto& adj_xs = mat_id_2_xs_map_.at(adj_cell.material_id_);
          const double adj_Dg = adj_xs.Dg[g];

          // Compute kappa
          double kappa = 1.0;
          if (cell.Type() == chi_mesh::CellType::SLAB)
            kappa = fmax(options.penalty_factor * (adj_Dg / hp + Dg / hm) * 0.5, 0.25);
          if (cell.Type() == chi_mesh::CellType::POLYGON)
            kappa = fmax(options.penalty_factor * (adj_Dg / hp + Dg / hm) * 0.5, 0.25);
          if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
            kappa = fmax(options.penalty_factor * 2.0 * (adj_Dg / hp + Dg / hm) * 0.5, 0.25);

          // Assembly penalty terms
          for (size_t fi = 0; fi < num_face_nodes; ++fi)
          {
            const int i = cell_mapping.MapFaceNode(f, fi);
            const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

            for (size_t fj = 0; fj < num_face_nodes; ++fj)
            {
              const int jm = cell_mapping.MapFaceNode(f, fj); // j-minus
              const int jp =
                MapFaceNodeDisc(cell, adj_cell, cc_nodes, ac_nodes, f, acf, fj); // j-plus
              const int64_t jmmap = sdm_.MapDOF(cell, jm, uk_man_, 0, g);
              const int64_t jpmap = sdm_.MapDOF(adj_cell, jp, uk_man_, 0, g);

              const double aij = kappa * face_M[i][jm];

              MatSetValue(A_, imap, jmmap, aij, ADD_VALUES);
              MatSetValue(A_, imap, jpmap, -aij, ADD_VALUES);
            } // for fj
          }   // for fi

          // Assemble gradient terms
          // For the following comments we use the notation:
          // Dk = 0.5* n dot nabla bk

          // 0.5*D* n dot (b_j^+ - b_j^-)*nabla b_i^-
          for (int i = 0; i < num_nodes; i++)
          {
            const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

            for (int fj = 0; fj < num_face_nodes; fj++)
            {
              const int jm = cell_mapping.MapFaceNode(f, fj); // j-minus
              const int jp =
                MapFaceNodeDisc(cell, adj_cell, cc_nodes, ac_nodes, f, acf, fj); // j-plus
              const int64_t jmmap = sdm_.MapDOF(cell, jm, uk_man_, 0, g);
              const int64_t jpmap = sdm_.MapDOF(adj_cell, jp, uk_man_, 0, g);

              const double aij = -0.5 * Dg * n_f.Dot(face_G[jm][i]);

              MatSetValue(A_, imap, jmmap, aij, ADD_VALUES);
              MatSetValue(A_, imap, jpmap, -aij, ADD_VALUES);
            } // for fj
          }   // for i

          // 0.5*D* n dot (b_i^+ - b_i^-)*nabla b_j^-
          for (int fi = 0; fi < num_face_nodes; fi++)
          {
            const int im = cell_mapping.MapFaceNode(f, fi); // i-minus
            const int ip =
              MapFaceNodeDisc(cell, adj_cell, cc_nodes, ac_nodes, f, acf, fi); // i-plus
            const int64_t immap = sdm_.MapDOF(cell, im, uk_man_, 0, g);
            const int64_t ipmap = sdm_.MapDOF(adj_cell, ip, uk_man_, 0, g);

            for (int j = 0; j < num_nodes; j++)
            {
              const int64_t jmap = sdm_.MapDOF(cell, j, uk_man_, 0, g);

              const double aij = -0.5 * Dg * n_f.Dot(face_G[im][j]);

              MatSetValue(A_, immap, jmap, aij, ADD_VALUES);
              MatSetValue(A_, ipmap, jmap, -aij, ADD_VALUES);
            } // for j
          }   // for fi

        } // internal face
        else
        {
          auto bc = DefaultBCDirichlet;
          if (bcs_.count(face.neighbor_id_) > 0) bc = bcs_.at(face.neighbor_id_);

          if (bc.type == BCType::DIRICHLET)
          {
            const double bc_value = bc.values[0];

            // Compute kappa
            double kappa = 1.0;
            if (cell.Type() == chi_mesh::CellType::SLAB)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYGON)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
              kappa = fmax(options.penalty_factor * 2.0 * Dg / hm, 0.25);

            // Assembly penalty terms
            for (size_t fi = 0; fi < num_face_nodes; ++fi)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t fj = 0; fj < num_face_nodes; ++fj)
              {
                const int jm = cell_mapping.MapFaceNode(f, fj);
                const int64_t jmmap = sdm_.MapDOF(cell, jm, uk_man_, 0, g);

                const double aij = kappa * face_M[i][jm];
                const double aij_bc_value = aij * bc_value;

                MatSetValue(A_, imap, jmmap, aij, ADD_VALUES);
                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for fi

            // Assemble gradient terms
            // For the following comments we use the notation:
            // Dk = n dot nabla bk

            // D* n dot (b_j^+ - b_j^-)*nabla b_i^-
            for (size_t i = 0; i < num_nodes; i++)
            {
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t j = 0; j < num_nodes; j++)
              {
                const int64_t jmap = sdm_.MapDOF(cell, j, uk_man_, 0, g);

                const double aij = -Dg * n_f.Dot(face_G[j][i] + face_G[i][j]);
                const double aij_bc_value = aij * bc_value;

                MatSetValue(A_, imap, jmap, aij, ADD_VALUES);
                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for i
          }     // Dirichlet BC
          else if (bc.type == BCType::ROBIN)
          {
            const double aval = bc.values[0];
            const double bval = bc.values[1];
            const double fval = bc.values[2];

            if (std::fabs(bval) < 1.0e-12) continue; // a and f assumed zero

            for (size_t fi = 0; fi < num_face_nodes; fi++)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t ir = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              if (std::fabs(aval) >= 1.0e-12)
              {
                for (size_t fj = 0; fj < num_face_nodes; fj++)
                {
                  const int j = cell_mapping.MapFaceNode(f, fj);
                  const int64_t jr = sdm_.MapDOF(cell, j, uk_man_, 0, g);

                  const double aij = (aval / bval) * face_M[i][j];

                  MatSetValue(A_, ir, jr, aij, ADD_VALUES);
                } // for fj
              }   // if a nonzero

              if (std::fabs(fval) >= 1.0e-12)
              {
                const double rhs_val = (fval / bval) * face_Si[i];

                VecSetValue(rhs_, ir, rhs_val, ADD_VALUES);
              } // if f nonzero
            }   // for fi
          }     // Robin BC
        }       // boundary face
      }         // for face
    }           // for g
  }             // for cell

  MatAssemblyBegin(A_, MAT_FINAL_ASSEMBLY);
  MatAssemblyEnd(A_, MAT_FINAL_ASSEMBLY);
  VecAssemblyBegin(rhs_);
  VecAssemblyEnd(rhs_);

  if (options.verbose)
  {
    MatInfo info;
    MatGetInfo(A_, MAT_GLOBAL_SUM, &info);

    Chi::log.Log() << "Number of mallocs used = " << info.mallocs
                   << "\nNumber of non-zeros allocated = " << info.nz_allocated
                   << "\nNumber of non-zeros used = " << info.nz_used
                   << "\nNumber of unneeded non-zeros = " << info.nz_unneeded;
  }

  if (options.perform_symmetry_check)
  {
    PetscBool symmetry = PETSC_FALSE;
    MatIsSymmetric(A_, 1.0e-6, &symmetry);
    if (symmetry == PETSC_FALSE) throw std::logic_error(fname + ":Symmetry check failed");
  }

  KSPSetOperators(ksp_, A_, A_);

  if (options.verbose)
    Chi::log.Log() << Chi::program_timer.GetTimeString() << " Assembly completed";

  PC pc;
  KSPGetPC(ksp_, &pc);
  PCSetUp(pc);

  KSPSetUp(ksp_);
}

void
DiffusionMIPSolver::Assemble_b(const std::vector<double>& q_vector)
{
  const std::string fname = "lbs::acceleration::DiffusionMIPSolver::"
                            "Assemble_b";
  if (A_ == nullptr or rhs_ == nullptr or ksp_ == nullptr)
    throw std::logic_error(fname + ": Some or all PETSc elements are null. "
                                   "Check that Initialize has been called.");
  if (options.verbose) Chi::log.Log() << Chi::program_timer.GetTimeString() << " Starting assembly";

  const size_t num_groups = uk_man_.unknowns_.front().num_components_;

  VecSet(rhs_, 0.0);
  for (const auto& cell : grid_.local_cells)
  {
    const size_t num_faces = cell.faces_.size();
    const auto& cell_mapping = sdm_.GetCellMapping(cell);
    const size_t num_nodes = cell_mapping.NumNodes();
    const auto cc_nodes = cell_mapping.GetNodeLocations();
    const auto& unit_cell_matrices = unit_cell_matrices_[cell.local_id_];

    const auto& cell_M_matrix = unit_cell_matrices.M_matrix;

    const auto& xs = mat_id_2_xs_map_.at(cell.material_id_);

    for (size_t g = 0; g < num_groups; ++g)
    {
      // Get coefficient and nodal src
      const double Dg = xs.Dg[g];

      std::vector<double> qg(num_nodes, 0.0);
      for (size_t j = 0; j < num_nodes; j++)
        qg[j] = q_vector[sdm_.MapDOFLocal(cell, j, uk_man_, 0, g)];

      // Assemble continuous terms
      for (size_t i = 0; i < num_nodes; i++)
      {
        const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);
        double entry_rhs_i = 0.0;
        for (size_t j = 0; j < num_nodes; j++)
          entry_rhs_i += qg[j] * cell_M_matrix[i][j];

        VecSetValue(rhs_, imap, entry_rhs_i, ADD_VALUES);
      } // for i

      // Assemble face terms
      for (size_t f = 0; f < num_faces; ++f)
      {
        const auto& face = cell.faces_[f];
        const auto& n_f = face.normal_;
        const size_t num_face_nodes = cell_mapping.NumFaceNodes(f);

        const auto& face_M = unit_cell_matrices.face_M_matrices[f];
        const auto& face_G = unit_cell_matrices.face_G_matrices[f];
        const auto& face_Si = unit_cell_matrices.face_Si_vectors[f];

        const double hm = HPerpendicular(cell, f);

        if (not face.has_neighbor_)
        {
          auto bc = DefaultBCDirichlet;
          if (bcs_.count(face.neighbor_id_) > 0) bc = bcs_.at(face.neighbor_id_);

          if (bc.type == BCType::DIRICHLET)
          {
            const double bc_value = bc.values[0];

            // Compute kappa
            double kappa = 1.0;
            if (cell.Type() == chi_mesh::CellType::SLAB)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYGON)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
              kappa = fmax(options.penalty_factor * 2.0 * Dg / hm, 0.25);

            // Assembly penalty terms
            for (size_t fi = 0; fi < num_face_nodes; ++fi)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t fj = 0; fj < num_face_nodes; ++fj)
              {
                const int jm = cell_mapping.MapFaceNode(f, fj);

                const double aij = kappa * face_M[i][jm];
                const double aij_bc_value = aij * bc_value;

                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for fi

            // Assemble gradient terms
            // For the following comments we use the notation:
            // Dk = n dot nabla bk

            // D* n dot (b_j^+ - b_j^-)*nabla b_i^-
            for (size_t i = 0; i < num_nodes; i++)
            {
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t j = 0; j < num_nodes; j++)
              {
                const double aij = -Dg * n_f.Dot(face_G[j][i] + face_G[i][j]);
                const double aij_bc_value = aij * bc_value;

                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for i
          }     // Dirichlet BC
          else if (bc.type == BCType::ROBIN)
          {
            const double bval = bc.values[1];
            const double fval = bc.values[2];

            if (std::fabs(bval) < 1.0e-12) continue; // a and f assumed zero

            for (size_t fi = 0; fi < num_face_nodes; fi++)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t ir = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              if (std::fabs(fval) >= 1.0e-12)
              {
                const double rhs_val = (fval / bval) * face_Si[i];

                VecSetValue(rhs_, ir, rhs_val, ADD_VALUES);
              } // if f nonzero
            }   // for fi
          }     // Robin BC
        }       // boundary face
      }         // for face
    }           // for g
  }             // for cell

  VecAssemblyBegin(rhs_);
  VecAssemblyEnd(rhs_);

  if (options.verbose)
    Chi::log.Log() << Chi::program_timer.GetTimeString() << " Assembly completed";
}

void
DiffusionMIPSolver::Assemble_b(Vec petsc_q_vector)
{
  const std::string fname = "lbs::acceleration::DiffusionMIPSolver::"
                            "Assemble_b";
  if (A_ == nullptr or rhs_ == nullptr or ksp_ == nullptr)
    throw std::logic_error(fname + ": Some or all PETSc elements are null. "
                                   "Check that Initialize has been called.");
  if (options.verbose) Chi::log.Log() << Chi::program_timer.GetTimeString() << " Starting assembly";

  const size_t num_groups = uk_man_.unknowns_.front().num_components_;

  const double* q_vector;
  VecGetArrayRead(petsc_q_vector, &q_vector);

  VecSet(rhs_, 0.0);
  for (const auto& cell : grid_.local_cells)
  {
    const size_t num_faces = cell.faces_.size();
    const auto& cell_mapping = sdm_.GetCellMapping(cell);
    const size_t num_nodes = cell_mapping.NumNodes();
    const auto cc_nodes = cell_mapping.GetNodeLocations();
    const auto& unit_cell_matrices = unit_cell_matrices_[cell.local_id_];

    const auto& cell_M_matrix = unit_cell_matrices.M_matrix;

    const auto& xs = mat_id_2_xs_map_.at(cell.material_id_);

    for (size_t g = 0; g < num_groups; ++g)
    {
      // Get coefficient and nodal src
      const double Dg = xs.Dg[g];

      std::vector<double> qg(num_nodes, 0.0);
      for (size_t j = 0; j < num_nodes; j++)
        qg[j] = q_vector[sdm_.MapDOFLocal(cell, j, uk_man_, 0, g)];

      // Assemble continuous terms
      for (size_t i = 0; i < num_nodes; i++)
      {
        const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);
        double entry_rhs_i = 0.0;
        for (size_t j = 0; j < num_nodes; j++)
          entry_rhs_i += qg[j] * cell_M_matrix[i][j];

        VecSetValue(rhs_, imap, entry_rhs_i, ADD_VALUES);
      } // for i

      // Assemble face terms
      for (size_t f = 0; f < num_faces; ++f)
      {
        const auto& face = cell.faces_[f];
        const auto& n_f = face.normal_;
        const size_t num_face_nodes = cell_mapping.NumFaceNodes(f);

        const auto& face_M = unit_cell_matrices.face_M_matrices[f];
        const auto& face_G = unit_cell_matrices.face_G_matrices[f];
        const auto& face_Si = unit_cell_matrices.face_Si_vectors[f];

        const double hm = HPerpendicular(cell, f);

        if (not face.has_neighbor_)
        {
          auto bc = DefaultBCDirichlet;
          if (bcs_.count(face.neighbor_id_) > 0) bc = bcs_.at(face.neighbor_id_);

          if (bc.type == BCType::DIRICHLET)
          {
            const double bc_value = bc.values[0];

            // Compute kappa
            double kappa = 1.0;
            if (cell.Type() == chi_mesh::CellType::SLAB)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYGON)
              kappa = fmax(options.penalty_factor * Dg / hm, 0.25);
            if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
              kappa = fmax(options.penalty_factor * 2.0 * Dg / hm, 0.25);

            // Assembly penalty terms
            for (size_t fi = 0; fi < num_face_nodes; ++fi)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t fj = 0; fj < num_face_nodes; ++fj)
              {
                const int jm = cell_mapping.MapFaceNode(f, fj);

                const double aij = kappa * face_M[i][jm];
                const double aij_bc_value = aij * bc_value;

                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for fi

            // Assemble gradient terms
            // For the following comments we use the notation:
            // Dk = n dot nabla bk

            // D* n dot (b_j^+ - b_j^-)*nabla b_i^-
            for (size_t i = 0; i < num_nodes; i++)
            {
              const int64_t imap = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              for (size_t j = 0; j < num_nodes; j++)
              {
                const double aij = -Dg * n_f.Dot(face_G[j][i] + face_G[i][j]);
                const double aij_bc_value = aij * bc_value;

                VecSetValue(rhs_, imap, aij_bc_value, ADD_VALUES);
              } // for fj
            }   // for i
          }     // Dirichlet BC
          else if (bc.type == BCType::ROBIN)
          {
            const double bval = bc.values[1];
            const double fval = bc.values[2];

            if (std::fabs(bval) < 1.0e-12) continue; // a and f assumed zero

            for (size_t fi = 0; fi < num_face_nodes; fi++)
            {
              const int i = cell_mapping.MapFaceNode(f, fi);
              const int64_t ir = sdm_.MapDOF(cell, i, uk_man_, 0, g);

              if (std::fabs(fval) >= 1.0e-12)
              {
                const double rhs_val = (fval / bval) * face_Si[i];

                VecSetValue(rhs_, ir, rhs_val, ADD_VALUES);
              } // if f nonzero
            }   // for fi
          }     // Robin BC
        }       // boundary face
      }         // for face
    }           // for g
  }             // for cell

  VecRestoreArrayRead(petsc_q_vector, &q_vector);

  VecAssemblyBegin(rhs_);
  VecAssemblyEnd(rhs_);

  if (options.verbose)
    Chi::log.Log() << Chi::program_timer.GetTimeString() << " Assembly completed";
}

double
DiffusionMIPSolver::HPerpendicular(const chi_mesh::Cell& cell, unsigned int f)
{
  const auto& cell_mapping = sdm_.GetCellMapping(cell);
  double hp;

  const size_t num_faces = cell.faces_.size();
  const size_t num_vertices = cell.vertex_ids_.size();

  const double volume = cell_mapping.CellVolume();
  const double face_area = cell_mapping.FaceArea(f);

  /**Lambda to compute surface area.*/
  auto ComputeSurfaceArea = [&cell_mapping, &num_faces]()
  {
    double surface_area = 0.0;
    for (size_t fr = 0; fr < num_faces; ++fr)
      surface_area += cell_mapping.FaceArea(fr);

    return surface_area;
  };

  //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%% SLAB
  if (cell.Type() == chi_mesh::CellType::SLAB) hp = volume / 2.0;
  //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%% POLYGON
  else if (cell.Type() == chi_mesh::CellType::POLYGON)
  {
    if (num_faces == 3) hp = 2.0 * volume / face_area;
    else if (num_faces == 4)
      hp = volume / face_area;
    else // Nv > 4
    {
      const double surface_area = ComputeSurfaceArea();

      if (num_faces % 2 == 0) hp = 4.0 * volume / surface_area;
      else
      {
        hp = 2.0 * volume / surface_area;
        hp += sqrt(2.0 * volume / (scdouble(num_faces) * sin(2.0 * M_PI / scdouble(num_faces))));
      }
    }
  }
  //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%% POLYHEDRON
  else if (cell.Type() == chi_mesh::CellType::POLYHEDRON)
  {
    const double surface_area = ComputeSurfaceArea();

    if (num_faces == 4) // Tet
      hp = 3 * volume / surface_area;
    else if (num_faces == 6 && num_vertices == 8) // Hex
      hp = volume / surface_area;
    else // Polyhedron
      hp = 6 * volume / surface_area;
  } // Polyhedron
  else
    throw std::logic_error("lbs::acceleration::DiffusionMIPSolver::HPerpendicular: "
                           "Unsupported cell type in call to HPerpendicular");

  return hp;
}

int
DiffusionMIPSolver::MapFaceNodeDisc(const chi_mesh::Cell& cur_cell,
                                    const chi_mesh::Cell& adj_cell,
                                    const std::vector<chi_mesh::Vector3>& cc_node_locs,
                                    const std::vector<chi_mesh::Vector3>& ac_node_locs,
                                    size_t ccf,
                                    size_t acf,
                                    size_t ccfi,
                                    double epsilon /*=1.0e-12*/)
{
  const auto& cur_cell_mapping = sdm_.GetCellMapping(cur_cell);
  const auto& adj_cell_mapping = sdm_.GetCellMapping(adj_cell);

  const int i = cur_cell_mapping.MapFaceNode(ccf, ccfi);
  const auto& node_i_loc = cc_node_locs[i];

  const size_t adj_face_num_nodes = adj_cell_mapping.NumFaceNodes(acf);

  for (size_t fj = 0; fj < adj_face_num_nodes; ++fj)
  {
    const int j = adj_cell_mapping.MapFaceNode(acf, fj);
    if ((node_i_loc - ac_node_locs[j]).NormSquare() < epsilon) return j;
  }

  throw std::logic_error(
    "lbs::acceleration::DiffusionMIPSolver::MapFaceNodeDisc: Mapping failure.");
}

#ifdef OPENSN_WITH_LUA
double
DiffusionMIPSolver::CallLuaXYZFunction(lua_State* L,
                                       const std::string& lua_func_name,
                                       const chi_mesh::Vector3& xyz)
{
  const std::string fname = "lbs::acceleration::DiffusionMIPSolver::"
                            "CallLuaXYZFunction";
  // Load lua function
  lua_getglobal(L, lua_func_name.c_str());

  // Error check lua function
  if (not lua_isfunction(L, -1))
    throw std::logic_error(fname + " attempted to access lua-function, " + lua_func_name +
                           ", but it seems the function"
                           " could not be retrieved.");

  // Push arguments
  lua_pushnumber(L, xyz.x);
  lua_pushnumber(L, xyz.y);
  lua_pushnumber(L, xyz.z);

  // Call lua function
  // 3 arguments, 1 result (double), 0=original error object
  double lua_return;
  if (lua_pcall(L, 3, 1, 0) == 0)
  {
    LuaCheckNumberValue(fname, L, -1);
    lua_return = lua_tonumber(L, -1);
  }
  else
    throw std::logic_error(fname + " attempted to call lua-function, " + lua_func_name +
                           ", but the call failed." + xyz.PrintStr());

  lua_pop(L, 1); // pop the double, or error code

  return lua_return;
}
#endif

} // namespace lbs::acceleration
