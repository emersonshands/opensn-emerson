#include "framework/lua.h"

#include "framework/mesh/volume_mesher/volume_mesher.h"
#include "volumemesher_lua.h"
#include "framework/console/console.h"

RegisterLuaFunctionAsIs(chiVolumeMesherSetMatIDToAll);

int
chiVolumeMesherSetMatIDToAll(lua_State* L)
{
  int num_args = lua_gettop(L);
  if (num_args != 1) LuaPostArgAmountError(__FUNCTION__, 1, num_args);

  LuaCheckNilValue(__FUNCTION__, L, 1);

  int mat_id = lua_tonumber(L, 1);

  chi_mesh::VolumeMesher::SetMatIDToAll(mat_id);
  return 0;
}
