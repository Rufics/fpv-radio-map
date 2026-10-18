/*
 * rfmod C ABI, version 2.
 *
 * The only binary boundary between the rfmap host and a module. Everything that crosses it
 * is a NUL-terminated UTF-8 JSON string, so modules can be built with any compiler or
 * standard library. The ABI version changes only when this struct layout changes; new
 * features are new JSON methods, which keeps old modules loading.
 *
 * Version 2 replaced the untyped host context pointer with the opaque rfmod_host_context.
 */
#ifndef RFMOD_ABI_H
#define RFMOD_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A typed constant in C++; in C an enum, the only non-macro integer constant expression, so
 * it still works in file-scope initializers, case labels and _Static_assert. */
#ifdef __cplusplus
inline constexpr uint32_t RFMOD_ABI_VERSION = 2;
#else
enum rfmod_abi { RFMOD_ABI_VERSION = 2 };
#endif

#if defined(_WIN32)
#define RFMOD_EXPORT __declspec(dllexport)
#else
#define RFMOD_EXPORT __attribute__((visibility("default")))
#endif

enum rfmod_status {
    RFMOD_OK = 0,
    RFMOD_ERR_UNKNOWN_METHOD = 1,
    RFMOD_ERR_BAD_REQUEST = 2,
    RFMOD_ERR_INTERNAL = 3,
    RFMOD_ERR_HOST_CALL = 4
};

enum rfmod_log_level { RFMOD_LOG_DEBUG = 0, RFMOD_LOG_INFO = 1, RFMOD_LOG_WARN = 2, RFMOD_LOG_ERROR = 3 };

/* Host state handed back to every host callback. Only the host knows its layout; a module
 * passes the pointer through unchanged and never dereferences it. */
typedef struct rfmod_host_context rfmod_host_context;

/* Services the host offers to a module. */
typedef struct rfmod_host {
    uint32_t abi_version;
    rfmod_host_context* context;
    /* Call a host method (for example "link.evaluate"). On success *response_json points to a
     * host-owned string valid until the next call on this thread. */
    int (*call)(rfmod_host_context* context, const char* method, const char* request_json, const char** response_json);
    void (*log)(rfmod_host_context* context, int level, const char* message);
} rfmod_host;

/* What a module offers to the host. */
typedef struct rfmod_module {
    uint32_t abi_version;
    /* The module's manifest (same content as manifest.json in the package). */
    const char* (*manifest_json)(void);
    int (*init)(const rfmod_host* host, const char* options_json);
    /* Handle a method. On success *response_json is allocated by the module and released
     * with free_string. On error it may hold a JSON error object, also released with free_string. */
    int (*invoke)(const char* method, const char* request_json, char** response_json);
    void (*free_string)(const char* s);
    void (*shutdown)(void);
} rfmod_module;

/* The single exported symbol, and the name to look it up by. */
typedef const rfmod_module* (*rfmod_get_module_fn)(void);
static const char RFMOD_ENTRY_SYMBOL[] = "rfmod_get_module";

#ifdef __cplusplus
}
#endif

#endif /* RFMOD_ABI_H */
