// tea_common.h
// Teascript commons

#ifndef TEA_COMMON_H
#define TEA_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEA_NAN_TAGGING

#define TEA_DEBUG

//#define TEA_DEBUG_TOKENS
//#define TEA_DEBUG_PRINT_CODE
//#define TEA_DEBUG_TRACE_EXECUTION
//#define TEA_DEBUG_TRACE_MEMORY
//#define TEA_DEBUG_FINAL_MEMORY
//#define TEA_DEBUG_STRESS_GC
//#define TEA_DEBUG_LOG_GC

#ifdef TEA_DEBUG
#include <assert.h>
#define tea_assert(c)   assert(c)
#else
#define tea_assert(c)   ((void)0)
#endif

#ifndef _MSC_VER
#define TEA_COMPUTED_GOTO
#endif

#define UINT8_COUNT (UINT8_MAX + 1)

#endif