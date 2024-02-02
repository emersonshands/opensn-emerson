#include "framework/lua.h"

#include "modules/diffusion_solver/diffusion_solver.h"

#include "framework/runtime.h"

using namespace opensn;

int
DiffusionInitialize(lua_State* L)
{
  int solver_index = lua_tonumber(L, 1);

  auto& solver =
    opensn::GetStackItem<diffusion::Solver>(opensn::object_stack, solver_index, __FUNCTION__);

  bool success = solver.Initialize(true);

  lua_pushnumber(L, success);
  return 1;
}
