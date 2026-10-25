# Aggregation of the independent 45 percent milestone lane fragments.
# Each scripts/interop_lane_<name>.mk appends its targets to InteropLaneTests,
# InteropLaneSanitizers and InteropLaneCoverage. A fragment owns only its own
# targets, so lanes never edit scripts/interop.mk concurrently. The aggregate
# targets below add prerequisites to the existing interop-test,
# interop-sanitizers and interop-coverage rules without changing their recipes.
InteropLaneTests ?=
InteropLaneSanitizers ?=
InteropLaneCoverage ?=

interop-test: $(InteropLaneTests)
interop-sanitizers: $(InteropLaneSanitizers)
interop-coverage: $(InteropLaneCoverage)
