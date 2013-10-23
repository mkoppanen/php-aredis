PHP_ARG_WITH(aredis, whether to enable aredis,
[  --with-aredis               Enable aredis])

if test "$PHP_AREDIS" != "no"; then

  PHP_CHECK_LIBRARY(event, event_base_new,
  [
      PHP_ADD_LIBRARY(event, 1, AREDIS_SHARED_LIBADD)
  ],
  [AC_MSG_ERROR(libevent not found)])

  PHP_ADD_LIBRARY(hiredis, 1, AREDIS_SHARED_LIBADD)
  
  PHP_ADD_LIBRARY_WITH_PATH(lfds611, [liblfds/], AREDIS_SHARED_LIBADD)
  PHP_SUBST(AREDIS_SHARED_LIBADD)

  PHP_NEW_EXTENSION(aredis, aredis.c, $ext_shared)
  
fi

