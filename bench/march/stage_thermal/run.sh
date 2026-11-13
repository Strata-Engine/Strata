#!/bin/sh
# units: one column of one pass (66 x 66 columns, 4 passes a relax)
cd "$(dirname "$0")" && . ../common.sh
build stage_thermal
row "March, Strata.Thermal as it stands"  out/stage_thermal_march 0 1000 17424
row "March, no Float-valued if"           out/stage_thermal_march 1 3000 17424
row "C"                                   out/stage_thermal_c     0 20000 17424
echo "weighted sum of the relaxed patch: March $(VARIANT=0 REPS=0 out/stage_thermal_march | tail -1), no-if $(VARIANT=1 REPS=0 out/stage_thermal_march | tail -1), C $(REPS=0 out/stage_thermal_c | tail -1)"
ir stage_thermal
