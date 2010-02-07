#include <errno.h>
#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool one_to_many = true;
static size_t total_bytes = 0;
static const int PART_SIZE_BYTES = 100*1024*1024;
static char *full_file_name;

static int full_fd = -1;
static struct filepart
{
	char *name;
	size_t len;
} parts[100] = {};

/*
 * Look up a directory entry by name and get its attributes.
 *
 * Valid replies:
 *   fuse_reply_entry
 *   fuse_reply_err
 *
 * @param req request handle
 * @param parent inode number of the parent directory
 * @param name the name to look up
 */
void splitfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	if (one_to_many)
	{
		int i;

		for (i = 0; parts[i].name; i++)
		{
			if (strcmp(parts[i].name, name) == 0)
			{
				struct stat st =
				{
					.st_mode = S_IFREG | 0444,
					.st_ino = (ino_t)&parts[i],
					.st_size = parts[i].len,
				};
				struct fuse_entry_param ep =
				{
					.ino = (fuse_ino_t)&parts[i],
					.generation = 1,
					.attr = st,
					.attr_timeout = 0.0,
					.entry_timeout = 0.0,
				};

				fuse_reply_entry(req, &ep);
				return;
			}
		}
	} else if (strcmp(name, full_file_name) == 0) {
		struct stat st =
		{
			.st_mode = S_IFREG | 0444,
			.st_ino = 2,
			.st_size = total_bytes,
		};
		struct fuse_entry_param ep =
		{
			.ino = 2,
			.generation = 1,
			.attr = st,
			.attr_timeout = 0.0,
			.entry_timeout = 0.0,
		};

		fuse_reply_entry(req, &ep);
		return;
	}

	fuse_reply_err(req, ENOENT);
}

/*
 * Get file attributes
 *
 * Valid replies:
 *   fuse_reply_attr
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi for future use, currently always NULL
 */
void splitfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	if (ino == FUSE_ROOT_ID)
	{
		struct stat st =
		{
			.st_mode = S_IFDIR,
		};

		fuse_reply_attr(req, &st, 0.0);
	} else if (ino == 2) {
		struct stat st =
		{
			.st_mode = S_IFREG | 0444,
			.st_size = total_bytes,
		};

		fuse_reply_attr(req, &st, 0.0);
	} else {
		struct filepart *part = (struct filepart *)ino;
		struct stat st =
		{
			.st_mode = S_IFREG | 0444,
			.st_size = part->len,

		};

		fuse_reply_attr(req, &st, 0.0);
	}

}

void splitfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
	int err = 0;
	char *buf = malloc(size);

	if (!one_to_many)
	{
		if (off == 0)
		{
			struct stat st =
			{
				.st_mode = S_IFREG,
				.st_ino = 2,
			};
			fuse_add_direntry(req, buf, size, full_file_name, &st, off+1);
			fuse_reply_buf(req, buf,
				fuse_dirent_size(strlen(full_file_name)));
		} else {
			fuse_reply_buf(req, NULL, 0);
		}

		return;
	}

	int pos = 0;

	while (parts[off].name)
	{
		struct stat st =
		{
			.st_mode = S_IFREG,
			.st_ino = (ino_t)&parts[off],
		};

		int len = fuse_dirent_size(strlen(parts[off].name));
		if (pos + len > size)
			break;

		fuse_add_direntry(req, buf + pos, size - pos, parts[off].name, &st, off+1);
		off++;
		pos += len;
	}

	if (pos)
	{
		fuse_reply_buf(req, buf, pos);
		free (buf);
	} else {
		fuse_reply_buf(req, NULL, 0);
	}

	return;
out_err:
	if (buf)
		free(buf);
	fuse_reply_err (req, err);
}

/*
 * Rename a file
 *
 * Valid replies:
 *   fuse_reply_err
 *
 * @param req request handle
 * @param parent inode number of the old parent directory
 * @param name old name
 * @param newparent inode number of the new parent directory
 * @param newname new name
 */
void splitfs_rename(fuse_req_t req, fuse_ino_t parent,
	const char *name, fuse_ino_t newparent, const char *newname)
{
	if (one_to_many)
	{
		int i;
		for (i = 0; parts[i].name; i++)
		{
			if (strcmp(parts[i].name, name) == 0)
			{
				free(parts[i].name);
				parts[i].name = strdup(newname);
				fuse_reply_err(req, 0);
				return;
			}
		}

		fuse_reply_err(req, ENOENT);
	} else {
		free(full_file_name);
		full_file_name = strdup(newname);
		fuse_reply_err(req, 0);
	}
}

/*
 * Read data
 *
 * Read should send exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the file
 * has been opened in 'direct_io' mode, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * fi->fh will contain the value set by the open method, or will
 * be undefined if the open method didn't set any value.
 *
 * Valid replies:
 *   fuse_reply_buf
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param size number of bytes to read
 * @param off offset to read from
 * @param fi file information
 *
 * ATR: this will not be called when ino points to a directory.
 */
void splitfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
	if (one_to_many)
	{
		struct filepart *part = (struct filepart *)ino;
		int i;
		size_t offset = 0;
		int ret;
		char buf[size];

		for (i = 0; parts[i].name; i++)
		{
			if (part != &parts[i])
			{
				offset += parts[i].len;
				continue;
			}

			offset += off;
			ret = pread(full_fd, buf, size, offset);
			if (ret < 0)
				fuse_reply_err(req, errno);
			else
				fuse_reply_buf(req, buf, ret);
			return;
		}
	} else {
		int i;
		char buf[size];
		size_t total_read = 0;

		for (i = 0; parts[i].name; i++)
		{
			if (off > parts[i].len)
			{
				off -= parts[i].len;
				continue;
			}

			break;
		}

		if (!parts[i].name)
		{
			fuse_reply_err(req, ERANGE);
			return;
		}

		while (size > 0 && parts[i].name)
		{
			int fd = open(parts[i].name, O_RDONLY);
			if (fd < 0)
				goto out;

			int len = pread(fd, buf + total_read, size, off);
			if (len < 0)
			{
				perror("pread");
				goto out;
			} else if (len == size) {
				total_read += len;
				close(fd);
				goto out_ok;
			} else {
				i++;
				off = 0;
				size -= len;
				total_read += len;
				close(fd);
				continue;
			}
		}

		if (total_read > 0)
		{
out_ok:
			fuse_reply_buf(req, buf, total_read);
			return;
		}
out:
		fuse_reply_err(req, EIO);
	}
}

/*
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 *
 * Introduced in version 2.5
 *
 * Valid replies:
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param mask requested access mode
 */
void splitfs_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
	fuse_reply_err(req, 0);
}

struct fuse_lowlevel_ops splitfs_operations =
{
	.lookup = splitfs_lookup,
	.getattr = splitfs_getattr,
	.rename = splitfs_rename,
	.read = splitfs_read,
	.access = splitfs_access,
	.readdir = splitfs_readdir,
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(2, argv + (argc - 2));
	struct stat st;

	char *mountpoint;
	int foreground;
	int err = -1;

	int i = 0;

	if (argc == 3) {/*./oma file mnt/ */
		one_to_many = true;
	} else if (argc > 3) { /* ./oma file1 file2 ... mnt/ */
		one_to_many = false;
		full_file_name = strdup("full_file");
	} else {
		printf("Usage: \t%s <file_to_split> <mount_point>\n"
			"\t%s <part1> <part2> ... <mount_point\n",
			argv[0], argv[0]);
		return 1;
	}

	if (one_to_many)
	{
		/* One to many */
		full_fd = open(argv[1], O_RDONLY);
		if (full_fd < 0)
		{
			perror("open");
			return 1;
		}

		if (fstat(full_fd, &st) < 0)
		{
			perror("fstat");
			close(full_fd);
			return 1;
		}

		total_bytes = st.st_size;

		while (total_bytes > 0)
		{
			char buf[128];
			sprintf(buf, "part_%.3d", i + 1);
			parts[i].name = strdup(buf);

			if (total_bytes >= PART_SIZE_BYTES)
			{
				parts[i].len = PART_SIZE_BYTES;
				total_bytes -= PART_SIZE_BYTES;
			} else {
				parts[i].len = total_bytes;
				total_bytes = 0;
			}

			i++;
		}
	} else {
		/* Many to one */
		struct stat st;
		int i = 0;
		while (--argc > 1)
		{
			parts[i].name = strdup(canonicalize_file_name(*++argv));
			if (stat(parts[i].name, &st) < 0)
			{
				perror("stat");
				return 1;
			}

			parts[i].len = st.st_size;
			total_bytes += st.st_size;
			i++;
		}
	}

	if (fuse_parse_cmdline(&args, &mountpoint, NULL, &foreground) != -1)
	{
		fuse_opt_add_arg(&args, "-ofsname=splitfs");
		int fd = open(mountpoint, O_RDONLY);
		struct fuse_chan *fc = fuse_mount(mountpoint, &args);
		if (fc)
		{
			struct fuse_session *fs = fuse_lowlevel_new(&args,
				&splitfs_operations,
				sizeof(splitfs_operations),
				canonicalize_file_name("atrfs.conf"));

			if (fs)
			{
				if (fuse_set_signal_handlers(fs) != -1)
				{
					fuse_session_add_chan(fs, fc);
					fuse_daemonize(foreground);

					fchdir(fd);
					close(fd);
					err = fuse_session_loop(fs);
					fuse_remove_signal_handlers(fs);
					fuse_session_remove_chan(fc);
				}

				fuse_session_destroy(fs);
			}

			fuse_unmount(mountpoint, fc);
		}
	}

	fuse_opt_free_args(&args);
	return err ? 1 : 0;
}
