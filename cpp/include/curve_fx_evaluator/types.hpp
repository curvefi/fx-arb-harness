#pragma once

#include <string>
#include <string_view>
#include <type_traits>


namespace curve_fx::evaluator {

#if defined(ARB_MODE_LD) && defined(ARB_MODE_F64)
#error "select exactly one evaluator arithmetic mode"
#elif defined(ARB_MODE_LD)
using RealT = long double;
inline constexpr std::string_view NUMERIC_MODE = "longdouble";
#elif defined(ARB_MODE_F64)
using RealT = double;
inline constexpr std::string_view NUMERIC_MODE = "double";
#else
#error "evaluator target must define ARB_MODE_LD or ARB_MODE_F64"
#endif

template <typename T>
using MetricF = std::conditional_t<std::is_same_v<T, long double>, long double, double>;



} // namespace curve_fx::evaluator
