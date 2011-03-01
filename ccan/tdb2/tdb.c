#include "private.h"
#include <ccan/tdb2/tdb2.h>
#include <assert.h>
#include <stdarg.h>

/* The null return. */
struct tdb_data tdb_null = { .dptr = NULL, .dsize = 0 };

/* all contexts, to ensure no double-opens (fcntl locks don't nest!) */
static struct tdb_context *tdbs = NULL;

static bool tdb_already_open(dev_t device, ino_t ino)
{
	struct tdb_context *i;
	
	for (i = tdbs; i; i = i->next) {
		if (i->device == device && i->inode == ino) {
			return true;
		}
	}

	return false;
}

static bool read_all(int fd, void *buf, size_t len)
{
	while (len) {
		ssize_t ret;
		ret = read(fd, buf, len);
		if (ret < 0)
			return false;
		if (ret == 0) {
			/* ETOOSHORT? */
			errno = EWOULDBLOCK;
			return false;
		}
		buf = (char *)buf + ret;
		len -= ret;
	}
	return true;
}

static uint64_t random_number(struct tdb_context *tdb)
{
	int fd;
	uint64_t ret = 0;
	struct timeval now;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		if (read_all(fd, &ret, sizeof(ret))) {
			close(fd);
			return ret;
		}
		close(fd);
	}
	/* FIXME: Untested!  Based on Wikipedia protocol description! */
	fd = open("/dev/egd-pool", O_RDWR);
	if (fd >= 0) {
		/* Command is 1, next byte is size we want to read. */
		char cmd[2] = { 1, sizeof(uint64_t) };
		if (write(fd, cmd, sizeof(cmd)) == sizeof(cmd)) {
			char reply[1 + sizeof(uint64_t)];
			int r = read(fd, reply, sizeof(reply));
			if (r > 1) {
				/* Copy at least some bytes. */
				memcpy(&ret, reply+1, r - 1);
				if (reply[0] == sizeof(uint64_t)
				    && r == sizeof(reply)) {
					close(fd);
					return ret;
				}
			}
		}
		close(fd);
	}

	/* Fallback: pid and time. */
	gettimeofday(&now, NULL);
	ret = getpid() * 100132289ULL + now.tv_sec * 1000000ULL + now.tv_usec;
	tdb_logerr(tdb, TDB_SUCCESS, TDB_LOG_WARNING,
		   "tdb_open: random from getpid and time");
	return ret;
}

struct new_database {
	struct tdb_header hdr;
	struct tdb_freetable ftable;
};

/* initialise a new database */
static int tdb_new_database(struct tdb_context *tdb,
			    struct tdb_attribute_seed *seed,
			    struct tdb_header *hdr)
{
	/* We make it up in memory, then write it out if not internal */
	struct new_database newdb;
	unsigned int magic_len;
	ssize_t rlen;

	/* Fill in the header */
	newdb.hdr.version = TDB_VERSION;
	if (seed)
		newdb.hdr.hash_seed = seed->seed;
	else
		newdb.hdr.hash_seed = random_number(tdb);
	newdb.hdr.hash_test = TDB_HASH_MAGIC;
	newdb.hdr.hash_test = tdb->khash(&newdb.hdr.hash_test,
					 sizeof(newdb.hdr.hash_test),
					 newdb.hdr.hash_seed,
					 tdb->hash_priv);
	newdb.hdr.recovery = 0;
	memset(newdb.hdr.reserved, 0, sizeof(newdb.hdr.reserved));
	/* Initial hashes are empty. */
	memset(newdb.hdr.hashtable, 0, sizeof(newdb.hdr.hashtable));

	/* Free is empty. */
	newdb.hdr.free_table = offsetof(struct new_database, ftable);
	memset(&newdb.ftable, 0, sizeof(newdb.ftable));
	tdb->ecode = set_header(NULL, &newdb.ftable.hdr, TDB_FTABLE_MAGIC, 0,
				sizeof(newdb.ftable) - sizeof(newdb.ftable.hdr),
				sizeof(newdb.ftable) - sizeof(newdb.ftable.hdr),
				0);
	if (tdb->ecode != TDB_SUCCESS) {
		return -1;
	}

	/* Magic food */
	memset(newdb.hdr.magic_food, 0, sizeof(newdb.hdr.magic_food));
	strcpy(newdb.hdr.magic_food, TDB_MAGIC_FOOD);

	/* This creates an endian-converted database, as if read from disk */
	magic_len = sizeof(newdb.hdr.magic_food);
	tdb_convert(tdb,
		    (char *)&newdb.hdr + magic_len, sizeof(newdb) - magic_len);

	*hdr = newdb.hdr;

	if (tdb->flags & TDB_INTERNAL) {
		tdb->map_size = sizeof(newdb);
		tdb->map_ptr = malloc(tdb->map_size);
		if (!tdb->map_ptr) {
			tdb_logerr(tdb, TDB_ERR_OOM, TDB_LOG_ERROR,
				   "tdb_new_database: failed to allocate");
			return -1;
		}
		memcpy(tdb->map_ptr, &newdb, tdb->map_size);
		return 0;
	}
	if (lseek(tdb->fd, 0, SEEK_SET) == -1)
		return -1;

	if (ftruncate(tdb->fd, 0) == -1)
		return -1;

	rlen = write(tdb->fd, &newdb, sizeof(newdb));
	if (rlen != sizeof(newdb)) {
		if (rlen >= 0)
			errno = ENOSPC;
		tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
			   "tdb_new_database: %zi writing header: %s",
			   rlen, strerror(errno));
		return -1;
	}
	return 0;
}

struct tdb_context *tdb_open(const char *name, int tdb_flags,
			     int open_flags, mode_t mode,
			     union tdb_attribute *attr)
{
	struct tdb_context *tdb;
	struct stat st;
	int saved_errno = 0;
	uint64_t hash_test;
	unsigned v;
	ssize_t rlen;
	struct tdb_header hdr;
	struct tdb_attribute_seed *seed = NULL;
	tdb_bool_err berr;
	enum TDB_ERROR ecode;

	tdb = malloc(sizeof(*tdb));
	if (!tdb) {
		/* Can't log this */
		errno = ENOMEM;
		return NULL;
	}
	tdb->name = NULL;
	tdb->map_ptr = NULL;
	tdb->direct_access = 0;
	tdb->fd = -1;
	tdb->map_size = sizeof(struct tdb_header);
	tdb->ecode = TDB_SUCCESS;
	tdb->flags = tdb_flags;
	tdb->logfn = NULL;
	tdb->transaction = NULL;
	tdb->stats = NULL;
	tdb->access = NULL;
	tdb_hash_init(tdb);
	tdb_io_init(tdb);
	tdb_lock_init(tdb);

	while (attr) {
		switch (attr->base.attr) {
		case TDB_ATTRIBUTE_LOG:
			tdb->logfn = attr->log.log_fn;
			tdb->log_private = attr->log.log_private;
			break;
		case TDB_ATTRIBUTE_HASH:
			tdb->khash = attr->hash.hash_fn;
			tdb->hash_priv = attr->hash.hash_private;
			break;
		case TDB_ATTRIBUTE_SEED:
			seed = &attr->seed;
			break;
		case TDB_ATTRIBUTE_STATS:
			tdb->stats = &attr->stats;
			/* They have stats we don't know about?  Tell them. */
			if (tdb->stats->size > sizeof(attr->stats))
				tdb->stats->size = sizeof(attr->stats);
			break;
		default:
			tdb_logerr(tdb, TDB_ERR_EINVAL, TDB_LOG_USE_ERROR,
				   "tdb_open: unknown attribute type %u",
				   attr->base.attr);
			goto fail;
		}
		attr = attr->base.next;
	}

	if ((open_flags & O_ACCMODE) == O_WRONLY) {
		tdb_logerr(tdb, TDB_ERR_EINVAL, TDB_LOG_USE_ERROR,
			   "tdb_open: can't open tdb %s write-only", name);
		goto fail;
	}

	if ((open_flags & O_ACCMODE) == O_RDONLY) {
		tdb->read_only = true;
		/* read only databases don't do locking */
		tdb->flags |= TDB_NOLOCK;
		tdb->mmap_flags = PROT_READ;
	} else {
		tdb->read_only = false;
		tdb->mmap_flags = PROT_READ | PROT_WRITE;
	}

	/* internal databases don't need any of the rest. */
	if (tdb->flags & TDB_INTERNAL) {
		tdb->flags |= (TDB_NOLOCK | TDB_NOMMAP);
		if (tdb_new_database(tdb, seed, &hdr) != 0) {
			goto fail;
		}
		tdb_convert(tdb, &hdr.hash_seed, sizeof(hdr.hash_seed));
		tdb->hash_seed = hdr.hash_seed;
		tdb_ftable_init(tdb);
		return tdb;
	}

	if ((tdb->fd = open(name, open_flags, mode)) == -1) {
		/* errno set by open(2) */
		saved_errno = errno;
		tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
			   "tdb_open: could not open file %s: %s",
			   name, strerror(errno));
		goto fail;
	}

	/* on exec, don't inherit the fd */
	v = fcntl(tdb->fd, F_GETFD, 0);
        fcntl(tdb->fd, F_SETFD, v | FD_CLOEXEC);

	/* ensure there is only one process initialising at once */
	tdb->ecode = tdb_lock_open(tdb, TDB_LOCK_WAIT|TDB_LOCK_NOCHECK);
	if (tdb->ecode != TDB_SUCCESS) {
		goto fail;
	}

	/* If they used O_TRUNC, read will return 0. */
	rlen = read(tdb->fd, &hdr, sizeof(hdr));
	if (rlen == 0 && (open_flags & O_CREAT)) {
		if (tdb_new_database(tdb, seed, &hdr) == -1) {
			goto fail;
		}
	} else if (rlen < 0) {
		tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
			   "tdb_open: error %s reading %s",
			   strerror(errno), name);
		goto fail;
	} else if (rlen < sizeof(hdr)
		   || strcmp(hdr.magic_food, TDB_MAGIC_FOOD) != 0) {
		tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
			   "tdb_open: %s is not a tdb file", name);
		goto fail;
	}

	if (hdr.version != TDB_VERSION) {
		if (hdr.version == bswap_64(TDB_VERSION))
			tdb->flags |= TDB_CONVERT;
		else {
			/* wrong version */
			tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
				   "tdb_open: %s is unknown version 0x%llx",
				   name, (long long)hdr.version);
			goto fail;
		}
	}

	tdb_convert(tdb, &hdr, sizeof(hdr));
	tdb->hash_seed = hdr.hash_seed;
	hash_test = TDB_HASH_MAGIC;
	hash_test = tdb_hash(tdb, &hash_test, sizeof(hash_test));
	if (hdr.hash_test != hash_test) {
		/* wrong hash variant */
		tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
			   "tdb_open: %s uses a different hash function",
			   name);
		goto fail;
	}

	if (fstat(tdb->fd, &st) == -1) {
		saved_errno = errno;
		tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
			   "tdb_open: could not stat open %s: %s",
			   name, strerror(errno));
		goto fail;
	}

	/* Is it already in the open list?  If so, fail. */
	if (tdb_already_open(st.st_dev, st.st_ino)) {
		/* FIXME */
		tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_USE_ERROR,
			   "tdb_open: %s (%d,%d) is already open in this"
			   " process",
			   name, (int)st.st_dev, (int)st.st_ino);
		goto fail;
	}

	tdb->name = strdup(name);
	if (!tdb->name) {
		tdb_logerr(tdb, TDB_ERR_OOM, TDB_LOG_ERROR,
			   "tdb_open: failed to allocate name");
		goto fail;
	}

	tdb->device = st.st_dev;
	tdb->inode = st.st_ino;
	tdb_unlock_open(tdb);

	/* This make sure we have current map_size and mmap. */
	tdb->methods->oob(tdb, tdb->map_size + 1, true);

	/* Now it's fully formed, recover if necessary. */
	berr = tdb_needs_recovery(tdb);
	if (unlikely(berr != false)) {
		if (berr < 0) {
			ecode = berr;
			goto fail;
		}
		ecode = tdb_lock_and_recover(tdb);
		if (ecode != TDB_SUCCESS) {
			tdb->ecode = ecode;
			goto fail;
		}
	}

	tdb->ecode = tdb_ftable_init(tdb);
	if (tdb->ecode != TDB_SUCCESS) {
		goto fail;
	}

	tdb->next = tdbs;
	tdbs = tdb;
	return tdb;

 fail:
	/* Map ecode to some logical errno. */
	if (!saved_errno) {
		switch (tdb->ecode) {
		case TDB_ERR_CORRUPT:
		case TDB_ERR_IO:
			saved_errno = EIO;
			break;
		case TDB_ERR_LOCK:
			saved_errno = EWOULDBLOCK;
			break;
		case TDB_ERR_OOM:
			saved_errno = ENOMEM;
			break;
		case TDB_ERR_EINVAL:
			saved_errno = EINVAL;
			break;
		default:
			saved_errno = EINVAL;
			break;
		}
	}

#ifdef TDB_TRACE
	close(tdb->tracefd);
#endif
	if (tdb->map_ptr) {
		if (tdb->flags & TDB_INTERNAL) {
			free(tdb->map_ptr);
		} else
			tdb_munmap(tdb);
	}
	free(tdb->lockrecs);
	free((char *)tdb->name);
	if (tdb->fd != -1)
		if (close(tdb->fd) != 0)
			tdb_logerr(tdb, TDB_ERR_IO, TDB_LOG_ERROR,
				   "tdb_open: failed to close tdb->fd"
				   " on error: %s", strerror(errno));
	free(tdb);
	errno = saved_errno;
	return NULL;
}

static int update_rec_hdr(struct tdb_context *tdb,
			  tdb_off_t off,
			  tdb_len_t keylen,
			  tdb_len_t datalen,
			  struct tdb_used_record *rec,
			  uint64_t h)
{
	uint64_t dataroom = rec_data_length(rec) + rec_extra_padding(rec);
	enum TDB_ERROR ecode;

	ecode = set_header(tdb, rec, TDB_USED_MAGIC, keylen, datalen,
			   keylen + dataroom, h);
	if (ecode != TDB_SUCCESS) {
		tdb->ecode = ecode;
		return -1;
	}

	ecode = tdb_write_convert(tdb, off, rec, sizeof(*rec));
	if (ecode != TDB_SUCCESS) {
		tdb->ecode = ecode;
		return -1;
	}
	return 0;
}

/* Returns -1 on error, 0 on OK */
static int replace_data(struct tdb_context *tdb,
			struct hash_info *h,
			struct tdb_data key, struct tdb_data dbuf,
			tdb_off_t old_off, tdb_len_t old_room,
			bool growing)
{
	tdb_off_t new_off;
	enum TDB_ERROR ecode;

	/* Allocate a new record. */
	new_off = alloc(tdb, key.dsize, dbuf.dsize, h->h, TDB_USED_MAGIC,
			growing);
	if (TDB_OFF_IS_ERR(new_off)) {
		tdb->ecode = new_off;
		return -1;
	}

	/* We didn't like the existing one: remove it. */
	if (old_off) {
		add_stat(tdb, frees, 1);
		ecode = add_free_record(tdb, old_off,
					sizeof(struct tdb_used_record)
					+ key.dsize + old_room);
		if (ecode == TDB_SUCCESS)
			ecode = replace_in_hash(tdb, h, new_off);
	} else {
		ecode = add_to_hash(tdb, h, new_off);
	}
	if (ecode != TDB_SUCCESS) {
		tdb->ecode = ecode;
		return -1;
	}

	new_off += sizeof(struct tdb_used_record);
	ecode = tdb->methods->twrite(tdb, new_off, key.dptr, key.dsize);
	if (ecode != TDB_SUCCESS) {
		tdb->ecode = ecode;
		return -1;
	}

	new_off += key.dsize;
	ecode = tdb->methods->twrite(tdb, new_off, dbuf.dptr, dbuf.dsize);
	if (ecode != TDB_SUCCESS) {
		tdb->ecode = ecode;
		return -1;
	}

	/* FIXME: tdb_increment_seqnum(tdb); */
	return 0;
}

int tdb_store(struct tdb_context *tdb,
	      struct tdb_data key, struct tdb_data dbuf, int flag)
{
	struct hash_info h;
	tdb_off_t off;
	tdb_len_t old_room = 0;
	struct tdb_used_record rec;
	int ret;
	enum TDB_ERROR ecode;

	off = find_and_lock(tdb, key, F_WRLCK, &h, &rec, NULL);
	if (TDB_OFF_IS_ERR(off)) {
		tdb->ecode = off;
		return -1;
	}

	/* Now we have lock on this hash bucket. */
	if (flag == TDB_INSERT) {
		if (off) {
			tdb->ecode = TDB_ERR_EXISTS;
			goto fail;
		}
	} else {
		if (off) {
			old_room = rec_data_length(&rec)
				+ rec_extra_padding(&rec);
			if (old_room >= dbuf.dsize) {
				/* Can modify in-place.  Easy! */
				if (update_rec_hdr(tdb, off,
						   key.dsize, dbuf.dsize,
						   &rec, h.h))
					goto fail;
				ecode = tdb->methods->twrite(tdb,
							     off + sizeof(rec)
							     + key.dsize,
							     dbuf.dptr,
							     dbuf.dsize);
				if (ecode != TDB_SUCCESS) {
					tdb->ecode = ecode;
					goto fail;
				}
				tdb_unlock_hashes(tdb, h.hlock_start,
						  h.hlock_range, F_WRLCK);
				return 0;
			}
		} else {
			if (flag == TDB_MODIFY) {
				/* if the record doesn't exist and we
				   are in TDB_MODIFY mode then we should fail
				   the store */
				tdb->ecode = TDB_ERR_NOEXIST;
				goto fail;
			}
		}
	}

	/* If we didn't use the old record, this implies we're growing. */
	ret = replace_data(tdb, &h, key, dbuf, off, old_room, off != 0);
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return ret;

fail:
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return -1;
}

int tdb_append(struct tdb_context *tdb,
	       struct tdb_data key, struct tdb_data dbuf)
{
	struct hash_info h;
	tdb_off_t off;
	struct tdb_used_record rec;
	tdb_len_t old_room = 0, old_dlen;
	unsigned char *newdata;
	struct tdb_data new_dbuf;
	enum TDB_ERROR ecode;
	int ret;

	off = find_and_lock(tdb, key, F_WRLCK, &h, &rec, NULL);
	if (TDB_OFF_IS_ERR(off)) {
		tdb->ecode = off;
		return -1;
	}

	if (off) {
		old_dlen = rec_data_length(&rec);
		old_room = old_dlen + rec_extra_padding(&rec);

		/* Fast path: can append in place. */
		if (rec_extra_padding(&rec) >= dbuf.dsize) {
			if (update_rec_hdr(tdb, off, key.dsize,
					   old_dlen + dbuf.dsize, &rec, h.h))
				goto fail;

			off += sizeof(rec) + key.dsize + old_dlen;
			ecode = tdb->methods->twrite(tdb, off, dbuf.dptr,
						     dbuf.dsize);
			if (ecode != TDB_SUCCESS) {
				tdb->ecode = ecode;
				goto fail;
			}

			/* FIXME: tdb_increment_seqnum(tdb); */
			tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range,
					  F_WRLCK);
			return 0;
		}

		/* Slow path. */
		newdata = malloc(key.dsize + old_dlen + dbuf.dsize);
		if (!newdata) {
			tdb_logerr(tdb, TDB_ERR_OOM, TDB_LOG_ERROR,
				   "tdb_append: failed to allocate %zu bytes",
				   (size_t)(key.dsize+old_dlen+dbuf.dsize));
			goto fail;
		}
		ecode = tdb->methods->tread(tdb, off + sizeof(rec) + key.dsize,
					    newdata, old_dlen);
		if (ecode != TDB_SUCCESS) {
			tdb->ecode = ecode;
			free(newdata);
			goto fail;
		}
		memcpy(newdata + old_dlen, dbuf.dptr, dbuf.dsize);
		new_dbuf.dptr = newdata;
		new_dbuf.dsize = old_dlen + dbuf.dsize;
	} else {
		newdata = NULL;
		new_dbuf = dbuf;
	}

	/* If they're using tdb_append(), it implies they're growing record. */
	ret = replace_data(tdb, &h, key, new_dbuf, off, old_room, true);
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	free(newdata);

	return ret;

fail:
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return -1;
}

struct tdb_data tdb_fetch(struct tdb_context *tdb, struct tdb_data key)
{
	tdb_off_t off;
	struct tdb_used_record rec;
	struct hash_info h;
	struct tdb_data ret;

	off = find_and_lock(tdb, key, F_RDLCK, &h, &rec, NULL);
	if (TDB_OFF_IS_ERR(off)) {
		tdb->ecode = off;
		return tdb_null;
	}

	if (!off) {
		tdb->ecode = TDB_ERR_NOEXIST;
		ret = tdb_null;
	} else {
		ret.dsize = rec_data_length(&rec);
		ret.dptr = tdb_alloc_read(tdb, off + sizeof(rec) + key.dsize,
					  ret.dsize);
		if (TDB_PTR_IS_ERR(ret.dptr)) {
			tdb->ecode = TDB_PTR_ERR(ret.dptr);
			ret = tdb_null;
		}
	}

	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_RDLCK);
	return ret;
}

int tdb_delete(struct tdb_context *tdb, struct tdb_data key)
{
	tdb_off_t off;
	struct tdb_used_record rec;
	struct hash_info h;
	enum TDB_ERROR ecode;

	off = find_and_lock(tdb, key, F_WRLCK, &h, &rec, NULL);
	if (TDB_OFF_IS_ERR(off)) {
		tdb->ecode = off;
		return -1;
	}

	if (!off) {
		tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
		tdb->ecode = TDB_ERR_NOEXIST;
		return -1;
	}

	ecode = delete_from_hash(tdb, &h);
	if (ecode != TDB_SUCCESS) {
		tdb->ecode = ecode;
		goto unlock_err;
	}

	/* Free the deleted entry. */
	add_stat(tdb, frees, 1);
	ecode = add_free_record(tdb, off,
				sizeof(struct tdb_used_record)
				+ rec_key_length(&rec)
				+ rec_data_length(&rec)
				+ rec_extra_padding(&rec));
	if (ecode != TDB_SUCCESS) {
		tdb->ecode = ecode;
		goto unlock_err;
	}

	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return 0;

unlock_err:
	tdb_unlock_hashes(tdb, h.hlock_start, h.hlock_range, F_WRLCK);
	return -1;
}

int tdb_close(struct tdb_context *tdb)
{
	struct tdb_context **i;
	int ret = 0;

	tdb_trace(tdb, "tdb_close");

	if (tdb->transaction) {
		tdb_transaction_cancel(tdb);
	}

	if (tdb->map_ptr) {
		if (tdb->flags & TDB_INTERNAL)
			free(tdb->map_ptr);
		else
			tdb_munmap(tdb);
	}
	free((char *)tdb->name);
	if (tdb->fd != -1) {
		ret = close(tdb->fd);
		tdb->fd = -1;
	}
	free(tdb->lockrecs);

	/* Remove from contexts list */
	for (i = &tdbs; *i; i = &(*i)->next) {
		if (*i == tdb) {
			*i = tdb->next;
			break;
		}
	}

#ifdef TDB_TRACE
	close(tdb->tracefd);
#endif
	free(tdb);

	return ret;
}

enum TDB_ERROR tdb_error(const struct tdb_context *tdb)
{
	return tdb->ecode;
}

const char *tdb_errorstr(const struct tdb_context *tdb)
{
	/* Gcc warns if you miss a case in the switch, so use that. */
	switch (tdb->ecode) {
	case TDB_SUCCESS: return "Success";
	case TDB_ERR_CORRUPT: return "Corrupt database";
	case TDB_ERR_IO: return "IO Error";
	case TDB_ERR_LOCK: return "Locking error";
	case TDB_ERR_OOM: return "Out of memory";
	case TDB_ERR_EXISTS: return "Record exists";
	case TDB_ERR_EINVAL: return "Invalid parameter";
	case TDB_ERR_NOEXIST: return "Record does not exist";
	case TDB_ERR_RDONLY: return "write not permitted";
	}
	return "Invalid error code";
}

enum TDB_ERROR COLD tdb_logerr(struct tdb_context *tdb,
			       enum TDB_ERROR ecode,
			       enum tdb_log_level level,
			       const char *fmt, ...)
{
	char *message;
	va_list ap;
	size_t len;
	/* tdb_open paths care about errno, so save it. */
	int saved_errno = errno;

	tdb->ecode = ecode;

	if (!tdb->logfn)
		return ecode;

	/* FIXME: Doesn't assume asprintf. */
	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);

	message = malloc(len + 1);
	if (!message) {
		tdb->logfn(tdb, TDB_LOG_ERROR, tdb->log_private,
			   "out of memory formatting message:");
		tdb->logfn(tdb, level, tdb->log_private, fmt);
		return ecode;
	}
	va_start(ap, fmt);
	len = vsprintf(message, fmt, ap);
	va_end(ap);
	tdb->logfn(tdb, level, tdb->log_private, message);
	free(message);
	errno = saved_errno;
	return ecode;
}
