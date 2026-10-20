/* stela.h: the Stela annotation template, version 1.
 * Don't include this file. `stela vendor` generates each library's
 * <lib>_stela.h from it, with the library's macro prefix. Outside the Stela
 * extractor every annotation expands to nothing.
 *
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License. */
#if !defined(STELA_VERSION) || (STELA_VERSION < 1)
#undef STELA_VERSION
#define STELA_VERSION 1

#undef STELA_ANNOTATE_
#if defined(__clang__) && defined(STELA_EXTRACT)
#define STELA_ANNOTATE_(s) __attribute__((annotate("stela:" s)))
#else
#define STELA_ANNOTATE_(s)
#endif

/* Fixed underlying types where supported: C++, and C23 except MSVC's C
 * compiler. Elsewhere an exact-width typedef plus plain enumerators. The
 * layout is the same either way. */
#undef STELA_FIXED_ENUMS_
#if defined(__cplusplus) ||                                     \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L && \
     (defined(__clang__) || !defined(_MSC_VER)))
#define STELA_FIXED_ENUMS_ 1
#else
#define STELA_FIXED_ENUMS_ 0
#endif

#undef STELA_ENUM
#undef STELA_FLAGS
#if STELA_FIXED_ENUMS_
#define STELA_ENUM(type, name) \
  enum name : type;                                  \
  typedef enum name name;                            \
  enum STELA_ANNOTATE_("enum=" #name) name : type
#define STELA_FLAGS(type, name) \
  typedef type name;            \
  enum STELA_ANNOTATE_("flags=" #name) : type
#else
#define STELA_ENUM(type, name) \
  typedef type name;           \
  enum
#define STELA_FLAGS(type, name) \
  typedef type name;            \
  enum
#endif

/* Parameters, results, and fields */
#undef STELA_OUT
#define STELA_OUT STELA_ANNOTATE_("out")
#undef STELA_INOUT
#define STELA_INOUT STELA_ANNOTATE_("inout")
#undef STELA_OPTIONAL
#define STELA_OPTIONAL STELA_ANNOTATE_("optional")
#undef STELA_NULLABLE
#define STELA_NULLABLE STELA_ANNOTATE_("nullable")
#undef STELA_TRANSFER_NONE
#define STELA_TRANSFER_NONE STELA_ANNOTATE_("transfer=none")
#undef STELA_TRANSFER_FULL
#define STELA_TRANSFER_FULL STELA_ANNOTATE_("transfer=full")
#undef STELA_COUNT
#define STELA_COUNT(param) STELA_ANNOTATE_("count=" #param)
#undef STELA_ZERO_TERMINATED
#define STELA_ZERO_TERMINATED STELA_ANNOTATE_("zero_terminated")
#undef STELA_ENCODING
#define STELA_ENCODING(encoding) STELA_ANNOTATE_("encoding=" #encoding)

/* Callbacks */
#undef STELA_SCOPE_CALL
#define STELA_SCOPE_CALL STELA_ANNOTATE_("scope=call")
#undef STELA_SCOPE_ASYNC
#define STELA_SCOPE_ASYNC STELA_ANNOTATE_("scope=async")
#undef STELA_SCOPE_OBJECT
#define STELA_SCOPE_OBJECT STELA_ANNOTATE_("scope=object")
#undef STELA_SCOPE_NOTIFIED
#define STELA_SCOPE_NOTIFIED STELA_ANNOTATE_("scope=notified")
#undef STELA_SCOPE_FOREVER
#define STELA_SCOPE_FOREVER STELA_ANNOTATE_("scope=forever")
#undef STELA_CLOSURE
#define STELA_CLOSURE(param) STELA_ANNOTATE_("closure=" #param)
#undef STELA_DESTROY
#define STELA_DESTROY(param) STELA_ANNOTATE_("destroy=" #param)

/* Lifecycle */
#undef STELA_SINCE
#define STELA_SINCE(version) STELA_ANNOTATE_("since=" #version)
#undef STELA_DEPRECATED
#define STELA_DEPRECATED STELA_ANNOTATE_("deprecated")
#undef STELA_EXPERIMENTAL
#define STELA_EXPERIMENTAL STELA_ANNOTATE_("experimental")

/* Tooling */
#undef STELA_ALLOW
#define STELA_ALLOW(rule) STELA_ANNOTATE_("allow=" #rule)
#undef STELA_ATTR
#define STELA_ATTR(key_value) STELA_ANNOTATE_(key_value)

#endif /* STELA_VERSION */
