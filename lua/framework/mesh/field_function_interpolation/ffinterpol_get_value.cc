#include "framework/lua.h"
#include "framework/field_functions/interpolation/ffinter_point.h"
#include "framework/field_functions/interpolation/ffinter_line.h"
#include "framework/field_functions/interpolation/ffinter_volume.h"
#include "framework/runtime.h"
#include "framework/logging/log.h"
#include "ffinterpol_lua.h"
#include "framework/console/console.h"

using namespace opensn;

namespace opensnlua
{

RegisterLuaFunctionNamespace(FFInterpolationGetValue, fieldfunc, GetValue);

int
FFInterpolationGetValue(lua_State* L)
{
  const std::string fname = "fieldfunc.GetValue";
  LuaCheckArgs<size_t>(L, fname);

  // Get handle to field function
  const auto ffihandle = LuaArg<size_t>(L, 1);

  auto p_ffi = opensn::GetStackItemPtr(opensn::field_func_interpolation_stack, ffihandle, fname);

  if (p_ffi->Type() == FieldFunctionInterpolationType::POINT)
  {
    auto& cur_ffi_point = dynamic_cast<opensn::FieldFunctionInterpolationPoint&>(*p_ffi);
    double value = cur_ffi_point.GetPointValue();
    return LuaReturn(L, value);
  }
  else if (p_ffi->Type() == FieldFunctionInterpolationType::LINE)
  {
    auto& cur_ffi_line = dynamic_cast<opensn::FieldFunctionInterpolationLine&>(*p_ffi);

    lua_newtable(L);

    for (int ff = 0; ff < cur_ffi_line.GetFieldFunctions().size(); ff++)
    {
      LuaPush(L, ff + 1);

      lua_newtable(L);
      const auto& ff_ctx = cur_ffi_line.GetFFContexts()[ff];
      for (int p = 0; p < cur_ffi_line.GetInterpolationPoints().size(); p++)
        LuaPushTableKey(L, p + 1, ff_ctx.interpolation_points_values[p]);
      lua_settable(L, -3);
    }

    return 1;
  }
  else if (p_ffi->Type() == FieldFunctionInterpolationType::VOLUME)
  {
    auto& cur_ffi_volume = dynamic_cast<opensn::FieldFunctionInterpolationVolume&>(*p_ffi);
    double value = cur_ffi_volume.GetOpValue();
    return LuaReturn(L, value);
  }
  else
  {
    opensn::log.Log0Warning()
      << fname + " is currently only supported for POINT, LINE and VOLUME interpolator types.";
  }

  return LuaReturn(L);
}

} // namespace opensnlua
