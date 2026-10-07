#pragma once

namespace curve_fx::evaluator {

#if defined(ARB_MODE_LD) && defined(ARB_MODE_F64)
#error "select exactly one evaluator arithmetic mode"
#elif defined(ARB_MODE_LD)
using RealT = long double;
#elif defined(ARB_MODE_F64)
using RealT = double;
#else
#error "evaluator target must define ARB_MODE_LD or ARB_MODE_F64"
#endif

} // namespace curve_fx::evaluator
