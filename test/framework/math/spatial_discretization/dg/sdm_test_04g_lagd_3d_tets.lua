dofile("mesh_3d_tets.lua")

unit_tests.math_SDM_Test02_DisContinuous
({
  sdm_type = "LagrangeD",
  --export_vtk = true,
});
