
#ifndef lib60870_shared_EXPORT_H
#define lib60870_shared_EXPORT_H

#ifdef lib60870_shared_BUILT_AS_STATIC
#  define lib60870_shared_EXPORT
#  define LIB60870_SHARED_NO_EXPORT
#else
#  ifndef lib60870_shared_EXPORT
#    ifdef lib60870_shared_EXPORTS
        /* We are building this library */
#      define lib60870_shared_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define lib60870_shared_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef LIB60870_SHARED_NO_EXPORT
#    define LIB60870_SHARED_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef LIB60870_SHARED_DEPRECATED
#  define LIB60870_SHARED_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef LIB60870_SHARED_DEPRECATED_EXPORT
#  define LIB60870_SHARED_DEPRECATED_EXPORT lib60870_shared_EXPORT LIB60870_SHARED_DEPRECATED
#endif

#ifndef LIB60870_SHARED_DEPRECATED_NO_EXPORT
#  define LIB60870_SHARED_DEPRECATED_NO_EXPORT LIB60870_SHARED_NO_EXPORT LIB60870_SHARED_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef LIB60870_SHARED_NO_DEPRECATED
#    define LIB60870_SHARED_NO_DEPRECATED
#  endif
#endif

#endif /* lib60870_shared_EXPORT_H */
