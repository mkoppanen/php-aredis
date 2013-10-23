/*
  +----------------------------------------------------------------------+
  | Copyright (c) 2013 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Mikko Koppanen <mkoppanen@php.net>                          |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_AREDIS_H
# define PHP_AREDIS_H

/*
  This is the main header file for the extension. It's good to keep
  this file as light as possible and if you need additional definitions
  do them in a separate header which is included from the implementation file (.c)
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"

/*
  Define the extension version
*/
#define PHP_AREDIS_EXTVER "0.0.1"

/*
  Define an entry-point to the extension
*/
extern zend_module_entry aredis_module_entry;
#define phpext_aredis_ptr &aredis_module_entry

#endif /* PHP_AREDIS_H */