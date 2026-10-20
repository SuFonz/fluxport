AC_PREREQ([2.69])
AC_INIT([fluxport], [1.0.0], [fluxport@example.invalid])
AC_CONFIG_SRCDIR([src/common/protocol.h])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])
AC_CANONICAL_HOST
AC_PROG_CC
AC_PROG_INSTALL
AC_USE_SYSTEM_EXTENSIONS

case "$host_os" in
    mingw*|msys*)
        LIBS="$LIBS -lws2_32"
        ;;
esac

AC_ARG_ENABLE([sanitizers],
    [AS_HELP_STRING([--enable-sanitizers],
                    [build with AddressSanitizer and UBSan (POSIX only)])],
    [],
    [enable_sanitizers=no])

AS_IF([test "x$enable_sanitizers" = "xyes"], [
    case "$host_os" in
        mingw*|msys*|cygwin*)
            AC_MSG_ERROR([--enable-sanitizers is not supported on $host_os])
            ;;
    esac
    AC_MSG_CHECKING([whether the compiler accepts sanitizer flags])
    saved_CFLAGS="$CFLAGS"
    CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
        [AC_MSG_RESULT([yes])],
        [AC_MSG_RESULT([no])
         AC_MSG_ERROR([the compiler does not support -fsanitize=address,undefined])])
    CFLAGS="$saved_CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
    LDFLAGS="$LDFLAGS -fsanitize=address,undefined"
])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
