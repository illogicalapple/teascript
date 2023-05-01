// tea_common.h
// Teascript commons

#ifndef TEA_COMMON_H
#define TEA_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEA_NAN_TAGGING

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