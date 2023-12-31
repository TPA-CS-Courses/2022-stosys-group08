/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#ifndef STOSYS_PROJECT_S2FILESYSTEM_H
#define STOSYS_PROJECT_S2FILESYSTEM_H

#include "rocksdb/env.h"
#include "rocksdb/io_status.h"
#include "rocksdb/file_system.h"
#include "rocksdb/status.h"
#include "S2FSImpl.h"
#include "S2FSCommon.h"
#include "my_thread_pool.h"

#include <zns_device.h>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <list>

namespace ROCKSDB_NAMESPACE
{

#define CACHE_SEG_THRESHOLD 4

    class S2FSObject;
    class S2FSBlock;
    class S2FSSegment;
    class S2FileSystem;

    struct GCWrapperArg
    {
        S2FileSystem *fs;
        uint64_t seg_start;
        uint64_t seg_num;
    };

    class S2FileSystem : public FileSystem
    {
    public:
        // No copying allowed
        S2FileSystem(std::string uri, bool debug);
        S2FileSystem(const S2FileSystem &) = delete;
        virtual ~S2FileSystem();

        IOStatus IsDirectory(const std::string &, const IOOptions &options, bool *is_dir, IODebugContext *) override;

        IOStatus
        NewSequentialFile(const std::string &fname, const FileOptions &file_opts,
                          std::unique_ptr<FSSequentialFile> *result,
                          IODebugContext *dbg);

        IOStatus
        NewRandomAccessFile(const std::string &fname, const FileOptions &file_opts,
                            std::unique_ptr<FSRandomAccessFile> *result,
                            IODebugContext *dbg);

        IOStatus
        NewWritableFile(const std::string &fname, const FileOptions &file_opts, std::unique_ptr<FSWritableFile> *result,
                        IODebugContext *dbg);

        IOStatus
        ReopenWritableFile(const std::string &, const FileOptions &, std::unique_ptr<FSWritableFile> *,
                           IODebugContext *);

        IOStatus
        NewRandomRWFile(const std::string &, const FileOptions &, std::unique_ptr<FSRandomRWFile> *, IODebugContext *);

        IOStatus NewMemoryMappedFileBuffer(const std::string &, std::unique_ptr<MemoryMappedFileBuffer> *);

        IOStatus NewDirectory(const std::string &name, const IOOptions &io_opts, std::unique_ptr<FSDirectory> *result,
                              IODebugContext *dbg);

        const char *Name() const;

        IOStatus GetFreeSpace(const std::string &, const IOOptions &, uint64_t *, IODebugContext *);

        IOStatus Truncate(const std::string &, size_t, const IOOptions &, IODebugContext *);

        IOStatus CreateDir(const std::string &dirname, const IOOptions &options, IODebugContext *dbg);

        IOStatus CreateDirIfMissing(const std::string &dirname, const IOOptions &options, IODebugContext *dbg);

        IOStatus
        GetFileSize(const std::string &fname, const IOOptions &options, uint64_t *file_size, IODebugContext *dbg);

        IOStatus DeleteDir(const std::string &dirname, const IOOptions &options, IODebugContext *dbg);

        IOStatus
        GetFileModificationTime(const std::string &fname, const IOOptions &options, uint64_t *file_mtime,
                                IODebugContext *dbg);

        IOStatus
        GetAbsolutePath(const std::string &db_path, const IOOptions &options, std::string *output_path,
                        IODebugContext *dbg);

        IOStatus DeleteFile(const std::string &fname,
                            const IOOptions &options,
                            IODebugContext *dbg);

        IOStatus
        NewLogger(const std::string &fname, const IOOptions &io_opts, std::shared_ptr<Logger> *result,
                  IODebugContext *dbg);

        IOStatus GetTestDirectory(const IOOptions &options, std::string *path, IODebugContext *dbg);

        IOStatus UnlockFile(FileLock *lock, const IOOptions &options, IODebugContext *dbg);

        IOStatus LockFile(const std::string &fname, const IOOptions &options, FileLock **lock, IODebugContext *dbg);

        IOStatus AreFilesSame(const std::string &, const std::string &, const IOOptions &, bool *, IODebugContext *);

        IOStatus NumFileLinks(const std::string &, const IOOptions &, uint64_t *, IODebugContext *);

        IOStatus LinkFile(const std::string &, const std::string &, const IOOptions &, IODebugContext *);

        IOStatus
        RenameFile(const std::string &src, const std::string &target, const IOOptions &options, IODebugContext *dbg);

        IOStatus
        GetChildrenFileAttributes(const std::string &dir, const IOOptions &options, std::vector<FileAttributes> *result,
                                  IODebugContext *dbg);

        IOStatus
        GetChildren(const std::string &dir, const IOOptions &options, std::vector<std::string> *result,
                    IODebugContext *dbg);

        IOStatus FileExists(const std::string &fname, const IOOptions &options, IODebugContext *dbg);

        IOStatus ReuseWritableFile(const std::string &fname, const std::string &old_fname, const FileOptions &file_opts,
                                   std::unique_ptr<FSWritableFile> *result, IODebugContext *dbg);

        struct user_zns_device *_zns_dev;
        struct zns_device_extra_info *_zns_dev_ex;

        S2FSSegment *ReadSegment(uint64_t from);
        S2FSSegment *FindNonFullSegment();
        bool DirectoryLookUp(std::string &name, S2FSBlock *parent, bool set_parent, S2FSBlock **res);
        // Read a segment regardless of whether it is in cache or not
        S2FSSegment *LoadSegmentFromDisk();
        // Read a segment regardless of whether it is in cache or not
        S2FSSegment *LoadSegmentFromDisk(uint64_t from);
        my_thread_pool *_thread_pool;

    private:
        std::string _uri;
        const std::string _fs_delimiter = "/";
        std::unordered_map<uint64_t, S2FSSegment *> _cache;
        uint64_t _wp_end;

        std::string get_seq_id();
        std::atomic<int> _seq_id{};
        std::string _name;
        std::stringstream _ss;

        struct GCWrapperArg *_gc_args[4];

        // Set res to the target file inode or its parent dir inode, depending on the set_parent flag
        IOStatus _FileExists(const std::string &fname, bool set_parent, S2FSBlock **res);
    };
}

#endif // STOSYS_PROJECT_S2FILESYSTEM_H
