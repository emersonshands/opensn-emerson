#pragma once

#include "framework/math/spatial_discretization/finite_element/finite_element_base.h"
#include "framework/math/quadratures/spatial/line_quadrature.h"
#include "framework/math/quadratures/spatial/triangle_quadrature.h"
#include "framework/math/quadratures/spatial/tetrahedra_quadrature.h"

namespace opensn
{

/**Base class for PieceWiseLinear based discretization.
 * \ingroup doc_SpatialDiscretization*/
class PieceWiseLinearBase : public FiniteElementBase
{
protected:
  /**Constructor*/
  explicit PieceWiseLinearBase(const MeshContinuum& grid,
                               QuadratureOrder q_order,
                               SDMType sdm_type,
                               CoordinateSystemType cs_type);

  QuadratureLine line_quad_order_arbitrary_;
  QuadratureTriangle tri_quad_order_arbitrary_;
  QuadratureTetrahedron tet_quad_order_arbitrary_;

  void CreateCellMappings();
};

} // namespace opensn
