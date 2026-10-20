/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

#define _CRT_SECURE_NO_WARNINGS

#include "clang-c/Index.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define getcwd _getcwd
#define strcasecmp _stricmp
#else
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STELA_VERSION "0.1.0"
#define MIN_LIBCLANG_MAJOR 22

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} Buffer;

typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} Strings;

typedef struct {
  char *path;
  bool directory;
} OwnedPath;

typedef struct {
  OwnedPath *items;
  size_t count;
  size_t capacity;
} OwnedPaths;

typedef enum {
  DECL_FUNCTION,
  DECL_TYPEDEF,
  DECL_STRUCT,
  DECL_UNION,
  DECL_ENUM,
  DECL_MACRO
} DeclarationKind;

typedef struct {
  DeclarationKind kind;
  CXCursor cursor;
  char *name;
  char *file;
  char *usr;
  unsigned line;
  unsigned column;
  unsigned file_order;
  bool macro_function_like;
  Strings macro_tokens;
  bool has_macro_value;
  bool macro_value_unsigned;
  unsigned long long macro_unsigned_value;
  long long macro_signed_value;
} Declaration;

typedef struct {
  Declaration *items;
  size_t count;
  size_t capacity;
} Declarations;

typedef struct {
  char *target;
  char *root;
  char *out;
  Strings includes;
  Strings defines;
  OwnedPaths owns;
  Strings headers;
} Options;

typedef struct {
  const Options *options;
  CXTranslationUnit tu;
  Declarations declarations;
  Strings files;
  bool failed;
} CollectContext;

typedef struct {
  CXCursor *items;
  size_t count;
  size_t capacity;
} Cursors;

static void fail_oom(void) {
  fputs("stela-extract: out of memory\n", stderr);
  exit(2);
}

static void *xmalloc(size_t size) {
  void *value = malloc(size == 0 ? 1 : size);
  if (value == NULL) {
    fail_oom();
  }
  return value;
}

static void *xrealloc(void *value, size_t size) {
  void *result = realloc(value, size == 0 ? 1 : size);
  if (result == NULL) {
    fail_oom();
  }
  return result;
}

static char *xstrdup(const char *value) {
  size_t length = strlen(value);
  char *copy = (char *)xmalloc(length + 1);
  memcpy(copy, value, length + 1);
  return copy;
}

static void buffer_reserve(Buffer *buffer, size_t extra) {
  size_t required = buffer->length + extra + 1;
  if (required <= buffer->capacity) {
    return;
  }
  size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
  while (capacity < required) {
    capacity *= 2;
  }
  buffer->data = (char *)xrealloc(buffer->data, capacity);
  buffer->capacity = capacity;
}

static void buffer_append_n(Buffer *buffer, const char *value, size_t length) {
  buffer_reserve(buffer, length);
  memcpy(buffer->data + buffer->length, value, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

static void buffer_append(Buffer *buffer, const char *value) {
  buffer_append_n(buffer, value, strlen(value));
}

static void buffer_append_char(Buffer *buffer, char value) {
  buffer_reserve(buffer, 1);
  buffer->data[buffer->length++] = value;
  buffer->data[buffer->length] = '\0';
}

static void buffer_append_unsigned(Buffer *buffer, unsigned long long value) {
  char text[32];
  int length = snprintf(text, sizeof(text), "%llu", value);
  if (length < 0) {
    fputs("stela-extract: cannot format integer\n", stderr);
    exit(2);
  }
  buffer_append_n(buffer, text, (size_t)length);
}

static void buffer_append_signed(Buffer *buffer, long long value) {
  char text[32];
  int length = snprintf(text, sizeof(text), "%lld", value);
  if (length < 0) {
    fputs("stela-extract: cannot format integer\n", stderr);
    exit(2);
  }
  buffer_append_n(buffer, text, (size_t)length);
}

static void buffer_free(Buffer *buffer) {
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

static void append_json_string(Buffer *buffer, const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  buffer_append_char(buffer, '"');
  while (*cursor != '\0') {
    unsigned char c = *cursor++;
    switch (c) {
      case '"':
        buffer_append(buffer, "\\\"");
        break;
      case '\\':
        buffer_append(buffer, "\\\\");
        break;
      case '\b':
        buffer_append(buffer, "\\b");
        break;
      case '\f':
        buffer_append(buffer, "\\f");
        break;
      case '\n':
        buffer_append(buffer, "\\n");
        break;
      case '\r':
        buffer_append(buffer, "\\r");
        break;
      case '\t':
        buffer_append(buffer, "\\t");
        break;
      default:
        if (c < 0x20) {
          char escaped[7];
          (void)snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          buffer_append(buffer, escaped);
        } else {
          buffer_append_char(buffer, (char)c);
        }
        break;
    }
  }
  buffer_append_char(buffer, '"');
}

static char *take_cx_string(CXString value) {
  const char *text = clang_getCString(value);
  char *copy = xstrdup(text == NULL ? "" : text);
  clang_disposeString(value);
  return copy;
}

static void strings_push_owned(Strings *strings, char *value) {
  if (strings->count == strings->capacity) {
    size_t capacity = strings->capacity == 0 ? 8 : strings->capacity * 2;
    strings->items = (char **)xrealloc(strings->items, capacity * sizeof(char *));
    strings->capacity = capacity;
  }
  strings->items[strings->count++] = value;
}

static void strings_push(Strings *strings, const char *value) {
  strings_push_owned(strings, xstrdup(value));
}

static void strings_free(Strings *strings) {
  size_t i;
  for (i = 0; i < strings->count; ++i) {
    free(strings->items[i]);
  }
  free(strings->items);
  memset(strings, 0, sizeof(*strings));
}

static void cursors_push(Cursors *cursors, CXCursor value) {
  if (cursors->count == cursors->capacity) {
    size_t capacity = cursors->capacity == 0 ? 8 : cursors->capacity * 2;
    cursors->items = (CXCursor *)xrealloc(cursors->items, capacity * sizeof(CXCursor));
    cursors->capacity = capacity;
  }
  cursors->items[cursors->count++] = value;
}

static void declarations_push(Declarations *declarations, Declaration value) {
  if (declarations->count == declarations->capacity) {
    size_t capacity = declarations->capacity == 0 ? 32 : declarations->capacity * 2;
    declarations->items =
        (Declaration *)xrealloc(declarations->items, capacity * sizeof(Declaration));
    declarations->capacity = capacity;
  }
  declarations->items[declarations->count++] = value;
}

static void declarations_free(Declarations *declarations) {
  size_t i;
  for (i = 0; i < declarations->count; ++i) {
    Declaration *declaration = &declarations->items[i];
    free(declaration->name);
    free(declaration->file);
    free(declaration->usr);
    strings_free(&declaration->macro_tokens);
  }
  free(declarations->items);
  memset(declarations, 0, sizeof(*declarations));
}

static bool is_path_separator(char value) {
  return value == '/' || value == '\\';
}

static void normalize_separators(char *path) {
  char *cursor;
  for (cursor = path; *cursor != '\0'; ++cursor) {
    if (*cursor == '\\') {
      *cursor = '/';
    }
  }
  while (cursor > path + 1 && cursor[-1] == '/') {
    *--cursor = '\0';
  }
}

static bool is_absolute_path(const char *path) {
#ifdef _WIN32
  return isalpha((unsigned char)path[0]) && path[1] == ':' && is_path_separator(path[2]);
#else
  return path[0] == '/';
#endif
}

static char *absolute_path(const char *path) {
#ifdef _WIN32
  char *result = _fullpath(NULL, path, 0);
  if (result == NULL) {
    fprintf(stderr, "stela-extract: cannot resolve path %s: %s\n", path, strerror(errno));
    return NULL;
  }
#else
  char *result = realpath(path, NULL);
  if (result == NULL) {
    fprintf(stderr, "stela-extract: cannot resolve path %s: %s\n", path, strerror(errno));
    return NULL;
  }
#endif
  normalize_separators(result);
  return result;
}

static int path_compare(const char *left, const char *right) {
#ifdef _WIN32
  return strcasecmp(left, right);
#else
  return strcmp(left, right);
#endif
}

static bool path_equal(const char *left, const char *right) {
  return path_compare(left, right) == 0;
}

static bool path_is_within(const char *path, const char *directory) {
  size_t length = strlen(directory);
  if (
#ifdef _WIN32
      _strnicmp(path, directory, length) != 0
#else
      strncmp(path, directory, length) != 0
#endif
  ) {
    return false;
  }
  return path[length] == '\0' || path[length] == '/';
}

static char *relative_path(const char *root, const char *path) {
  size_t length = strlen(root);
  if (!path_is_within(path, root)) {
    fprintf(stderr, "stela-extract: path is outside --root: %s\n", path);
    return NULL;
  }
  if (path[length] == '/') {
    ++length;
  }
  return xstrdup(path + length);
}

static bool path_exists(const char *path, bool *directory) {
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  *directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  return true;
#else
  struct stat info;
  if (stat(path, &info) != 0) {
    return false;
  }
  *directory = S_ISDIR(info.st_mode);
  return true;
#endif
}

static void owned_paths_push(OwnedPaths *paths, char *path, bool directory) {
  if (paths->count == paths->capacity) {
    size_t capacity = paths->capacity == 0 ? 8 : paths->capacity * 2;
    paths->items = (OwnedPath *)xrealloc(paths->items, capacity * sizeof(OwnedPath));
    paths->capacity = capacity;
  }
  paths->items[paths->count].path = path;
  paths->items[paths->count].directory = directory;
  ++paths->count;
}

static void owned_paths_free(OwnedPaths *paths) {
  size_t i;
  for (i = 0; i < paths->count; ++i) {
    free(paths->items[i].path);
  }
  free(paths->items);
  memset(paths, 0, sizeof(*paths));
}

static bool is_owned(const Options *options, const char *path) {
  size_t i;
  for (i = 0; i < options->owns.count; ++i) {
    const OwnedPath *owned = &options->owns.items[i];
    if (owned->directory ? path_is_within(path, owned->path) : path_equal(path, owned->path)) {
      return true;
    }
  }
  return false;
}

static void usage(void) {
  fputs(
      "usage: stela-extract --target <triple> --root <dir> --out <file.json>\n"
      "                     [--include <dir>]... [--define <NAME[=VALUE]>]...\n"
      "                     [--owns <path>]... <header>...\n",
      stderr);
}

static bool take_option_value(int argc, char **argv, int *index, const char **value) {
  if (*index + 1 >= argc) {
    fprintf(stderr, "stela-extract: %s needs a value\n", argv[*index]);
    return false;
  }
  *value = argv[++*index];
  return true;
}

static bool parse_options(int argc, char **argv, Options *options) {
  int i;
  bool positional = false;
  for (i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value;
    if (!positional && strcmp(arg, "--") == 0) {
      positional = true;
      continue;
    }
    if (!positional && strcmp(arg, "--target") == 0) {
      if (!take_option_value(argc, argv, &i, &value)) {
        return false;
      }
      free(options->target);
      options->target = xstrdup(value);
    } else if (!positional && strcmp(arg, "--root") == 0) {
      if (!take_option_value(argc, argv, &i, &value)) {
        return false;
      }
      free(options->root);
      options->root = absolute_path(value);
      if (options->root == NULL) {
        return false;
      }
    } else if (!positional && strcmp(arg, "--out") == 0) {
      if (!take_option_value(argc, argv, &i, &value)) {
        return false;
      }
      free(options->out);
      options->out = absolute_path(value);
      if (options->out == NULL) {
        return false;
      }
    } else if (!positional && strcmp(arg, "--include") == 0) {
      char *absolute;
      if (!take_option_value(argc, argv, &i, &value)) {
        return false;
      }
      absolute = absolute_path(value);
      if (absolute == NULL) {
        return false;
      }
      strings_push_owned(&options->includes, absolute);
    } else if (!positional && strcmp(arg, "--define") == 0) {
      if (!take_option_value(argc, argv, &i, &value)) {
        return false;
      }
      strings_push(&options->defines, value);
    } else if (!positional && strcmp(arg, "--owns") == 0) {
      char *absolute;
      bool directory;
      if (!take_option_value(argc, argv, &i, &value)) {
        return false;
      }
      absolute = absolute_path(value);
      if (absolute == NULL) {
        return false;
      }
      if (!path_exists(absolute, &directory)) {
        fprintf(stderr, "stela-extract: --owns path does not exist: %s\n", value);
        free(absolute);
        return false;
      }
      owned_paths_push(&options->owns, absolute, directory);
    } else if (!positional && arg[0] == '-') {
      fprintf(stderr, "stela-extract: unknown option %s\n", arg);
      return false;
    } else {
      char *absolute = absolute_path(arg);
      bool directory;
      positional = true;
      if (absolute == NULL) {
        return false;
      }
      if (!path_exists(absolute, &directory) || directory) {
        fprintf(stderr, "stela-extract: header does not exist: %s\n", arg);
        free(absolute);
        return false;
      }
      strings_push_owned(&options->headers, absolute);
    }
  }

  if (
      options->target == NULL || options->root == NULL || options->out == NULL ||
      options->headers.count == 0) {
    usage();
    return false;
  }
  if (!is_absolute_path(options->root) || !is_absolute_path(options->out)) {
    fputs("stela-extract: --root and --out must resolve to absolute paths\n", stderr);
    return false;
  }
  for (i = 0; (size_t)i < options->headers.count; ++i) {
    char *relative = relative_path(options->root, options->headers.items[i]);
    if (relative == NULL) {
      return false;
    }
    free(relative);
  }
  for (i = 0; (size_t)i < options->owns.count; ++i) {
    char *relative = relative_path(options->root, options->owns.items[i].path);
    if (relative == NULL) {
      return false;
    }
    free(relative);
  }
  if (options->owns.count == 0) {
    for (i = 0; (size_t)i < options->headers.count; ++i) {
      owned_paths_push(&options->owns, xstrdup(options->headers.items[i]), false);
    }
  }
  return true;
}

static void options_free(Options *options) {
  free(options->target);
  free(options->root);
  free(options->out);
  strings_free(&options->includes);
  strings_free(&options->defines);
  owned_paths_free(&options->owns);
  strings_free(&options->headers);
  memset(options, 0, sizeof(*options));
}

static Buffer make_main_file(const Options *options, const char *extra) {
  Buffer source = {0};
  size_t i;
  for (i = 0; i < options->headers.count; ++i) {
    buffer_append(&source, "#include \"");
    buffer_append(&source, options->headers.items[i]);
    buffer_append(&source, "\"\n");
  }
  if (extra != NULL) {
    buffer_append(&source, extra);
  }
  return source;
}

static Strings make_clang_args(const Options *options) {
  Strings args = {0};
  size_t i;
  Buffer value = {0};
  strings_push(&args, "-x");
  strings_push(&args, "c");
  strings_push(&args, "-std=c23");
  strings_push(&args, "-ffreestanding");
  strings_push(&args, "-DSTELA_EXTRACT");
  buffer_append(&value, "--target=");
  buffer_append(&value, options->target);
  strings_push(&args, value.data);
  buffer_free(&value);
  if (strncmp(options->target, "i686-", 5) == 0) {
    strings_push(&args, "-mrtd");
  }
  for (i = 0; i < options->includes.count; ++i) {
    value = (Buffer){0};
    buffer_append(&value, "-I");
    buffer_append(&value, options->includes.items[i]);
    strings_push(&args, value.data);
    buffer_free(&value);
  }
  for (i = 0; i < options->defines.count; ++i) {
    value = (Buffer){0};
    buffer_append(&value, "-D");
    buffer_append(&value, options->defines.items[i]);
    strings_push(&args, value.data);
    buffer_free(&value);
  }
  return args;
}

static bool translation_unit_has_errors(CXTranslationUnit tu, bool print) {
  unsigned count = clang_getNumDiagnostics(tu);
  unsigned i;
  bool failed = false;
  for (i = 0; i < count; ++i) {
    CXDiagnostic diagnostic = clang_getDiagnostic(tu, i);
    enum CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
    if (severity >= CXDiagnostic_Error) {
      failed = true;
    }
    if (print) {
      CXString formatted = clang_formatDiagnostic(
          diagnostic, clang_defaultDiagnosticDisplayOptions());
      const char *text = clang_getCString(formatted);
      if (text != NULL) {
        fprintf(stderr, "%s\n", text);
      }
      clang_disposeString(formatted);
    }
    clang_disposeDiagnostic(diagnostic);
  }
  return failed;
}

static CXTranslationUnit parse_translation_unit(
    CXIndex index,
    const Options *options,
    const char *main_name,
    const Buffer *source,
    bool print_diagnostics) {
  Strings args = make_clang_args(options);
  struct CXUnsavedFile unsaved;
  CXTranslationUnit tu = NULL;
  enum CXErrorCode error;
  unsaved.Filename = main_name;
  unsaved.Contents = source->data == NULL ? "" : source->data;
  unsaved.Length = (unsigned long)source->length;
  error = clang_parseTranslationUnit2(
      index,
      main_name,
      (const char *const *)args.items,
      (int)args.count,
      &unsaved,
      1,
      CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_DetailedPreprocessingRecord,
      &tu);
  strings_free(&args);
  if (error != CXError_Success || tu == NULL) {
    if (print_diagnostics) {
      fprintf(stderr, "stela-extract: libclang parse failed with error %d\n", (int)error);
    }
    if (tu != NULL) {
      clang_disposeTranslationUnit(tu);
    }
    return NULL;
  }
  if (translation_unit_has_errors(tu, print_diagnostics)) {
    clang_disposeTranslationUnit(tu);
    return NULL;
  }
  return tu;
}

static bool get_cursor_location(
    CXCursor cursor, char **file, unsigned *line, unsigned *column) {
  CXFile cx_file = NULL;
  CXSourceLocation location = clang_getCursorLocation(cursor);
  char *name;
  clang_getExpansionLocation(location, &cx_file, line, column, NULL);
  if (cx_file == NULL) {
    return false;
  }
  name = take_cx_string(clang_getFileName(cx_file));
  if (name[0] == '\0') {
    free(name);
    return false;
  }
  normalize_separators(name);
  *file = name;
  return true;
}

static unsigned get_file_order(CollectContext *context, const char *file) {
  size_t i;
  for (i = 0; i < context->files.count; ++i) {
    if (path_equal(context->files.items[i], file)) {
      return (unsigned)i;
    }
  }
  strings_push(&context->files, file);
  return (unsigned)(context->files.count - 1);
}

static Declaration *find_declaration_by_usr(CollectContext *context, const char *usr) {
  size_t i;
  if (usr[0] == '\0') {
    return NULL;
  }
  for (i = 0; i < context->declarations.count; ++i) {
    if (strcmp(context->declarations.items[i].usr, usr) == 0) {
      return &context->declarations.items[i];
    }
  }
  return NULL;
}

typedef struct {
  Strings annotations;
} AnnotationContext;

static enum CXChildVisitResult collect_annotations_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  AnnotationContext *context = (AnnotationContext *)client_data;
  char *spelling;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_AnnotateAttr) {
    return CXChildVisit_Continue;
  }
  spelling = take_cx_string(clang_getCursorSpelling(cursor));
  if (strncmp(spelling, "stela:", 6) == 0) {
    strings_push_owned(&context->annotations, spelling);
  } else {
    free(spelling);
  }
  return CXChildVisit_Continue;
}

static Strings collect_annotations(CXCursor cursor) {
  AnnotationContext context = {0};
  clang_visitChildren(cursor, collect_annotations_visitor, &context);
  return context.annotations;
}

static char *flags_name(CXCursor cursor) {
  Strings annotations = collect_annotations(cursor);
  size_t i;
  char *name = NULL;
  for (i = 0; i < annotations.count; ++i) {
    if (strncmp(annotations.items[i], "stela:flags=", 12) == 0) {
      name = xstrdup(annotations.items[i] + 12);
      break;
    }
  }
  strings_free(&annotations);
  return name;
}

static Strings macro_tokens(CXTranslationUnit tu, CXCursor cursor) {
  Strings result = {0};
  CXToken *tokens = NULL;
  unsigned count = 0;
  unsigned i;
  clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &count);
  for (i = 1; i < count; ++i) {
    strings_push_owned(&result, take_cx_string(clang_getTokenSpelling(tu, tokens[i])));
  }
  if (tokens != NULL) {
    clang_disposeTokens(tu, tokens, count);
  }
  return result;
}

static enum CXChildVisitResult collect_declarations_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  CollectContext *context = (CollectContext *)client_data;
  enum CXCursorKind kind = clang_getCursorKind(cursor);
  Declaration declaration;
  char *file = NULL;
  char *name = NULL;
  char *usr = NULL;
  Declaration *existing;
  unsigned line = 0;
  unsigned column = 0;
  (void)parent;

  memset(&declaration, 0, sizeof(declaration));
  switch (kind) {
    case CXCursor_FunctionDecl:
      declaration.kind = DECL_FUNCTION;
      break;
    case CXCursor_TypedefDecl:
      declaration.kind = DECL_TYPEDEF;
      break;
    case CXCursor_StructDecl:
      declaration.kind = DECL_STRUCT;
      break;
    case CXCursor_UnionDecl:
      declaration.kind = DECL_UNION;
      break;
    case CXCursor_EnumDecl:
      declaration.kind = DECL_ENUM;
      break;
    case CXCursor_MacroDefinition:
      declaration.kind = DECL_MACRO;
      break;
    default:
      return CXChildVisit_Continue;
  }

  if (!get_cursor_location(cursor, &file, &line, &column)) {
    return CXChildVisit_Continue;
  }
  if (!is_owned(context->options, file)) {
    free(file);
    return CXChildVisit_Continue;
  }

  name = take_cx_string(clang_getCursorSpelling(cursor));
  if (declaration.kind == DECL_ENUM) {
    char *annotated_name = flags_name(cursor);
    if (annotated_name != NULL) {
      free(name);
      name = annotated_name;
    } else if (name[0] == '\0') {
      free(name);
      free(file);
      return CXChildVisit_Continue;
    }
  }
  if (name[0] == '\0') {
    free(name);
    free(file);
    return CXChildVisit_Continue;
  }

  usr = take_cx_string(clang_getCursorUSR(cursor));
  existing = find_declaration_by_usr(context, usr);
  if (existing != NULL) {
    if (
        clang_isCursorDefinition(cursor) != 0 &&
        clang_isCursorDefinition(existing->cursor) == 0) {
      existing->cursor = cursor;
    }
    free(usr);
    free(name);
    free(file);
    return CXChildVisit_Continue;
  }

  declaration.cursor = cursor;
  declaration.name = name;
  declaration.file = file;
  declaration.usr = usr;
  declaration.line = line;
  declaration.column = column;
  declaration.file_order = get_file_order(context, file);
  if (declaration.kind == DECL_MACRO) {
    declaration.macro_function_like = clang_Cursor_isMacroFunctionLike(cursor) != 0;
    if (!declaration.macro_function_like) {
      declaration.macro_tokens = macro_tokens(context->tu, cursor);
    }
  }
  declarations_push(&context->declarations, declaration);
  return CXChildVisit_Continue;
}

static int compare_declarations(const void *left_value, const void *right_value) {
  const Declaration *left = (const Declaration *)left_value;
  const Declaration *right = (const Declaration *)right_value;
  if (left->file_order != right->file_order) {
    return left->file_order < right->file_order ? -1 : 1;
  }
  if (left->line != right->line) {
    return left->line < right->line ? -1 : 1;
  }
  if (left->column != right->column) {
    return left->column < right->column ? -1 : 1;
  }
  if (left->kind != right->kind) {
    return left->kind < right->kind ? -1 : 1;
  }
  return strcmp(left->name, right->name);
}

static bool macro_is_integer_candidate(const Declaration *declaration) {
  size_t i;
  if (declaration->macro_function_like || declaration->macro_tokens.count == 0) {
    return false;
  }
  for (i = 0; i < declaration->macro_tokens.count; ++i) {
    const char *token = declaration->macro_tokens.items[i];
    const unsigned char first = (unsigned char)token[0];
    if (first == '"' || first == '\'' || first == '{' || first == '[' || first == ';' ||
        first == '#' || first == '.') {
      return false;
    }
    if (isalpha(first) || first == '_' || isdigit(first)) {
      continue;
    }
    if (strlen(token) == 1 && strchr("()+-~!*/%<>&^|?:,", token[0]) != NULL) {
      continue;
    }
    if (
        strcmp(token, "<<") == 0 || strcmp(token, ">>") == 0 ||
        strcmp(token, "&&") == 0 || strcmp(token, "||") == 0 ||
        strcmp(token, "==") == 0 || strcmp(token, "!=") == 0 ||
        strcmp(token, "<=") == 0 || strcmp(token, ">=") == 0) {
      continue;
    }
    return false;
  }
  return true;
}

typedef struct {
  Declaration **candidates;
  size_t count;
} EvaluationContext;

static enum CXChildVisitResult evaluate_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  EvaluationContext *context = (EvaluationContext *)client_data;
  char *name;
  unsigned long index;
  char *end;
  CXEvalResult result;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_VarDecl) {
    return CXChildVisit_Continue;
  }
  name = take_cx_string(clang_getCursorSpelling(cursor));
  if (strncmp(name, "stela_eval_", 11) != 0) {
    free(name);
    return CXChildVisit_Continue;
  }
  errno = 0;
  index = strtoul(name + 11, &end, 10);
  if (errno != 0 || *end != '\0' || index >= context->count) {
    free(name);
    return CXChildVisit_Continue;
  }
  free(name);
  result = clang_Cursor_Evaluate(cursor);
  if (result == NULL || clang_EvalResult_getKind(result) != CXEval_Int) {
    if (result != NULL) {
      clang_EvalResult_dispose(result);
    }
    return CXChildVisit_Continue;
  }
  context->candidates[index]->has_macro_value = true;
  context->candidates[index]->macro_value_unsigned =
      clang_EvalResult_isUnsignedInt(result) != 0;
  if (context->candidates[index]->macro_value_unsigned) {
    context->candidates[index]->macro_unsigned_value =
        clang_EvalResult_getAsUnsigned(result);
  } else {
    context->candidates[index]->macro_signed_value = clang_EvalResult_getAsLongLong(result);
  }
  clang_EvalResult_dispose(result);
  return CXChildVisit_Continue;
}

static void append_macro_evaluation(
    Buffer *source, const Declaration *declaration, size_t index) {
  buffer_append(source, "static const __typeof__((");
  buffer_append(source, declaration->name);
  buffer_append(source, ")) stela_eval_");
  buffer_append_unsigned(source, index);
  buffer_append(source, " = (");
  buffer_append(source, declaration->name);
  buffer_append(source, ");\n");
}

static bool evaluate_source(
    CXIndex index,
    const Options *options,
    const char *main_name,
    Declaration **candidates,
    size_t count,
    bool combined) {
  Buffer source = make_main_file(options, NULL);
  CXTranslationUnit tu;
  EvaluationContext context;
  size_t i;
  for (i = 0; i < count; ++i) {
    append_macro_evaluation(&source, candidates[i], combined ? i : 0);
  }
  tu = parse_translation_unit(index, options, main_name, &source, false);
  buffer_free(&source);
  if (tu == NULL) {
    return false;
  }
  context.candidates = candidates;
  context.count = count;
  clang_visitChildren(clang_getTranslationUnitCursor(tu), evaluate_visitor, &context);
  clang_disposeTranslationUnit(tu);
  return true;
}

static void evaluate_macros(
    CXIndex index, const Options *options, const char *main_name, Declarations *declarations) {
  Declaration **candidates;
  size_t candidate_count = 0;
  size_t i;
  candidates = (Declaration **)xmalloc(declarations->count * sizeof(Declaration *));
  for (i = 0; i < declarations->count; ++i) {
    if (declarations->items[i].kind == DECL_MACRO &&
        macro_is_integer_candidate(&declarations->items[i])) {
      candidates[candidate_count++] = &declarations->items[i];
    }
  }
  if (candidate_count == 0) {
    free(candidates);
    return;
  }
  if (!evaluate_source(index, options, main_name, candidates, candidate_count, true)) {
    for (i = 0; i < candidate_count; ++i) {
      Declaration *one = candidates[i];
      (void)evaluate_source(index, options, main_name, &one, 1, false);
    }
  }
  free(candidates);
}

static void append_indent(Buffer *buffer, unsigned level) {
  unsigned i;
  for (i = 0; i < level * 2; ++i) {
    buffer_append_char(buffer, ' ');
  }
}

static const char *builtin_name(enum CXTypeKind kind) {
  switch (kind) {
    case CXType_Void:
      return "void";
    case CXType_Bool:
      return "bool";
    case CXType_Char_U:
    case CXType_Char_S:
      return "char";
    case CXType_UChar:
      return "unsigned char";
    case CXType_Char16:
      return "char16_t";
    case CXType_Char32:
      return "char32_t";
    case CXType_UShort:
      return "unsigned short";
    case CXType_UInt:
      return "unsigned int";
    case CXType_ULong:
      return "unsigned long";
    case CXType_ULongLong:
      return "unsigned long long";
    case CXType_SChar:
      return "signed char";
    case CXType_Short:
      return "short";
    case CXType_Int:
      return "int";
    case CXType_Long:
      return "long";
    case CXType_LongLong:
      return "long long";
    case CXType_Float:
      return "float";
    case CXType_Double:
      return "double";
    case CXType_LongDouble:
      return "long double";
    default:
      return NULL;
  }
}

static enum CXChildVisitResult collect_parameters_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  Cursors *parameters = (Cursors *)client_data;
  enum CXCursorKind kind = clang_getCursorKind(cursor);
  (void)parent;
  if (kind == CXCursor_ParmDecl) {
    cursors_push(parameters, cursor);
    return CXChildVisit_Continue;
  }
  if (kind == CXCursor_TypeRef || kind == CXCursor_AnnotateAttr) {
    return CXChildVisit_Continue;
  }
  return CXChildVisit_Recurse;
}

static Cursors collect_parameters(CXCursor cursor) {
  Cursors parameters = {0};
  clang_visitChildren(cursor, collect_parameters_visitor, &parameters);
  return parameters;
}

static void append_annotations(Buffer *buffer, CXCursor cursor) {
  Strings annotations = collect_annotations(cursor);
  size_t i;
  buffer_append_char(buffer, '[');
  for (i = 0; i < annotations.count; ++i) {
    const char *raw = annotations.items[i] + 6;
    const char *equals = strchr(raw, '=');
    if (i != 0) {
      buffer_append(buffer, ", ");
    }
    buffer_append(buffer, "{\"key\": ");
    if (equals == NULL) {
      append_json_string(buffer, raw);
    } else {
      char *key = (char *)xmalloc((size_t)(equals - raw) + 1);
      memcpy(key, raw, (size_t)(equals - raw));
      key[equals - raw] = '\0';
      append_json_string(buffer, key);
      free(key);
      buffer_append(buffer, ", \"value\": ");
      append_json_string(buffer, equals + 1);
    }
    buffer_append_char(buffer, '}');
  }
  buffer_append_char(buffer, ']');
  strings_free(&annotations);
}

static void append_type(Buffer *buffer, CXType type, CXCursor cursor);

typedef struct {
  Buffer *buffer;
  bool first;
} FieldsContext;

typedef struct {
  Buffer *buffer;
  bool first;
  bool unsigned_values;
} EnumeratorsContext;

static enum CXChildVisitResult append_field_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data);
static enum CXChildVisitResult append_enumerator_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data);
static bool enum_type_is_unsigned(CXType type);

/*
 * A struct, union, or enum with neither a tag nor a typedef name has no name
 * C code can spell; Clang's spelling holds its absolute path. Such a type is
 * written inline instead: its fields, or its integer type and enumerators.
 */
static bool append_anonymous_type(Buffer *buffer, CXType type) {
  CXCursor declaration = clang_getTypeDeclaration(type);
  enum CXCursorKind kind = clang_getCursorKind(declaration);
  if (kind != CXCursor_StructDecl && kind != CXCursor_UnionDecl && kind != CXCursor_EnumDecl) {
    return false;
  }
  if (clang_Cursor_isAnonymous(declaration) == 0) {
    return false;
  }
  if (kind == CXCursor_EnumDecl) {
    CXType underlying = clang_getEnumDeclIntegerType(declaration);
    EnumeratorsContext enumerators;
    enumerators.buffer = buffer;
    enumerators.first = true;
    enumerators.unsigned_values = enum_type_is_unsigned(underlying);
    buffer_append(buffer, "{\"enum\": {\"type\": ");
    append_type(buffer, underlying, clang_getNullCursor());
    buffer_append(buffer, ", \"enumerators\": [");
    clang_visitChildren(declaration, append_enumerator_visitor, &enumerators);
  } else {
    FieldsContext fields;
    fields.buffer = buffer;
    fields.first = true;
    buffer_append(
        buffer, kind == CXCursor_StructDecl ? "{\"struct\": {\"fields\": [" : "{\"union\": {\"fields\": [");
    clang_visitChildren(declaration, append_field_visitor, &fields);
  }
  buffer_append(buffer, "]}");
  return true;
}

static void append_parameter(
    Buffer *buffer, CXType type, CXCursor cursor, const char *fallback_name) {
  char *name = NULL;
  buffer_append(buffer, "{\"name\": ");
  if (!clang_Cursor_isNull(cursor)) {
    name = take_cx_string(clang_getCursorSpelling(cursor));
  }
  append_json_string(buffer, name != NULL && name[0] != '\0' ? name : fallback_name);
  free(name);
  buffer_append(buffer, ", \"type\": ");
  append_type(buffer, type, clang_getNullCursor());
  buffer_append(buffer, ", \"annotations\": ");
  if (clang_Cursor_isNull(cursor)) {
    buffer_append(buffer, "[]");
  } else {
    append_annotations(buffer, cursor);
  }
  buffer_append_char(buffer, '}');
}

static void append_function_type(Buffer *buffer, CXType type, CXCursor cursor) {
  int count = clang_getNumArgTypes(type);
  Cursors parameters = {0};
  int i;
  if (!clang_Cursor_isNull(cursor)) {
    parameters = collect_parameters(cursor);
  }
  buffer_append(buffer, "{\"function\": {\"result\": ");
  append_type(buffer, clang_getResultType(type), clang_getNullCursor());
  buffer_append(buffer, ", \"params\": [");
  for (i = 0; i < count; ++i) {
    char fallback[32];
    CXCursor parameter =
        (size_t)i < parameters.count ? parameters.items[i] : clang_getNullCursor();
    if (i != 0) {
      buffer_append(buffer, ", ");
    }
    (void)snprintf(fallback, sizeof(fallback), "arg%d", i);
    append_parameter(buffer, clang_getArgType(type, (unsigned)i), parameter, fallback);
  }
  buffer_append(buffer, "], \"variadic\": ");
  buffer_append(buffer, clang_isFunctionTypeVariadic(type) ? "true" : "false");
  buffer_append(buffer, "}}");
  free(parameters.items);
}

static void append_type(Buffer *buffer, CXType type, CXCursor cursor) {
  const char *builtin = builtin_name(type.kind);
  bool is_const = clang_isConstQualifiedType(type) != 0;
  char *spelling;
  if (type.kind == CXType_Pointer) {
    buffer_append(buffer, "{\"pointer\": ");
    append_type(buffer, clang_getPointeeType(type), cursor);
  } else if (type.kind == CXType_ConstantArray) {
    buffer_append(buffer, "{\"array\": ");
    append_type(buffer, clang_getArrayElementType(type), clang_getNullCursor());
    buffer_append(buffer, ", \"length\": ");
    buffer_append_signed(buffer, clang_getArraySize(type));
  } else if (type.kind == CXType_IncompleteArray) {
    buffer_append(buffer, "{\"array\": ");
    append_type(buffer, clang_getArrayElementType(type), clang_getNullCursor());
  } else if (type.kind == CXType_FunctionProto || type.kind == CXType_FunctionNoProto) {
    append_function_type(buffer, type, cursor);
    return;
  } else if (builtin != NULL) {
    buffer_append(buffer, "{\"builtin\": ");
    append_json_string(buffer, builtin);
  } else if (!append_anonymous_type(buffer, type)) {
    spelling = take_cx_string(clang_getTypeSpelling(clang_getUnqualifiedType(type)));
    if (strncmp(spelling, "struct ", 7) == 0) {
      memmove(spelling, spelling + 7, strlen(spelling + 7) + 1);
    } else if (strncmp(spelling, "union ", 6) == 0) {
      memmove(spelling, spelling + 6, strlen(spelling + 6) + 1);
    } else if (strncmp(spelling, "enum ", 5) == 0) {
      memmove(spelling, spelling + 5, strlen(spelling + 5) + 1);
    }
    buffer_append(buffer, "{\"name\": ");
    append_json_string(buffer, spelling);
    free(spelling);
  }
  if (is_const) {
    buffer_append(buffer, ", \"const\": true");
  }
  buffer_append_char(buffer, '}');
}

static char *normalize_doc_comment(CXCursor cursor) {
  char *raw = take_cx_string(clang_Cursor_getRawCommentText(cursor));
  Buffer output = {0};
  char *line;
  char *next;
  size_t length;
  bool block;
  if (raw[0] == '\0') {
    free(raw);
    return NULL;
  }
  length = strlen(raw);
  while (length > 0 && (raw[length - 1] == '\r' || raw[length - 1] == '\n')) {
    raw[--length] = '\0';
  }
  block = strncmp(raw, "/**", 3) == 0;
  if (block) {
    memmove(raw, raw + 3, strlen(raw + 3) + 1);
    length = strlen(raw);
    if (length >= 2 && raw[length - 2] == '*' && raw[length - 1] == '/') {
      raw[length - 2] = '\0';
    }
  }
  line = raw;
  while (line != NULL) {
    char *start = line;
    char *end;
    next = strchr(line, '\n');
    if (next != NULL) {
      *next++ = '\0';
    }
    if (!block) {
      while (strncmp(start, "///", 3) == 0 || strncmp(start, "//!", 3) == 0) {
        start += 3;
      }
      if (*start == '<') {
        ++start;
      }
    }
    while (*start == ' ' || *start == '\t') {
      ++start;
    }
    if (block && *start == '*') {
      ++start;
      if (*start == ' ') {
        ++start;
      }
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
      --end;
    }
    if (output.length != 0 || end > start) {
      if (output.length != 0) {
        buffer_append_char(&output, '\n');
      }
      buffer_append_n(&output, start, (size_t)(end - start));
    }
    line = next;
  }
  free(raw);
  while (output.length > 0 && output.data[output.length - 1] == '\n') {
    output.data[--output.length] = '\0';
  }
  if (output.length == 0) {
    buffer_free(&output);
    return NULL;
  }
  return output.data;
}

static void append_doc(Buffer *buffer, CXCursor cursor) {
  char *doc = normalize_doc_comment(cursor);
  if (doc == NULL) {
    buffer_append(buffer, "null");
  } else {
    append_json_string(buffer, doc);
    free(doc);
  }
}

static const char *calling_convention_name(enum CXCallingConv convention) {
  switch (convention) {
    case CXCallingConv_Default:
    case CXCallingConv_C:
      return "c";
    case CXCallingConv_X86StdCall:
      return "stdcall";
    case CXCallingConv_X86FastCall:
      return "fastcall";
    case CXCallingConv_X86ThisCall:
      return "thiscall";
    case CXCallingConv_X86Pascal:
      return "pascal";
    case CXCallingConv_AAPCS:
      return "aapcs";
    case CXCallingConv_AAPCS_VFP:
      return "aapcs_vfp";
    case CXCallingConv_X86RegCall:
      return "regcall";
    case CXCallingConv_Win64:
      return "win64";
    case CXCallingConv_X86_64SysV:
      return "sysv64";
    case CXCallingConv_X86VectorCall:
      return "vectorcall";
    case CXCallingConv_AArch64VectorCall:
      return "aarch64_vectorcall";
    case CXCallingConv_Invalid:
      return "invalid";
    default:
      return "unexposed";
  }
}

static CXType effective_function_type(CXType type) {
  type = clang_getCanonicalType(type);
  while (type.kind == CXType_Pointer) {
    type = clang_getPointeeType(type);
  }
  return type;
}

static enum CXChildVisitResult append_field_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  FieldsContext *context = (FieldsContext *)client_data;
  char *name;
  CXType type;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
    return CXChildVisit_Continue;
  }
  if (!context->first) {
    buffer_append(context->buffer, ", ");
  }
  context->first = false;
  name = take_cx_string(clang_getCursorSpelling(cursor));
  type = clang_getCursorType(cursor);
  buffer_append(context->buffer, "{\"name\": ");
  append_json_string(context->buffer, name);
  free(name);
  buffer_append(context->buffer, ", \"type\": ");
  append_type(context->buffer, type, cursor);
  buffer_append(context->buffer, ", \"annotations\": ");
  append_annotations(context->buffer, cursor);
  buffer_append(context->buffer, ", \"doc\": ");
  append_doc(context->buffer, cursor);
  if (clang_Cursor_isBitField(cursor)) {
    buffer_append(context->buffer, ", \"bitWidth\": ");
    buffer_append_signed(context->buffer, clang_getFieldDeclBitWidth(cursor));
  }
  buffer_append_char(context->buffer, '}');
  return CXChildVisit_Continue;
}

typedef struct {
  Buffer *buffer;
  bool first;
} OffsetsContext;

static enum CXChildVisitResult append_offset_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  OffsetsContext *context = (OffsetsContext *)client_data;
  long long bits;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
    return CXChildVisit_Continue;
  }
  if (!context->first) {
    buffer_append(context->buffer, ", ");
  }
  context->first = false;
  bits = clang_Cursor_getOffsetOfField(cursor);
  if (bits < 0) {
    buffer_append_signed(context->buffer, bits);
  } else {
    buffer_append_signed(context->buffer, bits / 8);
  }
  return CXChildVisit_Continue;
}

typedef struct {
  Buffer *buffer;
  bool first;
} FieldCallingConventionsContext;

static enum CXChildVisitResult find_function_field_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  bool *found = (bool *)client_data;
  CXType function_type;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
    return CXChildVisit_Continue;
  }
  function_type = effective_function_type(clang_getCursorType(cursor));
  if (function_type.kind == CXType_FunctionProto ||
      function_type.kind == CXType_FunctionNoProto) {
    *found = true;
    return CXChildVisit_Break;
  }
  return CXChildVisit_Continue;
}

static enum CXChildVisitResult append_field_calling_convention_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  FieldCallingConventionsContext *context =
      (FieldCallingConventionsContext *)client_data;
  CXType function_type;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
    return CXChildVisit_Continue;
  }
  if (!context->first) {
    buffer_append(context->buffer, ", ");
  }
  context->first = false;
  function_type = effective_function_type(clang_getCursorType(cursor));
  if (function_type.kind == CXType_FunctionProto ||
      function_type.kind == CXType_FunctionNoProto) {
    append_json_string(
        context->buffer,
        calling_convention_name(
            clang_getFunctionTypeCallingConv(function_type)));
  } else {
    buffer_append(context->buffer, "null");
  }
  return CXChildVisit_Continue;
}

static enum CXChildVisitResult append_enumerator_visitor(
    CXCursor cursor, CXCursor parent, CXClientData client_data) {
  EnumeratorsContext *context = (EnumeratorsContext *)client_data;
  char *name;
  (void)parent;
  if (clang_getCursorKind(cursor) != CXCursor_EnumConstantDecl) {
    return CXChildVisit_Continue;
  }
  if (!context->first) {
    buffer_append(context->buffer, ", ");
  }
  context->first = false;
  name = take_cx_string(clang_getCursorSpelling(cursor));
  buffer_append(context->buffer, "{\"name\": ");
  append_json_string(context->buffer, name);
  free(name);
  buffer_append(context->buffer, ", \"value\": ");
  if (context->unsigned_values) {
    buffer_append_unsigned(context->buffer, clang_getEnumConstantDeclUnsignedValue(cursor));
  } else {
    buffer_append_signed(context->buffer, clang_getEnumConstantDeclValue(cursor));
  }
  buffer_append(context->buffer, ", \"doc\": ");
  append_doc(context->buffer, cursor);
  buffer_append_char(context->buffer, '}');
  return CXChildVisit_Continue;
}

static bool enum_type_is_unsigned(CXType type) {
  type = clang_getCanonicalType(type);
  return type.kind == CXType_UChar || type.kind == CXType_UShort || type.kind == CXType_UInt ||
      type.kind == CXType_ULong || type.kind == CXType_ULongLong;
}

static void append_common_declaration(
    Buffer *buffer, const Options *options, const Declaration *declaration, const char *kind) {
  char *relative = relative_path(options->root, declaration->file);
  if (relative == NULL) {
    exit(2);
  }
  buffer_append(buffer, "      {\n");
  buffer_append(buffer, "        \"kind\": ");
  append_json_string(buffer, kind);
  buffer_append(buffer, ",\n        \"name\": ");
  append_json_string(buffer, declaration->name);
  buffer_append(buffer, ",\n        \"location\": {\"file\": ");
  append_json_string(buffer, relative);
  buffer_append(buffer, ", \"line\": ");
  buffer_append_unsigned(buffer, declaration->line);
  buffer_append(buffer, ", \"column\": ");
  buffer_append_unsigned(buffer, declaration->column);
  buffer_append(buffer, "},\n        \"doc\": ");
  append_doc(buffer, declaration->cursor);
  buffer_append(buffer, ",\n        \"annotations\": ");
  append_annotations(buffer, declaration->cursor);
  free(relative);
}

static void append_function_declaration(
    Buffer *buffer, const Options *options, const Declaration *declaration) {
  CXType function_type = clang_getCursorType(declaration->cursor);
  int count = clang_Cursor_getNumArguments(declaration->cursor);
  int i;
  append_common_declaration(buffer, options, declaration, "function");
  buffer_append(buffer, ",\n        \"result\": {\"type\": ");
  append_type(buffer, clang_getCursorResultType(declaration->cursor), clang_getNullCursor());
  buffer_append(buffer, "},\n        \"params\": [");
  for (i = 0; i < count; ++i) {
    CXCursor argument = clang_Cursor_getArgument(declaration->cursor, (unsigned)i);
    if (i != 0) {
      buffer_append(buffer, ", ");
    }
    append_parameter(buffer, clang_getCursorType(argument), argument, "");
  }
  buffer_append(buffer, "],\n        \"variadic\": ");
  buffer_append(buffer, clang_Cursor_isVariadic(declaration->cursor) ? "true" : "false");
  buffer_append(buffer, ",\n        \"perTarget\": {\n          ");
  append_json_string(buffer, options->target);
  buffer_append(buffer, ": {\"callingConvention\": ");
  append_json_string(
      buffer, calling_convention_name(clang_getFunctionTypeCallingConv(function_type)));
  buffer_append(buffer, "}\n        }\n      }");
}

static void append_typedef_declaration(
    Buffer *buffer, const Options *options, const Declaration *declaration) {
  CXType type = clang_getTypedefDeclUnderlyingType(declaration->cursor);
  CXType function_type = effective_function_type(type);
  long long size = clang_Type_getSizeOf(type);
  long long align = clang_Type_getAlignOf(type);
  append_common_declaration(buffer, options, declaration, "typedef");
  buffer_append(buffer, ",\n        \"type\": ");
  append_type(buffer, type, declaration->cursor);
  buffer_append(buffer, ",\n        \"perTarget\": {\n          ");
  append_json_string(buffer, options->target);
  buffer_append(buffer, ": {\"canonicalType\": ");
  append_type(buffer, clang_getCanonicalType(type), clang_getNullCursor());
  buffer_append(buffer, ", \"size\": ");
  buffer_append_signed(buffer, size);
  buffer_append(buffer, ", \"align\": ");
  buffer_append_signed(buffer, align);
  if (function_type.kind == CXType_FunctionProto || function_type.kind == CXType_FunctionNoProto) {
    buffer_append(buffer, ", \"callingConvention\": ");
    append_json_string(
        buffer, calling_convention_name(clang_getFunctionTypeCallingConv(function_type)));
  }
  buffer_append(buffer, "}\n        }\n      }");
}

/*
 * "typedef struct { ... } name;" gives the record the typedef's name, but C
 * code can't spell its type "struct name". Clang's USR marks such a record
 * ("c:@SA@name", "c:@UA@name"), however macros spell the declaration.
 */
static bool is_tagless_record(CXCursor cursor) {
  char *usr = take_cx_string(clang_getCursorUSR(cursor));
  bool tagless = strncmp(usr, "c:@SA@", 6) == 0 || strncmp(usr, "c:@UA@", 6) == 0;
  free(usr);
  return tagless;
}

static void append_record_declaration(
    Buffer *buffer,
    const Options *options,
    const Declaration *declaration,
    const char *kind) {
  CXType type = clang_getCursorType(declaration->cursor);
  FieldsContext fields;
  OffsetsContext offsets;
  FieldCallingConventionsContext field_calling_conventions;
  bool has_function_field = false;
  append_common_declaration(buffer, options, declaration, kind);
  if (is_tagless_record(declaration->cursor)) {
    buffer_append(buffer, ",\n        \"tagless\": true");
  }
  buffer_append(buffer, ",\n        \"fields\": [");
  fields.buffer = buffer;
  fields.first = true;
  clang_visitChildren(declaration->cursor, append_field_visitor, &fields);
  buffer_append(buffer, "],\n        \"perTarget\": {\n          ");
  append_json_string(buffer, options->target);
  buffer_append(buffer, ": {\"size\": ");
  buffer_append_signed(buffer, clang_Type_getSizeOf(type));
  buffer_append(buffer, ", \"align\": ");
  buffer_append_signed(buffer, clang_Type_getAlignOf(type));
  buffer_append(buffer, ", \"offsets\": [");
  offsets.buffer = buffer;
  offsets.first = true;
  clang_visitChildren(declaration->cursor, append_offset_visitor, &offsets);
  clang_visitChildren(
      declaration->cursor,
      find_function_field_visitor,
      &has_function_field);
  if (has_function_field) {
    buffer_append(buffer, "], \"fieldCallingConventions\": [");
    field_calling_conventions.buffer = buffer;
    field_calling_conventions.first = true;
    clang_visitChildren(
        declaration->cursor,
        append_field_calling_convention_visitor,
        &field_calling_conventions);
  }
  buffer_append(buffer, "]}\n        }\n      }");
}

static void append_enum_declaration(
    Buffer *buffer, const Options *options, const Declaration *declaration) {
  CXType underlying = clang_getEnumDeclIntegerType(declaration->cursor);
  EnumeratorsContext enumerators;
  append_common_declaration(buffer, options, declaration, "enum");
  buffer_append(buffer, ",\n        \"type\": ");
  append_type(buffer, underlying, clang_getNullCursor());
  buffer_append(buffer, ",\n        \"enumerators\": [");
  enumerators.buffer = buffer;
  enumerators.first = true;
  enumerators.unsigned_values = enum_type_is_unsigned(underlying);
  clang_visitChildren(declaration->cursor, append_enumerator_visitor, &enumerators);
  buffer_append(buffer, "],\n        \"perTarget\": {\n          ");
  append_json_string(buffer, options->target);
  buffer_append(buffer, ": {\"canonicalType\": ");
  append_type(buffer, clang_getCanonicalType(underlying), clang_getNullCursor());
  buffer_append(buffer, "}\n        }\n      }");
}

static void append_macro_declaration(
    Buffer *buffer, const Options *options, const Declaration *declaration) {
  size_t i;
  append_common_declaration(buffer, options, declaration, "macro");
  buffer_append(buffer, ",\n        \"functionLike\": ");
  buffer_append(buffer, declaration->macro_function_like ? "true" : "false");
  if (!declaration->macro_function_like) {
    buffer_append(buffer, ",\n        \"replacement\": [");
    for (i = 0; i < declaration->macro_tokens.count; ++i) {
      if (i != 0) {
        buffer_append(buffer, ", ");
      }
      append_json_string(buffer, declaration->macro_tokens.items[i]);
    }
    buffer_append_char(buffer, ']');
  }
  if (declaration->has_macro_value) {
    buffer_append(buffer, ",\n        \"perTarget\": {\n          ");
    append_json_string(buffer, options->target);
    buffer_append(buffer, ": {\"value\": ");
    {
      Buffer value = {0};
      if (declaration->macro_value_unsigned) {
        buffer_append_unsigned(&value, declaration->macro_unsigned_value);
      } else {
        buffer_append_signed(&value, declaration->macro_signed_value);
      }
      append_json_string(buffer, value.data);
      buffer_free(&value);
    }
    buffer_append(buffer, "}\n        }");
  }
  buffer_append(buffer, "\n      }");
}

static Buffer serialize_output(
    const Options *options, const char *libclang_version, const Declarations *declarations) {
  Buffer output = {0};
  size_t i;
  buffer_append(&output, "{\n  \"schema\": \"stela.raw/1\",\n  \"producer\": {\"stela\": \"");
  buffer_append(&output, STELA_VERSION);
  buffer_append(&output, "\", \"libclang\": ");
  append_json_string(&output, libclang_version);
  buffer_append(&output, "},\n  \"target\": ");
  append_json_string(&output, options->target);
  buffer_append(&output, ",\n  \"headers\": [");
  for (i = 0; i < options->headers.count; ++i) {
    char *relative = relative_path(options->root, options->headers.items[i]);
    if (relative == NULL) {
      exit(2);
    }
    if (i != 0) {
      buffer_append(&output, ", ");
    }
    append_json_string(&output, relative);
    free(relative);
  }
  buffer_append(&output, "],\n  \"declarations\": [\n");
  for (i = 0; i < declarations->count; ++i) {
    const Declaration *declaration = &declarations->items[i];
    if (i != 0) {
      buffer_append(&output, ",\n");
    }
    switch (declaration->kind) {
      case DECL_FUNCTION:
        append_function_declaration(&output, options, declaration);
        break;
      case DECL_TYPEDEF:
        append_typedef_declaration(&output, options, declaration);
        break;
      case DECL_STRUCT:
        append_record_declaration(&output, options, declaration, "struct");
        break;
      case DECL_UNION:
        append_record_declaration(&output, options, declaration, "union");
        break;
      case DECL_ENUM:
        append_enum_declaration(&output, options, declaration);
        break;
      case DECL_MACRO:
        append_macro_declaration(&output, options, declaration);
        break;
    }
  }
  buffer_append(&output, "\n  ]\n}\n");
  return output;
}

static bool write_output(const char *path, const Buffer *output) {
  Buffer temporary = {0};
  FILE *file;
  size_t written;
  buffer_append(&temporary, path);
#ifdef _WIN32
  buffer_append(&temporary, ".tmp.");
  buffer_append_unsigned(&temporary, GetCurrentProcessId());
#else
  buffer_append(&temporary, ".tmp.");
  buffer_append_unsigned(&temporary, (unsigned long long)getpid());
#endif
  file = fopen(temporary.data, "wb");
  if (file == NULL) {
    fprintf(stderr, "stela-extract: cannot open %s: %s\n", temporary.data, strerror(errno));
    buffer_free(&temporary);
    return false;
  }
  written = fwrite(output->data, 1, output->length, file);
  if (written != output->length || fclose(file) != 0) {
    fprintf(stderr, "stela-extract: cannot write %s: %s\n", temporary.data, strerror(errno));
    (void)remove(temporary.data);
    buffer_free(&temporary);
    return false;
  }
#ifdef _WIN32
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
    if (!MoveFileExA(temporary.data, path, MOVEFILE_WRITE_THROUGH)) {
      fprintf(
          stderr,
          "stela-extract: cannot move %s to %s: Windows error %lu\n",
          temporary.data,
          path,
          GetLastError());
      (void)remove(temporary.data);
      buffer_free(&temporary);
      return false;
    }
  } else if (!ReplaceFileA(
                 path,
                 temporary.data,
                 NULL,
                 REPLACEFILE_WRITE_THROUGH,
                 NULL,
                 NULL)) {
    fprintf(
        stderr,
        "stela-extract: cannot replace %s: Windows error %lu\n",
        path,
        GetLastError());
    (void)remove(temporary.data);
    buffer_free(&temporary);
    return false;
  }
#else
  if (rename(temporary.data, path) != 0) {
    fprintf(stderr, "stela-extract: cannot replace %s: %s\n", path, strerror(errno));
    (void)remove(temporary.data);
    buffer_free(&temporary);
    return false;
  }
#endif
  buffer_free(&temporary);
  return true;
}

static bool check_libclang_version(char **version) {
  char *cursor;
  long major;
  char *end;
  *version = take_cx_string(clang_getClangVersion());
  cursor = strstr(*version, "version ");
  if (cursor == NULL) {
    fprintf(stderr, "stela-extract: cannot parse libclang version: %s\n", *version);
    return false;
  }
  cursor += 8;
  errno = 0;
  major = strtol(cursor, &end, 10);
  if (errno != 0 || end == cursor) {
    fprintf(stderr, "stela-extract: cannot parse libclang version: %s\n", *version);
    return false;
  }
  if (major < MIN_LIBCLANG_MAJOR) {
    fprintf(
        stderr,
        "stela-extract: libclang %ld is too old; version %d or newer is required\n",
        major,
        MIN_LIBCLANG_MAJOR);
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  Options options = {0};
  CXIndex index = NULL;
  CXTranslationUnit tu = NULL;
  Buffer source = {0};
  Buffer output = {0};
  Buffer main_name = {0};
  CollectContext context = {0};
  char *libclang_version = NULL;
  int result = 2;

  if (!parse_options(argc, argv, &options)) {
    goto cleanup;
  }
  if (!check_libclang_version(&libclang_version)) {
    goto cleanup;
  }

  index = clang_createIndex(0, 0);
  if (index == NULL) {
    fputs("stela-extract: cannot create libclang index\n", stderr);
    goto cleanup;
  }
  source = make_main_file(&options, NULL);
  buffer_append(&main_name, options.root);
  buffer_append(&main_name, "/.stela_extract_input.c");
  tu = parse_translation_unit(index, &options, main_name.data, &source, true);
  if (tu == NULL) {
    goto cleanup;
  }

  context.options = &options;
  context.tu = tu;
  clang_visitChildren(
      clang_getTranslationUnitCursor(tu), collect_declarations_visitor, &context);
  if (context.failed) {
    goto cleanup;
  }
  qsort(
      context.declarations.items,
      context.declarations.count,
      sizeof(Declaration),
      compare_declarations);
  evaluate_macros(index, &options, main_name.data, &context.declarations);
  output = serialize_output(&options, libclang_version, &context.declarations);
  if (!write_output(options.out, &output)) {
    goto cleanup;
  }
  result = 0;

cleanup:
  free(libclang_version);
  strings_free(&context.files);
  declarations_free(&context.declarations);
  buffer_free(&output);
  if (tu != NULL) {
    clang_disposeTranslationUnit(tu);
  }
  if (index != NULL) {
    clang_disposeIndex(index);
  }
  buffer_free(&main_name);
  buffer_free(&source);
  options_free(&options);
  return result;
}
