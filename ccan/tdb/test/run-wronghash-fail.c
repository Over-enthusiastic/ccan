#define _XOPEN_SOURCE 500
#include <ccan/tdb/tdb.h>
#include <ccan/tdb/io.c>
#include <ccan/tdb/tdb.c>
#include <ccan/tdb/lock.c>
#include <ccan/tdb/freelist.c>
#include <ccan/tdb/traverse.c>
#include <ccan/tdb/transaction.c>
#include <ccan/tdb/error.c>
#include <ccan/tdb/open.c>
#include <ccan/tdb/check.c>
#include <ccan/hash/hash.h>
#include <ccan/tap/tap.h>
#include <stdlib.h>
#include <err.h>

static unsigned int jenkins_hash(TDB_DATA *key)
{
	return hash_stable(key->dptr, key->dsize, 0);
}

static void log_fn(struct tdb_context *tdb, enum tdb_debug_level level, const char *fmt, ...)
{
	unsigned int *count = tdb_get_logging_private(tdb);
	/* Old code used to complain about spinlocks when we put something
	   here. */
	if (strstr(fmt, "wrong hash") || strstr(fmt, "spinlock"))
		(*count)++;
}

int main(int argc, char *argv[])
{
	struct tdb_context *tdb;
	unsigned int log_count;
	struct tdb_logging_context log_ctx = { log_fn, &log_count };

	plan_tests(16);

	/* Create with default hash. */
	log_count = 0;
	tdb = tdb_open_ex("run-wronghash-fail.tdb", 0, 0,
			  O_CREAT|O_RDWR|O_TRUNC, 0600, &log_ctx, NULL);
	ok1(tdb);
	ok1(log_count == 0);
	tdb_close(tdb);

	/* Fail to open with different hash. */
	tdb = tdb_open_ex("run-wronghash-fail.tdb", 0, 0, O_RDWR, 0,
			  &log_ctx, jenkins_hash);
	ok1(!tdb);
	ok1(log_count == 1);

	/* Create with different hash. */
	log_count = 0;
	tdb = tdb_open_ex("run-wronghash-fail.tdb", 0, 0,
			  O_CREAT|O_RDWR|O_TRUNC,
			  0600, &log_ctx, jenkins_hash);
	ok1(tdb);
	ok1(log_count == 0);
	tdb_close(tdb);

	/* Endian should be no problem. */
	log_count = 0;
	tdb = tdb_open_ex("test/jenkins-le-hash.tdb", 0, 0, O_RDWR, 0,
			  &log_ctx, NULL);
	ok1(!tdb);
	ok1(log_count == 1);

	log_count = 0;
	tdb = tdb_open_ex("test/jenkins-be-hash.tdb", 0, 0, O_RDWR, 0,
			  &log_ctx, NULL);
	ok1(!tdb);
	ok1(log_count == 1);

	log_count = 0;
	/* Fail to open with defailt hash. */
	tdb = tdb_open_ex("run-wronghash-fail.tdb", 0, 0, O_RDWR, 0,
			  &log_ctx, NULL);
	ok1(!tdb);
	ok1(log_count == 1);

	log_count = 0;
	tdb = tdb_open_ex("test/jenkins-le-hash.tdb", 0, 0, O_RDONLY,
			  0, &log_ctx, jenkins_hash);
	ok1(tdb);
	ok1(log_count == 0);
	tdb_close(tdb);

	log_count = 0;
	tdb = tdb_open_ex("test/jenkins-be-hash.tdb", 0, 0, O_RDONLY,
			  0, &log_ctx, jenkins_hash);
	ok1(tdb);
	ok1(log_count == 0);
	tdb_close(tdb);

	return exit_status();
}
