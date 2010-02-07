#!/usr/bin/env python
import sys, os, fuse, errno, stat

class File():
    def __init__(self, filename):
        self.filename = filename
    def __repr__(self):
        return "<File: %s, %d>" % (self.filename, self.size())
    def size(self):
        return os.stat(self.filename).st_size
    def read(self, pos, length):
        f = file(self.filename)
        f.seek(pos)
        data = f.read(length)
        f.close()
        return data

class BigFile():
    def __init__(self, filelist):
        self.files = filelist
    def __repr__(self):
        return "<BigFile [%s], %d>" % (" ".join([file.filename for file in self.files]),
                                       self.size())
    def size(self):
        return reduce(lambda x,y: x+y, [file.size() for file in self.files], 0)
    def read(self, pos, length):
        lens = [0]
        p = 0
        for f in self.files:
            p = p + f.size()
            lens.append(p) # lens = 0 len0 len0+len1 len0+len1+len2 ... size

        end = min(self.size(), pos + length)
        parts = []
        i = 1
        while pos < end:
            if pos >= lens[i-1] and pos < lens[i]:
                sz = min(length, self.files[i-1].size())
                parts.append(self.files[i-1].read(pos - lens[i-1], sz))
                pos += sz
                length -= sz
            else:
                i = i + 1
        return "".join(parts)

class SplitStat(fuse.Stat):
	def __init__(self):
		self.st_mode = 0
		self.st_ino = 0
		self.st_dev = 0
		self.st_nlink = 0
		self.st_uid = 0
		self.st_gid = 0
		self.st_size = 0
		self.st_atime = 0
		self.st_mtime = 0
		self.st_ctime = 0

class SplitFS(fuse.Fuse):
    def __init__(self):
        fuse.Fuse.__init__(self)

    def getattr(self, path):
        st = SplitStat()
        if path == "/":
            st.st_mode = stat.S_IFDIR | 0444
        elif path != "/bigfile":
            return -errno.ENOENT
        else:
            stbuf = os.lstat(big.files[0].filename)
            st.st_mode = stat.S_IFREG | 0444
            st.st_size = big.size()
            st.st_mtime = stbuf.st_mtime
            st.st_nlink = 1
        return st

    def readdir(self, path, offset):
        for name in ["bigfile"]:
            yield fuse.Direntry(name)

    def truncate(self, path, len):
        return 0

    def read(self, size, offset):
        return big.read(offset, size)

    def lookup(self, path):
        if path != "/bigfile":
            return -errno.ENOENT
        return self

fuse.fuse_python_api = (0,2)

server = SplitFS()
server.parse()
big = BigFile([File(filename) for filename in sys.argv[1:]])
server.main()
