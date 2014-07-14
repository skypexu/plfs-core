/*
 * ByteRangeIndex.cpp  byte-range index code
 */

#include "plfs_private.h"
#include "Container.h"
#include "ContainerIndex.h"
#include "ContainerOpenFile.h"
#include "ByteRangeIndex.h"

/*
 * small private ByteRangeIndex API functions (longer fns get farmed
 * out to their own files).
 */

/**
 * ByteRangeIndex::flush_writebuf: flush out the write buffer to the
 * backing dropping.   the BRI should already be locked by the caller.
 *
 * @return PLFS_SUCCESS or an error code
 */
plfs_error_t
ByteRangeIndex::flush_writebuf() {
    plfs_error_t ret = PLFS_SUCCESS;
    size_t len;
    void *start;
    ssize_t bytes;

    len = this->writebuf.size();

    /* iwritefh check is just for sanity, should be non-null */
    if (len && this->iwritefh != NULL) {
        /* note: c++ vectors are guaranteed to be contiguous */
        len = len * sizeof(HostEntry);
        start = &(this->writebuf.front());

        ret = Util::Writen(start, len, this->iwritefh, &bytes);
        if (ret != PLFS_SUCCESS) {
            mlog(IDX_DRARE, "%s: failed to write fh %p: %s",
                 __FUNCTION__, this->iwritefh, strplfserr(ret));
        }

        this->writebuf.clear();   /* clear buffer */
    }

    return(ret);
}


/* public ContainerIndex API functions */

/**
 * ByteRangeIndex::ByteRangeIndex: constructor
 */
ByteRangeIndex::ByteRangeIndex(PlfsMount *) {
    pthread_mutex_init(&this->bri_mutex, NULL);
    this->isopen = false;
    this->brimode = -1;          /* an invalid value */
    this->eof_tracker = 0;
    this->write_count = 0;
    this->write_bytes = 0;
    this->iwritefh = NULL;
    this->iwriteback = NULL;
    this->nchunks = 0;
    this->backing_bytes = 0;
    /* init'd by C++: writebuf, idx, chunk_map */
}

/**
 * ByteRangeIndex::ByteRangeIndex: destructor
 */
ByteRangeIndex::~ByteRangeIndex() {
    pthread_mutex_destroy(&this->bri_mutex);
};

/**
 * ByteRangeIndex::index_open: establish an open index for open file
 *
 * @param cof state for the open file
 * @param open_flags the mode (RDONLY, WRONLY, or RDWR)
 * @param open_opt open options (e.g. for MPI opts)
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_open(Container_OpenFile *cof, int open_flags, 
                           Plfs_open_opt *open_opt) {

    plfs_error_t ret = PLFS_SUCCESS;
    bool urestart;
    pid_t upid;
    
    Util::MutexLock(&this->bri_mutex, __FUNCTION__);

    /*
     * for writeable indexes, the previous version of the code (prior
     * to ContainerIndex interface) would create the index dropping
     * here if !lazy_droppings.  we no longer do it here, instead we
     * create the dropping (if needed) in the this->index_new_wdrop()
     * call.
     */

    /*
     * readable index requires us to load the droppings into memory.
     * MPI code may pass us an index on open in open_opt.
     */
    if (open_flags != O_WRONLY) {

        if (open_opt && open_opt->index_stream != NULL) {

            ret = this->global_from_stream(open_opt->index_stream);

        } else {

            if (open_opt) {
                urestart = open_opt->uniform_restart_enable;
                upid = open_opt->uniform_restart_rank;
            } else {
                urestart = false;
                upid = 0;     /* rather than garbage */
            }
            ret = ByteRangeIndex::populateIndex(cof->pathcpy.canbpath,
                                                cof->pathcpy.canback,
                                                this, true, urestart, upid);

        }
            
        /*
         * we don't keep index in memory for RDWR.  instead, we reread
         * the index on each read operation.  this makes PLFS slow but
         * more correct in the RDWR case...
         */
        if (ret == PLFS_SUCCESS && open_flags == O_RDWR) {
            this->idx.clear();
            this->chunk_map.clear();
            this->nchunks = 0;
        }
    }
    
    if (ret == PLFS_SUCCESS) {
        this->isopen = true;
        this->brimode = open_flags;
    }

    Util::MutexUnlock(&this->bri_mutex, __FUNCTION__);
    return(ret);
}

/**
 * ByteRangeIndex::index_close: close off an open index
 *
 * @param cof the open file we belong to
 * @param lastoffp where to put last offset for metadata dropping (NULL ok)
 * @param tbytesp where to put total bytes for metadata dropping (NULL ok)
 * @param close_opt close options (e.g. for MPI opts)
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_close(Container_OpenFile *cof, off_t *lastoffp,
                            size_t *tbytesp, Plfs_close_opt *close_opt) {

    plfs_error_t ret = PLFS_SUCCESS;
    plfs_error_t rv;

    Util::MutexLock(&this->bri_mutex, __FUNCTION__);

    if (!this->isopen) {    /* already closed, nothing to do */
        goto done;
    }

    /*
     * lastoffp and tbytep return values only used when closing a
     * writeable container (to do the meta dropping), but go ahead and
     * return something meaningful on a O_RDONLY container just to be
     * safe.  for total bytes, we give back backing_bytes when
     * O_RDONLY since write_bytes will always be zero.  that RDONLY
     * wreturn value isn't used (though we could log it for debugging
     * info).
     */
    if (lastoffp != NULL) {
        *lastoffp = this->eof_tracker;
    }
    if (tbytesp != NULL) {
        *tbytesp = (this->brimode != O_RDONLY) ? this->write_bytes
             : this->backing_bytes;
    }
    
    /* flush out any cached write index records and shutdown write side */
    if (this->brimode != O_RDONLY) {   /* writeable? */
        ret = this->flush_writebuf();  /* clears this->writebuf */
        this->write_count = 0;
        this->write_bytes = 0;
        if (this->iwritefh != NULL) {
            rv = this->iwriteback->store->Close(this->iwritefh);
            if (ret == PLFS_SUCCESS && rv != PLFS_SUCCESS) {
                ret = rv;   /* bubble this error up */
            }
        }
        this->iwriteback = NULL;
    }

    /* free read-side memory */
    if (this->brimode != O_WRONLY) {
        this->idx.clear();
        this->chunk_map.clear();
        this->nchunks = 0;
        this->backing_bytes = 0;
    }

    /* let the eof_tracker persist for now */
    this->brimode = -1;
    this->isopen = false;
    Util::MutexUnlock(&this->bri_mutex, __FUNCTION__);

 done:
    return(ret);
}

/**
 * ByteRangeIndex::index_add: add an index record to a writeable index
 *
 * @param cof the open file
 * @param nbytes number of bytes we wrote
 * @param offset the logical offset of the record
 * @param pid the pid doing the writing
 * @param physoffset the physical offset in the data dropping of the data
 * @param begin the start timestamp
 * @param end the end timestamp
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_add(Container_OpenFile *cof, size_t nbytes,
                          off_t offset, pid_t pid, off_t physoffset,
                          double begin, double end) {

    plfs_error_t ret = PLFS_SUCCESS;
    HostEntry newent;

    newent.logical_offset = offset;
    newent.physical_offset = physoffset;
    newent.length = nbytes;
    newent.begin_timestamp = begin;
    newent.end_timestamp = end;
    newent.id = pid;

    Util::MutexLock(&this->bri_mutex, __FUNCTION__);
    this->writebuf.push_back(newent);
    this->write_count++;
    this->write_bytes += nbytes;

    /* XXX: carried over hardwired 1024 from old code */
    if ((this->write_count % 1024) == 0) {
        ret = this->flush_writebuf();
    }
    
    Util::MutexUnlock(&this->bri_mutex, __FUNCTION__);

    return(ret);
}

/**
 * ByteRangeIndex::index_sync push unwritten records to backing iostore
 *
 * @param cof the open file
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_sync(Container_OpenFile *cof) {

    plfs_error_t ret;

    Util::MutexLock(&this->bri_mutex, __FUNCTION__);

    ret = this->flush_writebuf();
    
    Util::MutexUnlock(&this->bri_mutex, __FUNCTION__);

    return(ret);
}

/*
 * ByteRangeIndex::index_query - query index for index records
 *
 * index should be open in either RDONLY or RDWR (callers shold have
 * checked all this already).
 *
 * @param cof the open file
 * @param input_offset the starting offset
 * @param input_length the length we are interested in
 * @param result the resulting records go here
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_query(Container_OpenFile *cof, off_t input_offset,
                             size_t input_length, 
                            list<index_record> &result) {

    plfs_error_t ret = PLFS_SUCCESS;
    ByteRangeIndex *target = NULL;

    /* these should never fire... */
    assert(cof->openflags != O_WRONLY);
    assert(this->isopen);

    /*
     * for RDWR we have to generate a temporary read index for the
     * read operation (this is one reason why PLFS container RDWR
     * performance isn't very good).
     */
    if (cof->openflags == O_RDWR) {

        target = new ByteRangeIndex(cof->pathcpy.mnt_pt);
        ret = target->index_open(cof, O_RDONLY, NULL);
        if (ret != PLFS_SUCCESS) {
            delete target;
            return(ret);
        }

    } else {

        target = this;   /* RDONLY, should already be in memory */

    }
    
    Util::MutexLock(&target->bri_mutex, __FUNCTION__);

    ret = target->query_helper(cof, input_offset, input_length, result);

    Util::MutexUnlock(&target->bri_mutex, __FUNCTION__);

    /*
     * discard tmp index if we created one
     */
    if (cof->openflags == O_RDWR) {
        /* ignore return value here */
        target->index_close(cof, NULL, NULL, NULL);
        delete target;
    }
        
    return(ret);
}

plfs_error_t
ByteRangeIndex::index_truncate(Container_OpenFile *cof, off_t offset) {
    /*
     * This is a two step operation: first we apply the operation to
     * all our index dropping files, then we update our in-memory data
     * structures (throwing out index record references that are
     * beyond the new offset).  The index should be open for writing
     * already.  IF we are zeroing (called from zero helper), then the
     * generic code has already truncated all our dropping files.  If
     * we are shrinking a file to a non-zero file, then we need to go
     * through each index dropping file and filter out any records
     * that have data past our new offset.  Once we have updated the
     * dropping files, then we need to walk through our in-memory
     * records and discard the ones past the new offset and reduce the
     * size of any that span the new EOF offset.
     */
    return(PLFS_ENOTSUP);
}

/**
 * ByteRangeIndex::index_closing_wdrop we are closing a write dropping
 *
 * @param cof the open file
 * @param ts timestamp string
 * @param pid the process id closing
 * @param filename the dropping being closed
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_closing_wdrop(Container_OpenFile *cof, string ts,
                                    pid_t pid, const char *filename) {

    /*
     * if we were doing a one-to-one mapping between a PID's data
     * dropping file and an index dropping file, we'd close the index
     * dropping file here.  however, we currently have one shared
     * index for all writing PIDs, so there is nothing for us to do
     * here.  the shared index is closed by index_close when the final
     * reference to the container is closed.
     */

    return(PLFS_SUCCESS);
}

/**
 * ByteRangeIndex::index_new_wdrop: opening a new write dropping
 *
 * @param cof the open file
 * @param ts the timestamp string
 * @param pid the pid/rank that is making the dropping
 * @param filename filename of data dropping
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_new_wdrop(Container_OpenFile *cof, string ts,
                                pid_t pid, const char *filename) {

    plfs_error_t ret = PLFS_SUCCESS;
    ostringstream idrop_pathstream;
    mode_t old_mode;

    if (this->iwritefh != NULL)     /* quick short circuit check... */
        return(ret);

    Util::MutexLock(&this->bri_mutex, __FUNCTION__);
    if (this->iwritefh == NULL) {   /* recheck, in case we lost a race */

        /*
         * use cof->pid rather than pid (the args) so that the index
         * filename matches the open file meta dropping.  they will be
         * the same most of the time (exception can be when we've got
         * multiple pids sharing the fd for writing).
         */
        idrop_pathstream << cof->subdir_path << "/" << INDEXPREFIX <<
            ts << "." << cof->hostname << "." << cof->pid;
        
        old_mode = umask(0);
        ret = cof->subdirback->store->Open(idrop_pathstream.str().c_str(),
                                           O_WRONLY|O_APPEND|O_CREAT,
                                           DROPPING_MODE, &this->iwritefh);
        umask(old_mode);

        if (ret == PLFS_SUCCESS) {
            this->iwriteback = cof->subdirback;
        }
    }
    Util::MutexUnlock(&this->bri_mutex, __FUNCTION__);
    
    return(ret);
}

plfs_error_t
ByteRangeIndex::index_optimize(Container_OpenFile *cof) {
#if 0
    struct plfs_pathback container;
    plfs_error_t ret = PLFS_SUCCESS;
    Index *index;
    bool newly_created = false;

    container.bpath = fd->getPath();
    container.back = fd->getCanBack();

    if ( fd && fd->getIndex() ) {
        index = fd->getIndex();
    } else {
        index = new Index(container.bpath, container.back);
        newly_created = true;
        // before we populate, need to blow away any old one
        ret = Container::populateIndex(container.bpath, container.back,
                index,false,false,0);
        /* XXXCDC: why are we ignoring return value of populateIndex? */
    }

    if (Container::isContainer(&container, NULL)) {
        ret = Container::flattenIndex(container.bpath, container.back,
                                      index);
    } else {
        ret = PLFS_EBADF; // not sure here.  Maybe return SUCCESS?
    }
    if (newly_created) {
        delete index;
    }
    return(ret);
#endif
    return(PLFS_ENOTSUP);
}

plfs_error_t
ByteRangeIndex::index_getattr_size(struct plfs_physpathinfo *ppip, 
                                   struct stat *stbuf, set<string> *openset,
                                   set<string> *metaset) {
    /* XXX: load into memory (if not), get size, release if loaded. */
    return(PLFS_ENOTSUP);
}

/**
 * index_droppings_rename: rename an index after a container has been
 * moved.
 *
 * @param src old pathname
 * @param dst new pathname
 * @return PLFS_SUCCESS or PLFS_E*
 */
plfs_error_t ByteRangeIndex::index_droppings_rename(
                  struct plfs_physpathinfo *src,
                  struct plfs_physpathinfo *dst) {

    /* nothing to do, since our data was moved with the container */

    return(PLFS_SUCCESS);
}

/**
 * index_droppings_trunc: this should be called when the truncate
 * offset is less than the current size of the file.  we don't actually
 * remove any data here, we just edit index files and meta droppings.
 * when a file is truncated to zero, that is handled separately and
 * that does actually remove data files.
 *
 * @param ppip container to truncate
 * @param offset the new max offset (>=1)
 * @return PLFS_SUCCESS or PLFS_E*
 */
plfs_error_t
ByteRangeIndex::index_droppings_trunc(struct plfs_physpathinfo *ppip,
                                      off_t offset) {
#if 0
    /*
     * XXXIDX: this is the old Container::Truncate() code -- needs
     * to be updated for the new index structure.
     */
    plfs_error_t ret = PLFS_SUCCESS;
    string indexfile;
    struct plfs_backend *indexback;
    mlog(CON_DAPI, "%s on %s to %ld", __FUNCTION__, path.c_str(),
         (unsigned long)offset);
    // this code here goes through each index dropping and rewrites it
    // preserving only entries that contain data prior to truncate offset
    IOSDirHandle *candir, *subdir;
    string hostdirpath;
    candir = subdir = NULL;

    int dropping;
    while ((ret = nextdropping(path, canback, &indexfile, &indexback,
                               INDEXPREFIX, &candir, &subdir,
                               &hostdirpath, &dropping)) == PLFS_SUCCESS) {
        if (dropping != 1) {
            break;
        }
        Index index( indexfile, indexback, NULL );
        mlog(CON_DCOMMON, "%s new idx %p %s", __FUNCTION__,
             &index,indexfile.c_str());
        ret = index.readIndex(indexfile, indexback);
        if ( ret == PLFS_SUCCESS ) {
            if ( index.lastOffset() > offset ) {
                mlog(CON_DCOMMON, "%s %p at %ld",__FUNCTION__,&index,
                     (unsigned long)offset);
                index.truncate(offset);
                IOSHandle *fh;
                ret = indexback->store->Open(indexfile.c_str(),
                                             O_TRUNC|O_WRONLY, &fh);
                if ( ret != PLFS_SUCCESS ) {
                    mlog(CON_CRIT, "Couldn't overwrite index file %s: %s",
                         indexfile.c_str(), strplfserr( ret ));
                    return ret;
                }
                /* note: index obj already contains indexback */
                ret = index.rewriteIndex(fh);
                indexback->store->Close(fh);
                if ( ret != PLFS_SUCCESS ) {
                    break;
                }
            }
        } else {
            mlog(CON_CRIT, "Failed to read index file %s: %s",
                 indexfile.c_str(), strplfserr( ret ));
            break;
        }
    }
    if ( ret == PLFS_SUCCESS ) {
        ret = truncateMeta(path,offset,canback);
    }
    mlog(CON_DAPI, "%s on %s to %ld ret: %d",
         __FUNCTION__, path.c_str(), (long)offset, ret);
    return ret;
#endif
    return(PLFS_ENOTSUP);
}

/**
 * ByteRangeIndex::index_droppings_unlink unlink index droppings as
 * part of an unlink operation.
 *
 * @param ppip the path we are unlinking
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_droppings_unlink(struct plfs_physpathinfo *ppip) {
    /*
     * nothing additional to do here, as we let container unlink
     * delete all our index droppings for us.
     */
    return(PLFS_SUCCESS);
}

/**
 * ByteRangeIndex::index_droppings_zero called when truncating a file
 * to zero when we want to zero all index records
 *
 * @param ppip the index getting zeroed
 * @return PLFS_SUCCESS or error code
 */
plfs_error_t
ByteRangeIndex::index_droppings_zero(struct plfs_physpathinfo *ppip) {
    /*
     * nothing additional to do here, as we let containerfs_zero_helper
     * delete all our index droppings for us.
     */
    return(PLFS_SUCCESS);
}


#if 0
/*
 * XXXCDC: this commented out section of old code is just here for
 * reference and will be removed in the final version of the file.
 */

/* XXXCDC:
 *
 *  we need the 'if ( openHosts.size() > 0 )' for index_getattr_size operation
 */
/**
 * Container::getattr: does stat of a PLFS file by examining internal droppings
 *
 * @param path the bpath to the canonical container
 * @param canback the canonical backend for bpath
 * @param stbuf where to place the results
 * @return PLFS_SUCCESS or PLFS_E*
 */
plfs_error_t
Container::getattr( const string& path, struct plfs_backend *canback,
                    struct stat *stbuf )
{
    plfs_error_t rv;
    // Need to walk the whole structure
    // and build up the stat.
    // three ways to do so:
    // used cached info when available
    // otherwise, either stat the data files or
    // read the index files
    // stating the data files is much faster
    // (see ~/Testing/plfs/doc/SC09/data/stat/stat_full.png)
    // but doesn't correctly account for holes
    // but reading index files might fail if they're being buffered
    // safest to use_cache and stat_data
    // ugh, but we can't stat the data dropping, actually need to read the
    // index.  this is because Chombo truncates the thing to a future
    // value and we don't see it since it's only in the index file
    // maybe safest to get all of them.  But using both is no good bec
    // it adds both the index and the data.  ugh.
    plfs_error_t ret = PLFS_SUCCESS;
    // get the permissions and stuff from the access file
    string accessfile = getAccessFilePath( path );
    if ( (rv = canback->store->Lstat( accessfile.c_str(), stbuf )) != PLFS_SUCCESS ) {
        mlog(CON_DRARE, "%s lstat of %s failed: %s",
             __FUNCTION__, accessfile.c_str(), strplfserr( rv ) );
        return(rv);
    }
    stbuf->st_size    = 0;
    stbuf->st_blocks  = 0;
    stbuf->st_mode    = file_mode(stbuf->st_mode);
    // first read the open dir to see who has the file open then read the
    // meta dir to pull all useful droppings out of there (use everything
    // as long as it's not open), if we can't use meta than we need to pull
    // the info from the hostdir by stating the data files and maybe even
    // actually reading the index files!
    // now the open droppings are stored in the meta dir so we don't need
    // to readdir twice
    set<string> entries, openHosts, validMeta;
    set<string>::iterator itr;
    ReaddirOp rop(NULL,&entries,false,true);
    ret = rop.op(getMetaDirPath(path).c_str(), DT_DIR, canback->store);
    // ignore ENOENT.  Possible this is freshly created container and meta
    // doesn't exist yet.
    if (ret!=PLFS_SUCCESS && ret!=PLFS_ENOENT) {
        mlog(CON_DRARE, "readdir of %s returned %d (%s)", 
            getMetaDirPath(path).c_str(), ret, strplfserr(ret));
        return ret;
    } 
    ret = PLFS_SUCCESS;

    // first get the set of all open hosts
    discoverOpenHosts(entries, openHosts);
    // then consider the valid set of all meta droppings (not open droppings)
    for(itr=entries.begin(); itr!=entries.end(); itr++) {
        if (istype(*itr,OPENPREFIX)) {
            continue;
        }
        off_t last_offset;
        size_t total_bytes;
        struct timespec time;
        mss::mlog_oss oss(CON_DCOMMON);
        string host = fetchMeta(*itr, &last_offset, &total_bytes, &time);
        if (openHosts.find(host) != openHosts.end()) {
            mlog(CON_DRARE, "Can't use metafile %s because %s has an "
                 " open handle", itr->c_str(), host.c_str() );
            continue;
        }
        oss  << "Pulled meta " << last_offset << " " << total_bytes
             << ", " << time.tv_sec << "." << time.tv_nsec
             << " on host " << host;
        oss.commit();
        // oh, let's get rewrite correct.  if someone writes
        // a file, and they close it and then later they
        // open it again and write some more then we'll
        // have multiple metadata droppings.  That's fine.
        // just consider all of them.
        stbuf->st_size   =  max( stbuf->st_size, last_offset );
        stbuf->st_blocks += bytesToBlocks( total_bytes );
        stbuf->st_mtime  =  max( stbuf->st_mtime, time.tv_sec );
        validMeta.insert(host);
    }
    // if we're using cached data we don't do this part unless there
    // were open hosts
    int chunks = 0;
    if ( openHosts.size() > 0 ) {
        // we used to use the very cute nextdropping code which maintained
        // open readdir handles and just iterated one at a time through
        // all the contents of a container
        // but the new metalink stuff makes that hard.  So lets use our
        // helper functions which will make memory overheads....
        vector<plfs_pathback> indices;
        vector<plfs_pathback>::iterator pitr;
        ret = collectIndices(path,canback,indices,true);
        chunks = indices.size();
        for(pitr=indices.begin(); pitr!=indices.end() && ret==PLFS_SUCCESS; pitr++) {
            plfs_pathback dropping = *pitr;
            string host = hostFromChunk(dropping.bpath,INDEXPREFIX);
            // need to read index data when host_is_open OR not cached
            bool host_is_open;
            bool host_is_cached;
            bool use_this_index;
            host_is_open = (openHosts.find(host) != openHosts.end());
            host_is_cached = (validMeta.find(host) != validMeta.end());
            use_this_index = (host_is_open || !host_is_cached);
            if (!use_this_index) {
                continue;
            }
            // stat the dropping to get the timestamps
            // then read the index info
            struct stat dropping_st;
            if ((ret = dropping.back->store->Lstat(dropping.bpath.c_str(),
                                                   &dropping_st)) != PLFS_SUCCESS ) {
                mlog(CON_DRARE, "lstat of %s failed: %s",
                     dropping.bpath.c_str(), strplfserr( ret ) );
                continue;   // shouldn't this be break?
            }
            stbuf->st_ctime = max(dropping_st.st_ctime, stbuf->st_ctime);
            stbuf->st_atime = max(dropping_st.st_atime, stbuf->st_atime);
            stbuf->st_mtime = max(dropping_st.st_mtime, stbuf->st_mtime);
            mlog(CON_DCOMMON, "Getting stat info from index dropping");
            Index index(path, dropping.back);
            index.readIndex(dropping.bpath, dropping.back);
            stbuf->st_blocks += bytesToBlocks( index.totalBytes() );
            stbuf->st_size   = max(stbuf->st_size, index.lastOffset());
        }
    }
    mss::mlog_oss oss(CON_DCOMMON);
    oss  << "Examined " << chunks << " droppings:"
         << path << " total size " << stbuf->st_size <<  ", usage "
         << stbuf->st_blocks << " at " << stbuf->st_blksize;
    oss.commit();
    return ret;
}
#endif

