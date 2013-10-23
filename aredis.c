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

#include "php_aredis.h"

/* For stuff used in PHP_MINFO_FUNCTION */
#include "ext/standard/info.h"

/* For smart strings */
#include "ext/standard/php_smart_str.h"
#include <pthread.h>

/* Could probably use socketpair(2) on other platforms than linux */
#include <sys/eventfd.h>

#include "liblfds/liblfds611.h"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>

#undef NDEBUG
#include <assert.h>

typedef struct _php_aredis_object  {
    zend_object zo;

    pthread_t io_thread;
    int efd;

    struct lfds611_queue_state *io_in_qs;
    struct lfds611_queue_state *io_out_qs;

} php_aredis_object;

static
	zend_class_entry *php_aredis_sc_entry;

static
	zend_object_handlers aredis_object_handlers;

/* io thread structure */
typedef struct _php_aredis_io_t {
    struct lfds611_queue_state *io_in_qs;
    struct lfds611_queue_state *io_out_qs;

    redisAsyncContext *c;

    /* When efd is signaled readable then io_in_qs is readable */
    int efd;

} php_aredis_io_t;

typedef enum _php_aredis_cmd_type_t {
    AREDIS_CMD_SET,
    AREDIS_CMD_GET,
    AREDIS_CMD_QUIT,
} php_aredis_cmd_type_t;

typedef struct _php_aredis_cmd_t {
    int type;
    char *key;
    char *value;
} php_aredis_cmd_t;

typedef struct _php_aredis_response_t {
    char *key;
    char *value;
} php_aredis_response_t;

typedef struct _php_aredis_get_args_t {
    char *key;
    php_aredis_io_t *io;
} php_aredis_get_args_t;

void s_process_get_results(redisAsyncContext *c, void *r, void *args)
{
    redisReply *reply = (redisReply *) r;
    if (!reply)
        return;

    /* Push the GET response to outbound queue */
    php_aredis_get_args_t *get_args = (php_aredis_get_args_t *) args;

    php_aredis_response_t *response = malloc (sizeof (php_aredis_response_t));
    assert (response);

    /* Get the key */
    assert (get_args->key);

    response->key = strdup (get_args->key);
    response->value = strndup (reply->str, reply->len);
    assert (response->value);

    int ret = lfds611_queue_enqueue (get_args->io->io_out_qs, response);
    assert (ret == 1);

    free (get_args->key);
    free (get_args);
}

typedef struct _php_aredis_event_in_t {
    php_aredis_io_t *io;
    struct event_base *base;
} php_aredis_event_in_t;

static void
s_event_in(evutil_socket_t fd, short event, void *args)
{
    assert (event & EV_READ);
    php_aredis_event_in_t *event_in = (php_aredis_event_in_t *) args;
    php_aredis_cmd_t *cmd = NULL;

    eventfd_t val;
    int ret = eventfd_read (fd, &val);

    if (ret == 0) {
        while ((ret = lfds611_queue_dequeue(event_in->io->io_in_qs, (void **)&cmd)) == 1) {

            switch (cmd->type) {
                case AREDIS_CMD_SET:
                    redisAsyncCommand(event_in->io->c, NULL, NULL, "SET %b %b", cmd->key, strlen (cmd->key), cmd->value, strlen (cmd->value));
                break;

                case AREDIS_CMD_GET:
                {
                    /* Callback frees this */
                    php_aredis_get_args_t *get_args = malloc (sizeof (php_aredis_get_args_t));
                    assert (get_args);

                    get_args->key = strdup (cmd->key);
                    get_args->io  = event_in->io;
                    assert (get_args->key);

                    redisAsyncCommand(event_in->io->c, s_process_get_results, get_args, "GET %b", cmd->key, strlen (cmd->key));
                }
                break;

                case AREDIS_CMD_QUIT:
                {
                    int rc = event_base_loopbreak (event_in->base);
                    assert (rc == 0);
                }
                break;
            }

            if (cmd->key)
                free (cmd->key);

            if (cmd->value)
                free (cmd->value);

            free (cmd);
        }
    }
}

static
void *s_hiredis_io_thread (void *args)
{
    php_aredis_io_t *io = (php_aredis_io_t *) args;

    struct event_base *base = event_base_new();
    io->c = redisAsyncConnect("127.0.0.1", 6379);

    if (io->c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", io->c->errstr);
        return NULL; /* Not handled */
    }

    /* Use the queues in this thread */
    lfds611_queue_use (io->io_in_qs);
    lfds611_queue_use (io->io_out_qs);

    redisLibeventAttach(io->c,base);

    php_aredis_event_in_t event_in = { io, base };

    /* Attach our eventfd so that parent thread can signal for events */
    struct event *efdevent = event_new(base, io->efd, EV_READ|EV_PERSIST, s_event_in, &event_in);
    event_add(efdevent, NULL);
    event_base_dispatch(base);
}

PHP_METHOD(aredis, __construct)
{
    struct lfds611_queue_state *io_in_qs, *io_out_qs;

    int rc, efd;
    php_aredis_object *intern;
    long queue_size = 1000;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &queue_size) == FAILURE) {
        return;
    }

    intern = (php_aredis_object *) zend_object_store_get_object(getThis() TSRMLS_CC);

    /* We need to create two queues, one from main to io and one from io main */
    rc = lfds611_queue_new (&io_in_qs, queue_size);
    assert (rc == 1);

    rc = lfds611_queue_new (&io_out_qs, queue_size);
    assert (rc == 1);

    /* Use the queue in this thread */
    lfds611_queue_use (io_in_qs);
    lfds611_queue_use (io_out_qs);

    /* We use eventfd to signal between threads */
    efd = eventfd (0, EFD_NONBLOCK);

    /* Parent needs these */
    intern->io_in_qs  = io_in_qs;
    intern->io_out_qs = io_out_qs;
    intern->efd       = efd;

    /* Initialise needed structures for the io-thread */
    php_aredis_io_t *io = emalloc (sizeof (php_aredis_io_t));

    /* Initialise queue with maximum of 1000 elements */
    io->io_in_qs  = io_in_qs;
    io->io_out_qs = io_out_qs;

    /* Eventfd for signaling */
    io->efd = efd;

    /* start io thread for the object */
    rc = pthread_create (&intern->io_thread, NULL, &s_hiredis_io_thread, (void *) io);
    assert (rc == 0);

    /* TODO: need to wait for thread to signal ready */
}

PHP_METHOD(aredis, set)
{
    char *key, *value;
    int key_len, value_len;
    php_aredis_object *intern;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &key, &key_len, &value, &value_len) == FAILURE) {
        return;
    }

    intern = (php_aredis_object *) zend_object_store_get_object(getThis() TSRMLS_CC);

    /* Create new command. Who ever dequeues this is responsible for freeing*/
    php_aredis_cmd_t *cmd = malloc (sizeof (php_aredis_cmd_t));
    assert (cmd);

    cmd->type  = AREDIS_CMD_SET;
    cmd->key   = strdup (key);
    cmd->value = strdup (value);
    assert (cmd->key && cmd->value);

    /* Enqueue the command */
    int ret = lfds611_queue_enqueue (intern->io_in_qs, cmd);
    assert (ret == 1);

    /* Signal the eventfd that there is data in the queue */
    ret = eventfd_write (intern->efd, 1);
    assert (ret == 0);
}


PHP_METHOD(aredis, get)
{
    char *key;
    int key_len;
    php_aredis_object *intern;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &key, &key_len) == FAILURE) {
        return;
    }

    intern = (php_aredis_object *) zend_object_store_get_object(getThis() TSRMLS_CC);

    /* Create new command. Who ever dequeues this is responsible for freeing*/
    php_aredis_cmd_t *cmd = malloc (sizeof (php_aredis_cmd_t));
    assert (cmd);

    cmd->type  = AREDIS_CMD_GET;
    cmd->key   = strdup (key);
    cmd->value = NULL;
    assert (cmd->key);

    /* Enqueue the command */
    int ret = lfds611_queue_enqueue (intern->io_in_qs, cmd);
    assert (ret == 1);

    /* Signal the eventfd that there is data in the queue */
    ret = eventfd_write (intern->efd, 1);
    assert (ret == 0);
}

PHP_METHOD(aredis, process_events)
{
    php_aredis_object *intern;
    php_aredis_response_t *response = NULL;

    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }

    intern = (php_aredis_object *) zend_object_store_get_object(getThis() TSRMLS_CC);

    int ret = lfds611_queue_dequeue (intern->io_out_qs, &response);
    if (!response) {
        RETURN_NULL();
    }
    array_init (return_value);
    add_assoc_string (return_value, response->key, response->value, 1);

    free (response->key);
    free (response->value);
    free (response);
}

static
zend_function_entry php_aredis_class_methods[] =
{
	PHP_ME(aredis, __construct, NULL,   ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(aredis, set, NULL,   ZEND_ACC_PUBLIC)
	PHP_ME(aredis, get, NULL,   ZEND_ACC_PUBLIC)
	PHP_ME(aredis, process_events, NULL,   ZEND_ACC_PUBLIC)
	{ NULL, NULL, NULL }
};

static
void php_aredis_object_free_storage(void *object TSRMLS_DC)
{
	php_aredis_object *intern = (php_aredis_object *)object;

	if (!intern) {
		return;
	}

    /* Create new command. Who ever dequeues this is responsible for freeing*/
    php_aredis_cmd_t *cmd = malloc (sizeof (php_aredis_cmd_t));
    assert (cmd);

    cmd->type  = AREDIS_CMD_QUIT;
    cmd->key   = NULL;
    cmd->value = NULL;

    /* Enqueue the command */
    int ret = lfds611_queue_enqueue (intern->io_in_qs, cmd);
    assert (ret == 1);

    /* Signal the eventfd that there is data in the queue */
    ret = eventfd_write (intern->efd, 1);
    assert (ret == 0);

    // Join the thread, we are done
    pthread_join (intern->io_thread, NULL);

    /*
        TODO: queue destructors
    */
    lfds611_queue_delete (intern->io_in_qs, NULL, NULL);
    lfds611_queue_delete (intern->io_out_qs, NULL, NULL);

	zend_object_std_dtor(&intern->zo TSRMLS_CC);
	efree(intern);
}

#if PHP_VERSION_ID < 50399 && !defined(object_properties_init)
# define object_properties_init(zo, class_type) { \
			zval *tmp; \
			zend_hash_copy((*zo).properties, \
							&class_type->default_properties, \
							(copy_ctor_func_t) zval_add_ref, \
							(void *) &tmp, \
							sizeof(zval *)); \
		 }
#endif

static
zend_object_value php_aredis_object_new(zend_class_entry *class_type TSRMLS_DC)
{
	zend_object_value retval;
	php_aredis_object *intern = emalloc (sizeof (php_aredis_object));

	memset(&intern->zo, 0, sizeof(zend_object));
	zend_object_std_init(&intern->zo, class_type TSRMLS_CC);

	object_properties_init(&intern->zo, class_type);

	retval.handle   = zend_objects_store_put(intern, NULL, (zend_objects_free_object_storage_t) php_aredis_object_free_storage, NULL TSRMLS_CC);
	retval.handlers = &aredis_object_handlers;

	return retval;
}


PHP_MINIT_FUNCTION(aredis)
{
	zend_class_entry ce;

	memcpy (&aredis_object_handlers, zend_get_std_object_handlers (), sizeof (zend_object_handlers));
	INIT_CLASS_ENTRY(ce, "aredis", php_aredis_class_methods);

	ce.create_object = php_aredis_object_new;
	aredis_object_handlers.clone_obj = NULL;

	php_aredis_sc_entry = zend_register_internal_class(&ce TSRMLS_CC);
	return SUCCESS;
}


PHP_MINFO_FUNCTION(aredis)
{
	php_info_print_table_start();

		php_info_print_table_header(2, "aredis extension", "enabled");
		php_info_print_table_row(2, "aredis extension version", PHP_AREDIS_EXTVER);
	php_info_print_table_end();
}

zend_function_entry aredis_functions[] = {
	{NULL, NULL, NULL}
};

zend_module_entry aredis_module_entry =
{
	STANDARD_MODULE_HEADER,
	"aredis"					/* Name of the extension */,
	aredis_functions,		    /* This is where you would functions that are not part of classes */
	PHP_MINIT(aredis),		    /* Executed once during module initialisation, not per request */
	NULL,                   	/* Executed once during module shutdown, not per request */
	NULL,		                /* Executed every request, before script is executed */
	NULL,	                    /* Executed every request, after script has executed */
	PHP_MINFO(aredis),		    /* Hook for displaying info in phpinfo() */
	PHP_AREDIS_EXTVER,	    	/* Extension version, defined in php_aredis.h */
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_AREDIS
ZEND_GET_MODULE(aredis)
#endif /* COMPILE_DL_AREDIS */
