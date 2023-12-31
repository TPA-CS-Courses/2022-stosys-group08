#ifndef STOSYS_PROJECT_S2FILESYSTEM_COMMON_H
#define STOSYS_PROJECT_S2FILESYSTEM_COMMON_H

#include "rocksdb/env.h"
#include "rocksdb/io_status.h"
#include "rocksdb/file_system.h"
#include "rocksdb/status.h"
#include "S2FileSystem.h"
#include "my_thread_pool.h"
#include "utils.h"

#include <list>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <map>
#include <set>
#include <pthread.h>
#include <zns_device.h>

namespace ROCKSDB_NAMESPACE
{

    #define MAX_NAME_LENGTH                     32
    #define INODE_MAP_ENTRY_LENGTH              16
    #define FILE_ATTR_SIZE                      64
    #define map_contains(map, key)              (map.find(key) != map.end())
    #define addr_2_segment(addr)                ((addr) / S2FSSegment::Size() * S2FSSegment::Size())
    #define segment_2_addr(segm)                ((segm) * S2FSSegment::Size())
    #define addr_2_block(addr)                  (((addr) / S2FSBlock::Size()) % S2FSSegment::Size())
    #define addr_2_inseg_offset(addr)           ((addr) % S2FSSegment::Size())
    #define block_2_inseg_offset(block)         ((block) * S2FSBlock::Size())
    #define round_up(val, up_to)                (((val) / (up_to) + (((val) % (up_to)) == 0 ? 0 : 1)) * (up_to))

    // Need to be written back to disk
    static std::atomic_int64_t id_alloc(0);

    enum INodeType
    {
        ITYPE_UNKNOWN = 0,
        ITYPE_FILE_INODE = 1,
        ITYPE_FILE_DATA = 2,
        ITYPE_DIR_INODE = 4,
        ITYPE_DIR_DATA = 8
    };

    class S2FileSystem;

    class S2FSObject
    {
    private:
        pthread_rwlock_t _rwlock = PTHREAD_RWLOCK_INITIALIZER;
    public:
        static S2FileSystem *_fs;
        S2FSObject(){}
        virtual ~S2FSObject(){}

        virtual inline int ReadLock() { return pthread_rwlock_rdlock(&_rwlock); }
        virtual inline int WriteLock() { return pthread_rwlock_wrlock(&_rwlock); }
        virtual inline int Unlock() { return pthread_rwlock_unlock(&_rwlock); }

        virtual uint64_t Serialize(char *buffer) = 0;
        virtual uint64_t Deserialize(char *buffer) = 0;
    };

    class S2FSFileAttr : public S2FSObject
    {
    private:
        std::string _name;
        uint64_t _size;
        uint64_t _create_time;
        bool _is_dir;
        // Global offset of the file inode
        uint64_t _offset;
        uint64_t _inode_id;
        // ...
    public:
        S2FSFileAttr(){}
        ~S2FSFileAttr(){}

        uint64_t Serialize(char *buffer);
        uint64_t Deserialize(char *buffer);

        inline const std::string& Name()                        { return _name; }
        inline uint64_t Size()                                  { return _size; }
        inline uint64_t CreateTime()                            { return _create_time; }
        inline bool IsDir()                                     { return _is_dir; }
        inline uint64_t Offset()                                { return _offset; }
        inline uint64_t InodeID()                               { return _inode_id; }

        inline S2FSFileAttr* Name(const std::string& name)      { _name = name; return this; }
        inline S2FSFileAttr* Size(uint64_t size)                { _size = size; return this; }
        inline S2FSFileAttr* CreateTime(uint64_t create_time)   { _create_time = create_time; return this; }
        inline S2FSFileAttr* IsDir(bool is_dir)                 { _is_dir = is_dir; return this; }
        inline S2FSFileAttr* Offset(uint64_t offset)            { _offset = offset; return this; }
        inline S2FSFileAttr* InodeID(uint64_t inode_id)         { _inode_id = inode_id; return this; }
    };

    class S2FSBlock : public S2FSObject
    {
    private:
        // Only valid for ITYPE_INODE types. Indicating next INode global offset
        uint64_t _next;
        // Only valid for ITYPE_INODE types. Indicating previous INode global offset
        uint64_t _prev;
        // Only valid for ITYPE_DIR_INODE type.
        std::string _name;
        // Only valid for ITYPE_INODE types.
        uint64_t _id;
        INodeType _type;
        // Only valid for ITYPE_INODE types.
        // Global offsets of data blocks
        std::list<uint64_t> _offsets;
        // Only valid for ITYPE_DIR_DATA type.
        std::list<S2FSFileAttr*> _file_attrs;
        // Only valid for ITYPE_FILE_DATA type.
        char *_content;
        // Only valid for ITYPE_DATA types. For DIR_DATA, this mean the maximum number of entries in _file_attrs.
        uint64_t _content_size;
        uint64_t _segment_addr;
        uint64_t _global_offset;
        bool _loaded;

        uint64_t SerializeFileInode(char *buffer);
        uint64_t DeserializeFileInode(char *buffer);
        uint64_t SerializeDirInode(char *buffer);
        uint64_t DeserializeDirInode(char *buffer);
        uint64_t SerializeDirData(char *buffer);
        uint64_t DeserializeDirData(char *buffer);

    public:
        S2FSBlock(INodeType type, uint64_t segmeng_addr, uint64_t content_size, char *base) 
        : _id(id_alloc++), 
        _type(type),
        _next(0),
        _prev(0),
        _content_size(content_size),
        _segment_addr(segmeng_addr),
        _loaded(true)
        {
            if (type == ITYPE_FILE_DATA && content_size)
                _content = base;
            else
                _content = 0;
        }

        // Should followed by Deserialize()
        S2FSBlock()
        : _loaded(false),
        _id(0),
        _next(0),
        _prev(0),
        _content(0),
        _content_size(0) {}
        ~S2FSBlock();

        uint64_t Serialize(char *buffer);
        uint64_t Deserialize(char *buffer);
        // Call this with write lock acquired
        void LivenessCheck();

        inline void AddOffset(uint64_t offset)                  { _offsets.push_back(offset); }
        inline uint64_t Next()                                  { return _next; }
        inline void Next(uint64_t next)                         { _next = next; }
        inline uint64_t Prev()                                  { return _prev; }
        inline void Prev(uint64_t prev)                         { _prev = prev; }
        inline INodeType Type()                                 { return _type; }
        inline uint64_t ID()                                    { return _id; }
        inline const std::string& Name()                        { return _name; }
        inline S2FSBlock* Name(const std::string& name)         { _name = name; return this; }
        inline std::list<uint64_t>& Offsets()                   { return _offsets; }
        inline const std::list<S2FSFileAttr*>& FileAttrs()      { return _file_attrs; }
        inline void AddFileAttr(S2FSFileAttr &fa)               
        {
            S2FSFileAttr *_fa = new S2FSFileAttr;
            _fa->Name(fa.Name())
            ->CreateTime(fa.CreateTime())
            ->IsDir(fa.IsDir())
            ->InodeID(fa.InodeID())
            ->Offset(fa.Offset())
            ->Size(fa.Size());
            _file_attrs.push_back(_fa);
        }
        S2FSFileAttr* RemoveFileAttr(const std::string& name);
        inline char* Content()                                  { return _content; }
        inline void Content(char *content)                      { _content = content; }
        inline uint64_t ContentSize()                           { return _content_size; }
        inline void AddContentSize(uint64_t to_add)             { _content_size += to_add; }
        inline void SetContentSize(uint64_t to_set)             { _content_size = to_set; }
        inline void SegmentAddr(uint64_t addr)                  { _segment_addr = addr; }
        inline uint64_t SegmentAddr()                           { return _segment_addr; }
        inline void Loaded(bool loaded)                         { _loaded = loaded; }
        inline bool Loaded()                                    { return _loaded; }

        int ChainReadLock();
        int ChainWriteLock();
        int ChainUnlock();
        uint64_t GlobalOffset()                                 { return _global_offset; }
        void GlobalOffset(uint64_t global_offset)               { _global_offset = global_offset; }
        S2FSBlock *DirectoryLookUp(std::string &name);
        int DataAppend(const char *data, uint64_t len);
        int DirectoryAppend(S2FSFileAttr& fa);
        int Read(char *buf, uint64_t n, uint64_t offset, uint64_t buf_offset);
        int ReadChildren(std::vector<std::string> *list);
        void RenameChild(const std::string &src, const std::string &target);
        void FreeChild(const std::string &name);
        void AddFileSize(uint64_t child_id, uint64_t size);
        uint64_t GetFileSize(const std::string &name);
        uint64_t GetFileSize(uint64_t child_id);
        // No locking inside
        int Offload();
        uint64_t ActualSize();

        static uint64_t Size();
        static uint64_t MaxDataSize(INodeType type);
    };

    class S2FSSegment : public S2FSObject
    {
    private:
        // should be aligned to zone_no
        uint64_t _addr_start;
        /* k: inode id, v: in-segment offset*/
        std::unordered_map<uint64_t, uint64_t> _inode_map;
        /* k: name, v: inode id*/
        std::unordered_map<std::string, uint32_t> _name_2_inode;
        /* k: in-segment offset, v: block*/
        std::map<uint64_t, S2FSBlock*> _blocks;
        uint32_t _reserve_for_inode;
        uint64_t _cur_size;
        char *_buffer;
        uint64_t _last_modify;
        bool _loaded;
    public:
        S2FSSegment(uint64_t addr);
        ~S2FSSegment();

        void Preload(char *buffer);
        uint64_t Serialize(char *buffer);
        uint64_t Deserialize(char *buffer);

        // Get block by in-segment offset
        // No lock ops in this function, not thread safe
        // Make sure to use WriteLock() before calling this
        S2FSBlock *GetBlockByOffset(uint64_t offset);
        S2FSBlock *GetBlockByID(uint64_t id);
        // Get inode block by name
        // No lock ops in this function, not thread safe
        // Make sure to use WriteLock() before calling this
        S2FSBlock *LookUp(const std::string &name);
        int64_t AllocateNew(const std::string &name, INodeType type, S2FSBlock **res, S2FSBlock *parent_dir);
        // Should call WriteLock() for inode_id before calling this
        int64_t AllocateData(uint64_t inode_id, INodeType type, const char *data, uint64_t size, S2FSBlock **res);
        // Equivalent to delete
        int Free(uint64_t inode_id);
        int RemoveINode(uint64_t inode_id);
        void OnRename(const std::string &src, const std::string &target);
        // int Write();

        // No locking inside
        int Flush();
        // No locking inside
        int Flush(uint64_t in_seg_off);
        // No locking inside
        int Offload();
        int OnGC();

        inline uint64_t CurSize() { return _cur_size; }
        inline void AddSize(uint64_t to_add) { _cur_size += to_add; }
        inline uint64_t Addr() { return _addr_start; }
        inline bool IsEmpty() { return _inode_map.empty(); }
        inline char *Buffer() { return _buffer; }
        inline uint64_t LastModify() { return _last_modify; }
        inline void LastModify(uint64_t last_modify) { _last_modify = last_modify; }
        inline void Loaded(bool loaded)                         { _loaded = loaded; }
        inline bool Loaded()                                    { return _loaded; }

        static uint64_t Size();
    };
}

#endif