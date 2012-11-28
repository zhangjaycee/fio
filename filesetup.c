#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "fio.h"
#include "smalloc.h"
#include "filehash.h"
#include "os/os.h"
#include "hash.h"
#include "lib/axmap.h"

#ifdef FIO_HAVE_LINUX_FALLOCATE
#include <linux/falloc.h>
#endif

static int root_warn;

static inline void clear_error(struct thread_data *td)
{
	td->error = 0;
	td->verror[0] = '\0';
}

/*
 * Leaves f->fd open on success, caller must close
 */
static int extend_file(struct thread_data *td, struct fio_file *f)
{
	int r, new_layout = 0, unlink_file = 0, flags;
	unsigned long long left;
	unsigned int bs;
	char *b;

	if (read_only) {
		log_err("fio: refusing extend of file due to read-only\n");
		return 0;
	}

	/*
	 * check if we need to lay the file out complete again. fio
	 * does that for operations involving reads, or for writes
	 * where overwrite is set
	 */
	if (td_read(td) || (td_write(td) && td->o.overwrite) ||
	    (td_write(td) && td->io_ops->flags & FIO_NOEXTEND))
		new_layout = 1;
	if (td_write(td) && !td->o.overwrite)
		unlink_file = 1;

	if (unlink_file || new_layout) {
		dprint(FD_FILE, "layout unlink %s\n", f->file_name);
		if ((unlink(f->file_name) < 0) && (errno != ENOENT)) {
			td_verror(td, errno, "unlink");
			return 1;
		}
	}

	flags = O_WRONLY | O_CREAT;
	if (new_layout)
		flags |= O_TRUNC;

	dprint(FD_FILE, "open file %s, flags %x\n", f->file_name, flags);
	f->fd = open(f->file_name, flags, 0644);
	if (f->fd < 0) {
		td_verror(td, errno, "open");
		return 1;
	}

#ifdef FIO_HAVE_FALLOCATE
	if (!td->o.fill_device) {
		switch (td->o.fallocate_mode) {
		case FIO_FALLOCATE_NONE:
			break;
		case FIO_FALLOCATE_POSIX:
			dprint(FD_FILE, "posix_fallocate file %s size %llu\n",
				 f->file_name, f->real_file_size);

			r = posix_fallocate(f->fd, 0, f->real_file_size);
			if (r > 0) {
				log_err("fio: posix_fallocate fails: %s\n",
						strerror(r));
			}
			break;
#ifdef FIO_HAVE_LINUX_FALLOCATE
		case FIO_FALLOCATE_KEEP_SIZE:
			dprint(FD_FILE,
				"fallocate(FALLOC_FL_KEEP_SIZE) "
				"file %s size %llu\n",
				f->file_name, f->real_file_size);

			r = fallocate(f->fd, FALLOC_FL_KEEP_SIZE, 0,
					f->real_file_size);
			if (r != 0) {
				td_verror(td, errno, "fallocate");
			}
			break;
#endif /* FIO_HAVE_LINUX_FALLOCATE */
		default:
			log_err("fio: unknown fallocate mode: %d\n",
				td->o.fallocate_mode);
			assert(0);
		}
	}
#endif /* FIO_HAVE_FALLOCATE */

	if (!new_layout)
		goto done;

	/*
	 * The size will be -1ULL when fill_device is used, so don't truncate
	 * or fallocate this file, just write it
	 */
	if (!td->o.fill_device) {
		dprint(FD_FILE, "truncate file %s, size %llu\n", f->file_name,
							f->real_file_size);
		if (ftruncate(f->fd, f->real_file_size) == -1) {
			td_verror(td, errno, "ftruncate");
			goto err;
		}
	}

	b = malloc(td->o.max_bs[DDIR_WRITE]);
	memset(b, 0, td->o.max_bs[DDIR_WRITE]);

	left = f->real_file_size;
	while (left && !td->terminate) {
		bs = td->o.max_bs[DDIR_WRITE];
		if (bs > left)
			bs = left;

		r = write(f->fd, b, bs);

		if (r > 0) {
			left -= r;
			continue;
		} else {
			if (r < 0) {
				int __e = errno;

				if (__e == ENOSPC) {
					if (td->o.fill_device)
						break;
					log_info("fio: ENOSPC on laying out "
						 "file, stopping\n");
					break;
				}
				td_verror(td, errno, "write");
			} else
				td_verror(td, EIO, "write");

			break;
		}
	}

	if (td->terminate) {
		dprint(FD_FILE, "terminate unlink %s\n", f->file_name);
		unlink(f->file_name);
	} else if (td->o.create_fsync) {
		if (fsync(f->fd) < 0) {
			td_verror(td, errno, "fsync");
			goto err;
		}
	}
	if (td->o.fill_device && !td_write(td)) {
		fio_file_clear_size_known(f);
		if (td_io_get_file_size(td, f))
			goto err;
		if (f->io_size > f->real_file_size)
			f->io_size = f->real_file_size;
	}

	free(b);
done:
	return 0;
err:
	close(f->fd);
	f->fd = -1;
	return 1;
}

static int pre_read_file(struct thread_data *td, struct fio_file *f)
{
	int r, did_open = 0, old_runstate;
	unsigned long long left;
	unsigned int bs;
	char *b;

	if (td->io_ops->flags & FIO_PIPEIO)
		return 0;

	if (!fio_file_open(f)) {
		if (td->io_ops->open_file(td, f)) {
			log_err("fio: cannot pre-read, failed to open file\n");
			return 1;
		}
		did_open = 1;
	}

	old_runstate = td->runstate;
	td_set_runstate(td, TD_PRE_READING);

	bs = td->o.max_bs[DDIR_READ];
	b = malloc(bs);
	memset(b, 0, bs);

	lseek(f->fd, f->file_offset, SEEK_SET);
	left = f->io_size;

	while (left && !td->terminate) {
		if (bs > left)
			bs = left;

		r = read(f->fd, b, bs);

		if (r == (int) bs) {
			left -= bs;
			continue;
		} else {
			td_verror(td, EIO, "pre_read");
			break;
		}
	}

	td_set_runstate(td, old_runstate);

	if (did_open)
		td->io_ops->close_file(td, f);
	free(b);
	return 0;
}

static unsigned long long get_rand_file_size(struct thread_data *td)
{
	unsigned long long ret, sized;
	unsigned long r;

	if (td->o.use_os_rand) {
		r = os_random_long(&td->file_size_state);
		sized = td->o.file_size_high - td->o.file_size_low;
		ret = (unsigned long long) ((double) sized * (r / (OS_RAND_MAX + 1.0)));
	} else {
		r = __rand(&td->__file_size_state);
		sized = td->o.file_size_high - td->o.file_size_low;
		ret = (unsigned long long) ((double) sized * (r / (FRAND_MAX + 1.0)));
	}

	ret += td->o.file_size_low;
	ret -= (ret % td->o.rw_min_bs);
	return ret;
}

static int file_size(struct thread_data *td, struct fio_file *f)
{
	struct stat st;

	if (stat(f->file_name, &st) == -1) {
		td_verror(td, errno, "fstat");
		return 1;
	}

	f->real_file_size = st.st_size;
	return 0;
}

static int bdev_size(struct thread_data *td, struct fio_file *f)
{
	unsigned long long bytes = 0;
	int r;

	if (td->io_ops->open_file(td, f)) {
		log_err("fio: failed opening blockdev %s for size check\n",
			f->file_name);
		return 1;
	}

	r = blockdev_size(f, &bytes);
	if (r) {
		td_verror(td, r, "blockdev_size");
		goto err;
	}

	if (!bytes) {
		log_err("%s: zero sized block device?\n", f->file_name);
		goto err;
	}

	f->real_file_size = bytes;
	td->io_ops->close_file(td, f);
	return 0;
err:
	td->io_ops->close_file(td, f);
	return 1;
}

static int char_size(struct thread_data *td, struct fio_file *f)
{
#ifdef FIO_HAVE_CHARDEV_SIZE
	unsigned long long bytes = 0;
	int r;

	if (td->io_ops->open_file(td, f)) {
		log_err("fio: failed opening blockdev %s for size check\n",
			f->file_name);
		return 1;
	}

	r = chardev_size(f, &bytes);
	if (r) {
		td_verror(td, r, "chardev_size");
		goto err;
	}

	if (!bytes) {
		log_err("%s: zero sized char device?\n", f->file_name);
		goto err;
	}

	f->real_file_size = bytes;
	td->io_ops->close_file(td, f);
	return 0;
err:
	td->io_ops->close_file(td, f);
	return 1;
#else
	f->real_file_size = -1ULL;
	return 0;
#endif
}

static int get_file_size(struct thread_data *td, struct fio_file *f)
{
	int ret = 0;

	if (fio_file_size_known(f))
		return 0;

	if (f->filetype == FIO_TYPE_FILE)
		ret = file_size(td, f);
	else if (f->filetype == FIO_TYPE_BD)
		ret = bdev_size(td, f);
	else if (f->filetype == FIO_TYPE_CHAR)
		ret = char_size(td, f);
	else
		f->real_file_size = -1;

	if (ret)
		return ret;

	if (f->file_offset > f->real_file_size) {
		log_err("%s: offset extends end (%llu > %llu)\n", td->o.name,
					f->file_offset, f->real_file_size);
		return 1;
	}

	fio_file_set_size_known(f);
	return 0;
}

static int __file_invalidate_cache(struct thread_data *td, struct fio_file *f,
				   unsigned long long off,
				   unsigned long long len)
{
	int ret = 0;

	if (len == -1ULL)
		len = f->io_size;
	if (off == -1ULL)
		off = f->file_offset;

	if (len == -1ULL || off == -1ULL)
		return 0;

	dprint(FD_IO, "invalidate cache %s: %llu/%llu\n", f->file_name, off,
								len);

	/*
	 * FIXME: add blockdev flushing too
	 */
	if (f->mmap_ptr) {
		ret = posix_madvise(f->mmap_ptr, f->mmap_sz, POSIX_MADV_DONTNEED);
#ifdef FIO_MADV_FREE
		(void) posix_madvise(f->mmap_ptr, f->mmap_sz, FIO_MADV_FREE);
#endif
	} else if (f->filetype == FIO_TYPE_FILE) {
		ret = posix_fadvise(f->fd, off, len, POSIX_FADV_DONTNEED);
	} else if (f->filetype == FIO_TYPE_BD) {
		ret = blockdev_invalidate_cache(f);
		if (ret < 0 && errno == EACCES && geteuid()) {
			if (!root_warn) {
				log_err("fio: only root may flush block "
					"devices. Cache flush bypassed!\n");
				root_warn = 1;
			}
			ret = 0;
		}
	} else if (f->filetype == FIO_TYPE_CHAR || f->filetype == FIO_TYPE_PIPE)
		ret = 0;

	if (ret < 0) {
		td_verror(td, errno, "invalidate_cache");
		return 1;
	} else if (ret > 0) {
		td_verror(td, ret, "invalidate_cache");
		return 1;
	}

	return ret;

}

int file_invalidate_cache(struct thread_data *td, struct fio_file *f)
{
	if (!fio_file_open(f))
		return 0;

	return __file_invalidate_cache(td, f, -1ULL, -1ULL);
}

int generic_close_file(struct thread_data fio_unused *td, struct fio_file *f)
{
	int ret = 0;

	dprint(FD_FILE, "fd close %s\n", f->file_name);

	remove_file_hash(f);

	if (close(f->fd) < 0)
		ret = errno;

	f->fd = -1;
	return ret;
}

int file_lookup_open(struct fio_file *f, int flags)
{
	struct fio_file *__f;
	int from_hash;

	__f = lookup_file_hash(f->file_name);
	if (__f) {
		dprint(FD_FILE, "found file in hash %s\n", f->file_name);
		/*
		 * racy, need the __f->lock locked
		 */
		f->lock = __f->lock;
		f->lock_owner = __f->lock_owner;
		f->lock_batch = __f->lock_batch;
		f->lock_ddir = __f->lock_ddir;
		from_hash = 1;
	} else {
		dprint(FD_FILE, "file not found in hash %s\n", f->file_name);
		from_hash = 0;
	}

	f->fd = open(f->file_name, flags, 0600);
	return from_hash;
}

int generic_open_file(struct thread_data *td, struct fio_file *f)
{
	int is_std = 0;
	int flags = 0;
	int from_hash = 0;

	dprint(FD_FILE, "fd open %s\n", f->file_name);

	if (td_trim(td) && f->filetype != FIO_TYPE_BD) {
		log_err("fio: trim only applies to block device\n");
		return 1;
	}

	if (!strcmp(f->file_name, "-")) {
		if (td_rw(td)) {
			log_err("fio: can't read/write to stdin/out\n");
			return 1;
		}
		is_std = 1;

		/*
		 * move output logging to stderr, if we are writing to stdout
		 */
		if (td_write(td))
			f_out = stderr;
	}

	if (td_trim(td))
		goto skip_flags;
	if (td->o.odirect)
		flags |= OS_O_DIRECT;
	if (td->o.sync_io)
		flags |= O_SYNC;
	if (td->o.create_on_open)
		flags |= O_CREAT;
skip_flags:
	if (f->filetype != FIO_TYPE_FILE)
		flags |= FIO_O_NOATIME;

open_again:
	if (td_write(td)) {
		if (!read_only)
			flags |= O_RDWR;

		if (f->filetype == FIO_TYPE_FILE)
			flags |= O_CREAT;

		if (is_std)
			f->fd = dup(STDOUT_FILENO);
		else
			from_hash = file_lookup_open(f, flags);
	} else if (td_read(td)) {
		if (f->filetype == FIO_TYPE_CHAR && !read_only)
			flags |= O_RDWR;
		else
			flags |= O_RDONLY;

		if (is_std)
			f->fd = dup(STDIN_FILENO);
		else
			from_hash = file_lookup_open(f, flags);
	} else { //td trim
		flags |= O_RDWR;
		from_hash = file_lookup_open(f, flags);
	}

	if (f->fd == -1) {
		char buf[FIO_VERROR_SIZE];
		int __e = errno;

		if (__e == EPERM && (flags & FIO_O_NOATIME)) {
			flags &= ~FIO_O_NOATIME;
			goto open_again;
		}

		snprintf(buf, sizeof(buf) - 1, "open(%s)", f->file_name);

		if (__e == EINVAL && (flags & OS_O_DIRECT)) {
			log_err("fio: looks like your file system does not " \
				"support direct=1/buffered=0\n");
		}

		td_verror(td, __e, buf);
	}

	if (!from_hash && f->fd != -1) {
		if (add_file_hash(f)) {
			int fio_unused ret;

			/*
			 * OK to ignore, we haven't done anything with it
			 */
			ret = generic_close_file(td, f);
			goto open_again;
		}
	}

	return 0;
}

int generic_get_file_size(struct thread_data *td, struct fio_file *f)
{
	return get_file_size(td, f);
}

/*
 * open/close all files, so that ->real_file_size gets set
 */
static int get_file_sizes(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;
	int err = 0;

	for_each_file(td, f, i) {
		dprint(FD_FILE, "get file size for %p/%d/%p\n", f, i,
								f->file_name);

		if (td_io_get_file_size(td, f)) {
			if (td->error != ENOENT) {
				log_err("%s\n", td->verror);
				err = 1;
			}
			clear_error(td);
		}

		if (f->real_file_size == -1ULL && td->o.size)
			f->real_file_size = td->o.size / td->o.nr_files;
	}

	return err;
}

struct fio_mount {
	struct flist_head list;
	const char *base;
	char __base[256];
	unsigned int key;
};

/*
 * Get free number of bytes for each file on each unique mount.
 */
static unsigned long long get_fs_free_counts(struct thread_data *td)
{
	struct flist_head *n, *tmp;
	unsigned long long ret = 0;
	struct fio_mount *fm;
	FLIST_HEAD(list);
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i) {
		struct stat sb;
		char buf[256];

		if (f->filetype == FIO_TYPE_BD || f->filetype == FIO_TYPE_CHAR) {
			if (f->real_file_size != -1ULL)
				ret += f->real_file_size;
			continue;
		} else if (f->filetype != FIO_TYPE_FILE)
			continue;

		strcpy(buf, f->file_name);

		if (stat(buf, &sb) < 0) {
			if (errno != ENOENT)
				break;
			strcpy(buf, ".");
			if (stat(buf, &sb) < 0)
				break;
		}

		fm = NULL;
		flist_for_each(n, &list) {
			fm = flist_entry(n, struct fio_mount, list);
			if (fm->key == sb.st_dev)
				break;

			fm = NULL;
		}

		if (fm)
			continue;

		fm = malloc(sizeof(*fm));
		strcpy(fm->__base, buf);
		fm->base = basename(fm->__base);
		fm->key = sb.st_dev;
		flist_add(&fm->list, &list);
	}

	flist_for_each_safe(n, tmp, &list) {
		unsigned long long sz;

		fm = flist_entry(n, struct fio_mount, list);
		flist_del(&fm->list);

		sz = get_fs_size(fm->base);
		if (sz && sz != -1ULL)
			ret += sz;

		free(fm);
	}

	return ret;
}

unsigned long long get_start_offset(struct thread_data *td)
{
	return td->o.start_offset +
		(td->thread_number - 1) * td->o.offset_increment;
}

/*
 * Open the files and setup files sizes, creating files if necessary.
 */
int setup_files(struct thread_data *td)
{
	unsigned long long total_size, extend_size;
	struct fio_file *f;
	unsigned int i;
	int err = 0, need_extend;

	dprint(FD_FILE, "setup files\n");

	if (td->o.read_iolog_file)
		goto done;

	/*
	 * if ioengine defines a setup() method, it's responsible for
	 * opening the files and setting f->real_file_size to indicate
	 * the valid range for that file.
	 */
	if (td->io_ops->setup)
		err = td->io_ops->setup(td);
	else
		err = get_file_sizes(td);

	if (err)
		return err;

	/*
	 * check sizes. if the files/devices do not exist and the size
	 * isn't passed to fio, abort.
	 */
	total_size = 0;
	for_each_file(td, f, i) {
		if (f->real_file_size == -1ULL)
			total_size = -1ULL;
		else
			total_size += f->real_file_size;
	}

	if (td->o.fill_device)
		td->fill_device_size = get_fs_free_counts(td);

	/*
	 * device/file sizes are zero and no size given, punt
	 */
	if ((!total_size || total_size == -1ULL) && !td->o.size &&
	    !(td->io_ops->flags & FIO_NOIO) && !td->o.fill_device) {
		log_err("%s: you need to specify size=\n", td->o.name);
		td_verror(td, EINVAL, "total_file_size");
		return 1;
	}

	/*
	 * now file sizes are known, so we can set ->io_size. if size= is
	 * not given, ->io_size is just equal to ->real_file_size. if size
	 * is given, ->io_size is size / nr_files.
	 */
	extend_size = total_size = 0;
	need_extend = 0;
	for_each_file(td, f, i) {
		f->file_offset = get_start_offset(td);

		if (!td->o.file_size_low) {
			/*
			 * no file size range given, file size is equal to
			 * total size divided by number of files. if that is
			 * zero, set it to the real file size.
			 */
			f->io_size = td->o.size / td->o.nr_files;
			if (!f->io_size)
				f->io_size = f->real_file_size - f->file_offset;
		} else if (f->real_file_size < td->o.file_size_low ||
			   f->real_file_size > td->o.file_size_high) {
			if (f->file_offset > td->o.file_size_low)
				goto err_offset;
			/*
			 * file size given. if it's fixed, use that. if it's a
			 * range, generate a random size in-between.
			 */
			if (td->o.file_size_low == td->o.file_size_high) {
				f->io_size = td->o.file_size_low
						- f->file_offset;
			} else {
				f->io_size = get_rand_file_size(td)
						- f->file_offset;
			}
		} else
			f->io_size = f->real_file_size - f->file_offset;

		if (f->io_size == -1ULL)
			total_size = -1ULL;
		else {
                        if (td->o.size_percent)
                                f->io_size = (f->io_size * td->o.size_percent) / 100;
			total_size += f->io_size;
		}

		if (f->filetype == FIO_TYPE_FILE &&
		    (f->io_size + f->file_offset) > f->real_file_size &&
		    !(td->io_ops->flags & FIO_DISKLESSIO)) {
			if (!td->o.create_on_open) {
				need_extend++;
				extend_size += (f->io_size + f->file_offset);
			} else
				f->real_file_size = f->io_size + f->file_offset;
			fio_file_set_extend(f);
		}
	}

	if (!td->o.size || td->o.size > total_size)
		td->o.size = total_size;

	/*
	 * See if we need to extend some files
	 */
	if (need_extend) {
		temp_stall_ts = 1;
		if (output_format == FIO_OUTPUT_NORMAL)
			log_info("%s: Laying out IO file(s) (%u file(s) /"
				 " %lluMB)\n", td->o.name, need_extend,
					extend_size >> 20);

		for_each_file(td, f, i) {
			unsigned long long old_len = -1ULL, extend_len = -1ULL;

			if (!fio_file_extend(f))
				continue;

			assert(f->filetype == FIO_TYPE_FILE);
			fio_file_clear_extend(f);
			if (!td->o.fill_device) {
				old_len = f->real_file_size;
				extend_len = f->io_size + f->file_offset -
						old_len;
			}
			f->real_file_size = (f->io_size + f->file_offset);
			err = extend_file(td, f);
			if (err)
				break;

			err = __file_invalidate_cache(td, f, old_len,
								extend_len);
			close(f->fd);
			f->fd = -1;
			if (err)
				break;
		}
		temp_stall_ts = 0;
	}

	if (err)
		return err;

	if (!td->o.zone_size)
		td->o.zone_size = td->o.size;

	/*
	 * iolog already set the total io size, if we read back
	 * stored entries.
	 */
	if (!td->o.read_iolog_file)
		td->total_io_size = td->o.size * td->o.loops;

done:
	if (td->o.create_only)
		td->done = 1;

	return 0;
err_offset:
	log_err("%s: you need to specify valid offset=\n", td->o.name);
	return 1;
}

int pre_read_files(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_FILE, "pre_read files\n");

	for_each_file(td, f, i) {
		pre_read_file(td, f);
	}

	return 1;
}

static int __init_rand_distribution(struct thread_data *td, struct fio_file *f)
{
	unsigned int range_size, seed;
	unsigned long nranges;

	range_size = min(td->o.min_bs[DDIR_READ], td->o.min_bs[DDIR_WRITE]);

	nranges = (f->real_file_size + range_size - 1) / range_size;

	seed = jhash(f->file_name, strlen(f->file_name), 0) * td->thread_number;
	if (!td->o.rand_repeatable)
		seed = td->rand_seeds[4];

	if (td->o.random_distribution == FIO_RAND_DIST_ZIPF)
		zipf_init(&f->zipf, nranges, td->o.zipf_theta, seed);
	else
		pareto_init(&f->zipf, nranges, td->o.pareto_h, seed);

	return 1;
}

static int init_rand_distribution(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;
	int state;

	if (td->o.random_distribution == FIO_RAND_DIST_RANDOM)
		return 0;

	state = td->runstate;
	td_set_runstate(td, TD_SETTING_UP);
	for_each_file(td, f, i)
		__init_rand_distribution(td, f);
	td_set_runstate(td, state);

	return 1;
}

int init_random_map(struct thread_data *td)
{
	unsigned long long blocks;
	struct fio_file *f;
	unsigned int i;

	if (init_rand_distribution(td))
		return 0;
	if (!td_random(td))
		return 0;

	for_each_file(td, f, i) {
		blocks = (f->real_file_size + td->o.rw_min_bs - 1) /
				(unsigned long long) td->o.rw_min_bs;
		if (td->o.random_generator == FIO_RAND_GEN_LFSR) {
			if (!lfsr_init(&f->lfsr, blocks))
				continue;
		} else if (!td->o.norandommap) {
			f->io_axmap = axmap_new(blocks);
			if (f->io_axmap)
				continue;
		}

		if (!td->o.softrandommap) {
			log_err("fio: failed allocating random map. If running"
				" a large number of jobs, try the 'norandommap'"
				" option or set 'softrandommap'. Or give"
				" a larger --alloc-size to fio.\n");
			return 1;
		}

		log_info("fio: file %s failed allocating random map. Running "
			 "job without.\n", f->file_name);
	}

	return 0;
}

void close_files(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i) {
		if (fio_file_open(f))
			td_io_close_file(td, f);
	}
}

void close_and_free_files(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_FILE, "close files\n");

	for_each_file(td, f, i) {
		if (td->o.unlink && f->filetype == FIO_TYPE_FILE) {
			dprint(FD_FILE, "free unlink %s\n", f->file_name);
			unlink(f->file_name);
		}

		if (fio_file_open(f))
			td_io_close_file(td, f);

		remove_file_hash(f);

		sfree(f->file_name);
		f->file_name = NULL;
		axmap_free(f->io_axmap);
		f->io_axmap = NULL;
		sfree(f);
	}

	td->o.filename = NULL;
	free(td->files);
	td->files_index = 0;
	td->files = NULL;
	td->o.nr_files = 0;
}

static void get_file_type(struct fio_file *f)
{
	struct stat sb;

	if (!strcmp(f->file_name, "-"))
		f->filetype = FIO_TYPE_PIPE;
	else
		f->filetype = FIO_TYPE_FILE;

	/* \\.\ is the device namespace in Windows, where every file is
	 * a block device */
	if (strncmp(f->file_name, "\\\\.\\", 4) == 0)
		f->filetype = FIO_TYPE_BD;

	if (!stat(f->file_name, &sb)) {
		if (S_ISBLK(sb.st_mode))
			f->filetype = FIO_TYPE_BD;
		else if (S_ISCHR(sb.st_mode))
			f->filetype = FIO_TYPE_CHAR;
		else if (S_ISFIFO(sb.st_mode))
			f->filetype = FIO_TYPE_PIPE;
	}
}

int add_file(struct thread_data *td, const char *fname)
{
	int cur_files = td->files_index;
	char file_name[PATH_MAX];
	struct fio_file *f;
	int len = 0;

	dprint(FD_FILE, "add file %s\n", fname);

	f = smalloc(sizeof(*f));
	if (!f) {
		log_err("fio: smalloc OOM\n");
		assert(0);
	}

	f->fd = -1;
	fio_file_reset(f);

	if (td->files_size <= td->files_index) {
		int new_size = td->o.nr_files + 1;

		dprint(FD_FILE, "resize file array to %d files\n", new_size);

		td->files = realloc(td->files, new_size * sizeof(f));
		td->files_size = new_size;
	}
	td->files[cur_files] = f;
	f->fileno = cur_files;

	/*
	 * init function, io engine may not be loaded yet
	 */
	if (td->io_ops && (td->io_ops->flags & FIO_DISKLESSIO))
		f->real_file_size = -1ULL;

	if (td->o.directory)
		len = sprintf(file_name, "%s/", td->o.directory);

	sprintf(file_name + len, "%s", fname);
	f->file_name = smalloc_strdup(file_name);
	if (!f->file_name) {
		log_err("fio: smalloc OOM\n");
		assert(0);
	}

	get_file_type(f);

	switch (td->o.file_lock_mode) {
	case FILE_LOCK_NONE:
		break;
	case FILE_LOCK_READWRITE:
		f->lock = fio_mutex_rw_init();
		break;
	case FILE_LOCK_EXCLUSIVE:
		f->lock = fio_mutex_init(FIO_MUTEX_UNLOCKED);
		break;
	default:
		log_err("fio: unknown lock mode: %d\n", td->o.file_lock_mode);
		assert(0);
	}

	td->files_index++;
	if (f->filetype == FIO_TYPE_FILE)
		td->nr_normal_files++;

	dprint(FD_FILE, "file %p \"%s\" added at %d\n", f, f->file_name,
							cur_files);

	return cur_files;
}

int add_file_exclusive(struct thread_data *td, const char *fname)
{
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i) {
		if (!strcmp(f->file_name, fname))
			return i;
	}

	return add_file(td, fname);
}

void get_file(struct fio_file *f)
{
	dprint(FD_FILE, "get file %s, ref=%d\n", f->file_name, f->references);
	assert(fio_file_open(f));
	f->references++;
}

int put_file(struct thread_data *td, struct fio_file *f)
{
	int f_ret = 0, ret = 0;

	dprint(FD_FILE, "put file %s, ref=%d\n", f->file_name, f->references);

	if (!fio_file_open(f)) {
		assert(f->fd == -1);
		return 0;
	}

	assert(f->references);
	if (--f->references)
		return 0;

	if (should_fsync(td) && td->o.fsync_on_close)
		f_ret = fsync(f->fd);

	if (td->io_ops->close_file)
		ret = td->io_ops->close_file(td, f);

	if (!ret)
		ret = f_ret;

	td->nr_open_files--;
	fio_file_clear_open(f);
	assert(f->fd == -1);
	return ret;
}

void lock_file(struct thread_data *td, struct fio_file *f, enum fio_ddir ddir)
{
	if (!f->lock || td->o.file_lock_mode == FILE_LOCK_NONE)
		return;

	if (f->lock_owner == td && f->lock_batch--)
		return;

	if (td->o.file_lock_mode == FILE_LOCK_READWRITE) {
		if (ddir == DDIR_READ)
			fio_mutex_down_read(f->lock);
		else
			fio_mutex_down_write(f->lock);
	} else if (td->o.file_lock_mode == FILE_LOCK_EXCLUSIVE)
		fio_mutex_down(f->lock);

	f->lock_owner = td;
	f->lock_batch = td->o.lockfile_batch;
	f->lock_ddir = ddir;
}

void unlock_file(struct thread_data *td, struct fio_file *f)
{
	if (!f->lock || td->o.file_lock_mode == FILE_LOCK_NONE)
		return;
	if (f->lock_batch)
		return;

	if (td->o.file_lock_mode == FILE_LOCK_READWRITE) {
		const int is_read = f->lock_ddir == DDIR_READ;
		int val = fio_mutex_getval(f->lock);

		if ((is_read && val == 1) || (!is_read && val == -1))
			f->lock_owner = NULL;

		if (is_read)
			fio_mutex_up_read(f->lock);
		else
			fio_mutex_up_write(f->lock);
	} else if (td->o.file_lock_mode == FILE_LOCK_EXCLUSIVE) {
		int val = fio_mutex_getval(f->lock);

		if (val == 0)
			f->lock_owner = NULL;

		fio_mutex_up(f->lock);
	}
}

void unlock_file_all(struct thread_data *td, struct fio_file *f)
{
	if (f->lock_owner != td)
		return;

	f->lock_batch = 0;
	unlock_file(td, f);
}

static int recurse_dir(struct thread_data *td, const char *dirname)
{
	struct dirent *dir;
	int ret = 0;
	DIR *D;

	D = opendir(dirname);
	if (!D) {
		char buf[FIO_VERROR_SIZE];

		snprintf(buf, FIO_VERROR_SIZE - 1, "opendir(%s)", dirname);
		td_verror(td, errno, buf);
		return 1;
	}

	while ((dir = readdir(D)) != NULL) {
		char full_path[PATH_MAX];
		struct stat sb;

		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
			continue;

		sprintf(full_path, "%s%s%s", dirname, FIO_OS_PATH_SEPARATOR, dir->d_name);

		if (lstat(full_path, &sb) == -1) {
			if (errno != ENOENT) {
				td_verror(td, errno, "stat");
				return 1;
			}
		}

		if (S_ISREG(sb.st_mode)) {
			add_file(td, full_path);
			td->o.nr_files++;
			continue;
		}
		if (!S_ISDIR(sb.st_mode))
			continue;

		ret = recurse_dir(td, full_path);
		if (ret)
			break;
	}

	closedir(D);
	return ret;
}

int add_dir_files(struct thread_data *td, const char *path)
{
	int ret = recurse_dir(td, path);

	if (!ret)
		log_info("fio: opendir added %d files\n", td->o.nr_files);

	return ret;
}

void dup_files(struct thread_data *td, struct thread_data *org)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_FILE, "dup files: %d\n", org->files_index);

	if (!org->files)
		return;

	td->files = malloc(org->files_index * sizeof(f));

	for_each_file(org, f, i) {
		struct fio_file *__f;

		__f = smalloc(sizeof(*__f));
		if (!__f) {
			log_err("fio: smalloc OOM\n");
			assert(0);
		}
		__f->fd = -1;
		fio_file_reset(__f);

		if (f->file_name) {
			__f->file_name = smalloc_strdup(f->file_name);
			if (!__f->file_name) {
				log_err("fio: smalloc OOM\n");
				assert(0);
			}

			__f->filetype = f->filetype;
		}

		td->files[i] = __f;
	}
}

/*
 * Returns the index that matches the filename, or -1 if not there
 */
int get_fileno(struct thread_data *td, const char *fname)
{
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i)
		if (!strcmp(f->file_name, fname))
			return i;

	return -1;
}

/*
 * For log usage, where we add/open/close files automatically
 */
void free_release_files(struct thread_data *td)
{
	close_files(td);
	td->files_index = 0;
	td->nr_normal_files = 0;
}
