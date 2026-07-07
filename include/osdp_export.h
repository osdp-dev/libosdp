/*
 * Copyright (c) 2025-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_EXPORT_H_
#define _OSDP_EXPORT_H_

/* ---------- feature detection helpers ---------- */
#if defined(__has_cpp_attribute)
  #define API_HAS_CPP_ATTR(x) __has_cpp_attribute(x)
#else
  #define API_HAS_CPP_ATTR(x) 0
#endif

#if defined(__has_attribute)
  #define API_HAS_ATTR(x) __has_attribute(x)
#else
  #define API_HAS_ATTR(x) 0
#endif

/* ---------- Export / Import / Visibility ---------- */
/*
 * Windows/Cygwin contract (consumer-facing):
 *   OSDP_STATIC_DEFINE  -> consumer links the static library; no decoration.
 *   BUILDING_API        -> producer is building the shared library; dllexport.
 *   (neither)           -> consumer links the shared library; dllimport.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(OSDP_STATIC_DEFINE)
    #define API_EXPORT
  #elif defined(BUILDING_API)
    #if defined(__GNUC__)
      #define API_EXPORT __attribute__ ((dllexport))
    #else
      #define API_EXPORT __declspec(dllexport)
    #endif
  #else
    #if defined(__GNUC__)
      #define API_EXPORT __attribute__ ((dllimport))
    #else
      #define API_EXPORT __declspec(dllimport)
    #endif
  #endif
  #define API_NO_EXPORT
#else
  #if defined(__GNUC__) && (__GNUC__ >= 4)
    #define API_EXPORT __attribute__ ((visibility ("default")))
    #define API_NO_EXPORT  __attribute__ ((visibility ("hidden")))
  #else
    #define API_EXPORT
    #define API_NO_EXPORT
  #endif
#endif

/* ---------- Deprecation (with message) ---------- */

/*
 * Undefine API_DEPRECATED before redefining it. On macOS the SDK header
 * <os/availability.h> defines API_DEPRECATED with a different signature
 * (platform/version specifiers, not a plain string), which causes macro
 * expansion errors when osdp.h uses OSDP_DEPRECATED_EXPORT("msg").
 */
#ifdef API_DEPRECATED
#undef API_DEPRECATED
#endif

/* Prefer C++ [[deprecated("msg")]] if available, otherwise compiler specifics. */
#if defined(__cplusplus) && API_HAS_CPP_ATTR(deprecated)
  /* [[deprecated("msg")]] supported */
  #define API_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(_MSC_VER)
  /* MSVC supports __declspec(deprecated("msg")) */
  #define API_DEPRECATED(msg) __declspec(deprecated(msg))
#elif (defined(__clang__) && API_HAS_ATTR(deprecated)) \
      || (defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5)))
  /* Clang and modern GCC support attribute deprecated("msg") */
  #define API_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif (defined(__GNUC__) || defined(__clang__))
  /* Older GCC/Clang: attribute deprecated without message */
  #define API_DEPRECATED(msg) __attribute__((deprecated))
#else
  /* Unknown compiler: no-op */
  #define API_DEPRECATED(msg)
#endif

/* ---------- helpers ---------- */
#define OSDP_EXPORT API_EXPORT
#define OSDP_NO_EXPORT API_NO_EXPORT
#define OSDP_DEPRECATED_EXPORT(msg) API_DEPRECATED(msg) API_EXPORT
#define OSDP_DEPRECATED_NO_EXPORT(msg) API_DEPRECATED(msg) API_NO_EXPORT

#endif /* _OSDP_EXPORT_H_ */
