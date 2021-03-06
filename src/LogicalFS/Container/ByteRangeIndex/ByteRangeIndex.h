/*
 * ByteRangeIndex.h  all ByteRangeIndex indexing structures
 */
class ByteRangeIndex;   /* forward decl the main class */

#include <deque>        /* used for index read tasks */

/*
 * locking: we try and keep all the locking at the entry point
 * functions in ByteRangeIndex.cpp (the one exception is
 * index_stream in BRI_ADIO.cpp).  there is also some internal
 * locking in BRI_Reader for the parallel index read.
 *
 * byte-order/structure padding: all native, so may not be
 * portable across architecture platforms.
 */

/*
 * HostEntry: this is the on-disk format for an index dropping file
 *
 * index dropping files are named: 
 *      dropping.index.SEC.USEC.HOST.PID
 * 
 * the sec/usec/host is set when the index dropping is opened.  the
 * pid is the pid of opener (or in MPI it is the rank).  note that is
 * possible for a single index dropping file to point at more than one
 * data dropping file because of the "id" pid field.  to get the data
 * dropping filename, we get a HostEntry record and the index dropping
 * filename.  the data dropping file is:
 *
 *     dropping.data.SEC.USEC.HOST.PID
 *
 * where SEC, USEC, and HOST match the index dropping filename and the
 * PID is from a HostEntry within that index dropping.
 */
class HostEntry
{
 public:
    /* constructors */
    HostEntry();
    HostEntry(off_t o, size_t s, pid_t p);
    HostEntry(const HostEntry& copy);
    bool contains (off_t) const;
    bool splittable (off_t) const;
    bool overlap(const HostEntry&);
    bool follows(const HostEntry&);
    bool preceeds(const HostEntry&);
    bool abut (const HostEntry&);
    off_t logical_tail() const;
    
 protected:
    off_t  logical_offset;    /* logical offset in container file */
    off_t  physical_offset;   /* physical offset in data dropping file */
    size_t length;            /* number of data bytes, can be zero */
    double begin_timestamp;   /* time write started */
    double end_timestamp;     /* time write completed */
    pid_t  id;                /* id (to locate data dropping) */

    friend class ByteRangeIndex;
};

/*
 * ContainerEntry: this is the in-memory data structure used to
 * store a container's index that we've read in.  it is also used
 * in the on-disk global index file (if we flatten the file).
 * the original_chunk is the id from the on-disk index dropping
 * (so we can rewrite it if needed).  the id is the chunk file #.
 * 
 * the on disk format for global.index is:
 *   <#ContainerEntry records>
 *   <ContainerEntry1> <ContainerEntry2> ... <ContainerEntryN>
 *   <chunk path 1>\n <chunk path 2>\n ... <chunk path M>\n
 * 
 * the chunk paths need to be full physical path specs, though we
 * allow paths that start with "/" to stand in for "posix:"
 */
class ContainerEntry : HostEntry
{
 public:
    /* split in two at offset, "this" becomes back, return front */
    ContainerEntry split(off_t);
    bool preceeds(const ContainerEntry&);
    bool follows(const ContainerEntry&);
    bool abut(const ContainerEntry&);
    bool mergable(const ContainerEntry&);
    bool older_than(const ContainerEntry&);
    
 protected:
    /* track orig chunk for rewriting index (e.g. truncate op) */
    pid_t original_chunk;

    friend ostream& operator <<(ostream&, const ContainerEntry&);
    friend class ByteRangeIndex;
};

/*
 * ChunkFile: a way to associate an int with a local file so that
 * we only need an int in the aggregated index (saves space).
 */
typedef struct {
    string bpath;                 /* bpath to data dropping */
    struct plfs_backend *backend; /* backend data dropping lives on */
    IOSHandle *fh;                /* NULL if not currently open */
} ChunkFile;

/*
 * IndexFileInfo: info on one index dropping file in a container hostdir
 *
 * this is used to generate a list of index dropping files in a 
 * specific hostdir.  so if you know /m/plfs/dir1/dir2/file has a 
 * hostdir "hostdir.5" on backend /mnt/panfs0, then you know this path:
 *   /mnt/panfs0/dir1/dir2/file/hostdir.5/
 * on the backend and you want to know all the index dropping files 
 * in there you can just return a list of <timestamp,hostname,id> records,
 * one per index dropping file and that can be appended to the above
 * path to get the full path.
 * 
 * appears to be used for MPI only when doing the MPI parallel index
 * read across all the nodes.
 */
class IndexFileInfo {
 public:
    plfs_error_t listToStream(vector<IndexFileInfo> &list, int *bytes,
                              void **ret_buf);
    vector<IndexFileInfo> streamToList(void *addr);

    double timestamp;
    string hostname;
    pid_t  id;
};

/**
 * ByteRangeIndex: ByteRange instance of PLFS container index
 */
class ByteRangeIndex : public ContainerIndex {
public:
    ByteRangeIndex(PlfsMount *);    /* constructor */
    ~ByteRangeIndex();              /* destructor */

    const char *index_name(void) { return("ByteRange"); };

    plfs_error_t index_open(Container_OpenFile *cof, int rw_flags, 
                            Plfs_open_opt *open_opt);
    plfs_error_t index_close(Container_OpenFile *cof, off_t *lastoffp,
                             size_t *tbytesp, Plfs_close_opt *close_opt);
    plfs_error_t index_add(Container_OpenFile *cof, size_t nbytes,
                           off_t offset, pid_t pid, off_t physoff,
                           double begin, double end);
    plfs_error_t index_sync(Container_OpenFile *cof);
    plfs_error_t index_query(Container_OpenFile *cof, off_t input_offset,
                             size_t input_length, 
                             list<index_record> &result);
    plfs_error_t index_truncate(Container_OpenFile *cof, off_t offset);
    plfs_error_t index_closing_wdrop(Container_OpenFile *cof,
                                     string ts, pid_t pid, const char *fn);
    plfs_error_t index_new_wdrop(Container_OpenFile *cof,
                                 string ts, pid_t pid, const char *fn);
    plfs_error_t index_optimize(Container_OpenFile *cof);
    plfs_error_t index_info(off_t &lastoff, off_t &bwritten);

    plfs_error_t index_droppings_getattrsize(struct plfs_physpathinfo *ppip,
                                             struct stat *stbuf,
                                             set<string> *openset,
                                             set<string> *metaset);

    plfs_error_t index_droppings_rename(struct plfs_physpathinfo *src,
                  struct plfs_physpathinfo *dst);
    plfs_error_t index_droppings_trunc(struct plfs_physpathinfo *ppip,
                                       off_t offset);
    plfs_error_t index_droppings_unlink(struct plfs_physpathinfo *ppip);
    plfs_error_t index_droppings_zero(struct plfs_physpathinfo *ppip);

    /*
     * XXX: this public functions are for the MPI optimizations.
     * they get directly called from container_adio.cpp, so they
     * need to be public.
     */
    static plfs_error_t hostdir_rddir(void **index_stream, char *targets,
                                      int rank, char *top_level,
                                      PlfsMount *mnt,
                                      struct plfs_backend *canback,
                                      int *index_sz);
    static plfs_error_t hostdir_zero_rddir(void **entries, const char *path,
                                           int /* rank */, PlfsMount *mnt,
                                           struct plfs_backend *canback,
                                           int *ret_size);
    static plfs_error_t parindex_read(int rank, int ranks_per_comm,
                                      void *index_files, void **index_stream,
                                      char *top_level, int *ret_index_size);
    static int parindexread_merge(const char *path, char *index_streams,
                                  int *index_sizes, int procs,
                                  void **index_stream);
    static plfs_error_t index_stream(Container_OpenFile **pfd, char **buffer,
                                     int *ret_index_sz); 

    friend ostream& operator <<(ostream&, const ByteRangeIndex&);

 private:
    static plfs_error_t insert_entry(map<off_t,ContainerEntry> &idxout,
                                     off_t *eof_trk, off_t *bbytes,
                                     ContainerEntry *add);
    static plfs_error_t insert_overlapped(map<off_t,ContainerEntry> &idxout,
                                          ContainerEntry& g_entry,
                                    pair< map<off_t,ContainerEntry>::iterator, 
                                    bool > &insert_ret );
    static plfs_error_t merge_dropping(map<off_t,ContainerEntry> &idxout,
                                       vector<ChunkFile> &cmapout,
                                       off_t *eof_trk, off_t *bbytes,
                                       string dropbpath,
                                       struct plfs_backend *dropback);
    static plfs_error_t merge_idx(map<off_t,ContainerEntry> &idxout,
                                  vector<ChunkFile> &cmapout, 
                                  off_t *eof_trk, off_t *bbytes,
                                  map<off_t,ContainerEntry> &idxin,
                                  vector<ChunkFile> &cmapin);
    static plfs_error_t reader(deque<struct plfs_pathback> &idrops,
                               ByteRangeIndex *bri, int rank);
    static void *reader_indexer_thread(void *va);
    static plfs_error_t collectIndices(const string& phys,
                                       struct plfs_backend *back,
                                       vector<plfs_pathback> &indices,
                                       bool full_path);
    static plfs_error_t aggregateIndices(const string& path,
                                         struct plfs_backend *canback,
                                         ByteRangeIndex *bri,
                                         bool uniform_restart,
                                         pid_t uniform_rank);
    static plfs_error_t populateIndex(const string& path,
                                      struct plfs_backend *canback,
                                      ByteRangeIndex *bri, bool use_global,
                                      bool uniform_restart,
                                      pid_t uniform_rank);

    plfs_error_t global_from_stream(void *addr);
    plfs_error_t global_to_stream(void **buffer, size_t *length);
    plfs_error_t global_to_file(IOSHandle *fh, struct plfs_backend *canback);

    plfs_error_t query_helper(Container_OpenFile *cof, off_t input_offset,
                              size_t input_length, 
                              list<index_record> &result);
    plfs_error_t query_helper_getrec(Container_OpenFile *cof, off_t ptr,
                                     size_t len, index_record *irp);
    void query_helper_load_irec(off_t ptr, index_record *irp,
                                map<off_t,ContainerEntry>::iterator qitr,
                                bool at_end);
    plfs_error_t flush_writebuf(void);
    static plfs_error_t scan_idropping(string dropbpath,
                                       struct plfs_backend *dropback,
                                       off_t *ep, off_t *bp);
    plfs_error_t trunc_edit_nz(struct plfs_physpathinfo *ppip, off_t nzo,
                               string openidrop);
    static void trunc_map(map<off_t,ContainerEntry> &mymap, off_t nzo);
    static plfs_error_t trunc_writemap(map<off_t,ContainerEntry> &mymap,
                                       IOSHandle *fh);

    static plfs_error_t indices_from_subdir(string path, PlfsMount *cmnt,
                                            struct plfs_backend *canback,
                                            struct plfs_backend **ibackp,
                                            vector<IndexFileInfo> &indices);
    static ByteRangeIndex parAggregateIndices(vector<IndexFileInfo>& indexlist,
                                              int rank, int ranks_per_comm,
                                              string path,
                                              struct plfs_backend *backend);


    pthread_mutex_t bri_mutex;       /* to lock this data structure */
    bool isopen;                     /* true if index is open */
    int brimode;                     /* isopen: O_RDONLY, O_WRONLY, O_RDWR */
    off_t eof_tracker;               /* RD/RDWR: the actual EOF */
                                     /* WR: max ending offset of our writes */

    /* data structures for the write side */
    vector<HostEntry> writebuf;      /* buffer write records here */
    int write_count;                 /* #write ops for this open */
    off_t write_bytes;               /* #bytes written for this open */
    IOSHandle *iwritefh;             /* where to write index to */
    struct plfs_backend *iwriteback; /* backend index is on */

    /* data structures for the read side */
    map<off_t,ContainerEntry> idx;   /* global index (aggregated) */
    vector<ChunkFile> chunk_map;     /* filenames for idx */
    /* note: next avail chunk_id is chunk_map.size() */
    off_t backing_bytes;             /* see below */
    /*
     * backing_bytes includes overwrites.  this field is only used
     * internally (it is easy to track) -- e.g. as an arg to functions
     * like merge_dropping/merge_idx.  the merge function needs that
     * arg for index_droppings_getattrsize(), so it is useful to keep
     * this around.  note that shrinking/truncating a file to a
     * nonzero size doesn't remove any data droppings (so the
     * backing_bytes can be larger than the actual file size due to
     * either overwrites or truncates).
     */
};

