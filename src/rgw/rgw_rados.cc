#include <errno.h>
#include <stdlib.h>

#include "rgw_access.h"
#include "rgw_rados.h"

#include "include/rados/librados.hpp"
using namespace librados;

#include <string>
#include <iostream>
#include <vector>
#include <list>
#include <map>

using namespace std;

Rados *rados = NULL;

static librados::IoCtx root_pool_ctx;

/** 
 * Initialize the RADOS instance and prepare to do other ops
 * Returns 0 on success, -ERR# on failure.
 */
int RGWRados::initialize(md_config_t *conf)
{
  int ret;
  rados = new Rados();
  if (!rados)
    return -ENOMEM;

  ret = rados->init_with_config(conf);
  if (ret < 0)
   return ret;

  ret = rados->connect();
  if (ret < 0)
   return ret;

  ret = open_root_pool_ctx();

  return ret;
}

/**
 * Open the pool used as root for this gateway
 * Returns: 0 on success, -ERR# otherwise.
 */
int RGWRados::open_root_pool_ctx()
{
  int r = rados->ioctx_create(RGW_ROOT_BUCKET, root_pool_ctx);
  if (r == -ENOENT) {
    r = rados->pool_create(RGW_ROOT_BUCKET);
    if (r < 0)
      return r;

    r = rados->ioctx_create(RGW_ROOT_BUCKET, root_pool_ctx);
  }

  return r;
}

int RGWRados::open_bucket_ctx(std::string& bucket, librados::IoCtx&  io_ctx)
{
  int r = rados->ioctx_create(bucket.c_str(), io_ctx);
  if (r != -ENOENT)
    return r;

  /* couldn't find bucket, might be a racing bucket creation,
     where client haven't gotten updated map, try to read
     the bucket object .. which will trigger update of osdmap
     if that is the case */
  time_t mtime;
  r = root_pool_ctx.stat(bucket, NULL, &mtime);
  if (r < 0)
    return -ENOENT;

  r = rados->ioctx_create(bucket.c_str(), io_ctx);

  return r;
}

class RGWRadosListState {
public:
  std::list<string> list;
  std::list<string>::iterator pos;
  RGWRadosListState() : pos(0) {}
};

/**
 * set up a bucket listing.
 * id is ignored
 * handle is filled in.
 * Returns 0 on success, -ERR# otherwise.
 */
int RGWRados::list_buckets_init(std::string& id, RGWAccessHandle *handle)
{
  RGWRadosListState *state = new RGWRadosListState();

  if (!state)
    return -ENOMEM;

  int r = rados->pool_list(state->list);
  if (r < 0)
    return r;
  state->pos = state->list.begin();

  *handle = (RGWAccessHandle)state;

  return 0;
}

/** 
 * get the next bucket in the listing.
 * id is ignored
 * obj is filled in,
 * handle is updated.
 * returns 0 on success, -ERR# otherwise.
 */
int RGWRados::list_buckets_next(std::string& id, RGWObjEnt& obj, RGWAccessHandle *handle)
{
  RGWRadosListState *state = (RGWRadosListState *)*handle;

  if (state->pos == state->list.end()) {
    delete state;
    return -ENOENT;
  }

  obj.name = *state->pos;
  state->pos++;

  /* FIXME: should read mtime/size vals for bucket */

  return 0;
}

/** 
 * get listing of the objects in a bucket.
 * id: ignored.
 * bucket: bucket to list contents of
 * max: maximum number of results to return
 * prefix: only return results that match this prefix
 * delim: do not include results that match this string.
 *     Any skipped results will have the matching portion of their name
 *     inserted in common_prefixes with a "true" mark.
 * marker: if filled in, begin the listing with this object.
 * result: the objects are put in here.
 * common_prefixes: if delim is filled in, any matching prefixes are placed
 *     here.
 */
int RGWRados::list_objects(string& id, string& bucket, int max, string& prefix, string& delim,
			   string& marker, vector<RGWObjEnt>& result, map<string, bool>& common_prefixes,
			   bool get_content_type)
{
  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;

  set<string> dir_set;
  {
    librados::ObjectIterator i_end = io_ctx.objects_end();
    for (librados::ObjectIterator i = io_ctx.objects_begin(); i != i_end; ++i) {
	if (prefix.empty() ||
	    ((*i).compare(0, prefix.size(), prefix) == 0)) {
	  dir_set.insert(*i);
	}
    }
  }

  set<string>::iterator p;
  if (!marker.empty())
    p = dir_set.lower_bound(marker);
  else
    p = dir_set.begin();

  if (max < 0) {
    max = dir_set.size();
  }

  result.clear();
  int i, count = 0;
  for (i=0; i<max && p != dir_set.end(); i++, ++p) {
    RGWObjEnt obj;
    obj.name = *p;

    if (!delim.empty()) {
      int delim_pos = obj.name.find(delim, prefix.size());

      if (delim_pos >= 0) {
        common_prefixes[obj.name.substr(0, delim_pos + 1)] = true;
        continue;
      }
    }

    uint64_t s;
    string p_str = *p;
    if (io_ctx.stat(*p, &s, &obj.mtime) < 0)
      continue;
    obj.size = s;

    bufferlist bl; 
    obj.etag[0] = '\0';
    if (io_ctx.getxattr(*p, RGW_ATTR_ETAG, bl) >= 0) {
      strncpy(obj.etag, bl.c_str(), sizeof(obj.etag));
      obj.etag[sizeof(obj.etag)-1] = '\0';
    }
    if (get_content_type) {
      bl.clear();
      obj.content_type = "";
      if (io_ctx.getxattr(*p, RGW_ATTR_CONTENT_TYPE, bl) >= 0) {
        obj.content_type = bl.c_str();
      }
    }
    result.push_back(obj);
  }

  return count;
}

/**
 * create a bucket with name bucket and the given list of attrs
 * if auid is set, it sets the auid of the underlying rados io_ctx
 * returns 0 on success, -ERR# otherwise.
 */
int RGWRados::create_bucket(std::string& id, std::string& bucket, map<std::string, bufferlist>& attrs, uint64_t auid)
{
  librados::ObjectOperation op;
  op.create(true);

  for (map<string, bufferlist>::iterator iter = attrs.begin(); iter != attrs.end(); ++iter)
    op.setxattr(iter->first.c_str(), iter->second);

  bufferlist outbl;
  int ret = root_pool_ctx.operate(bucket, &op, &outbl);
  if (ret < 0)
    return ret;

  ret = rados->pool_create(bucket.c_str(), auid);
  if (ret)
    root_pool_ctx.remove(bucket);

  return ret;
}

/**
 * Write/overwrite an object to the bucket storage.
 * id: ignored
 * bucket: the bucket to store the object in
 * obj: the object name/key
 * data: the object contents/value
 * size: the amount of data to write (data must be this long)
 * mtime: if non-NULL, writes the given mtime to the bucket storage
 * attrs: all the given attrs are written to bucket storage for the given object
 * Returns: 0 on success, -ERR# otherwise.
 */
int RGWRados::put_obj_meta(std::string& id, std::string& bucket, std::string& oid,
                  time_t *mtime, map<string, bufferlist>& attrs)
{
  librados::IoCtx io_ctx;

  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;

  map<string, bufferlist>::iterator iter;
  for (iter = attrs.begin(); iter != attrs.end(); ++iter) {
    const string& name = iter->first;
    bufferlist& bl = iter->second;

    if (bl.length()) {
      r = io_ctx.setxattr(oid, name.c_str(), bl);
      if (r < 0)
        return r;
    }
  }

  if (mtime) {
    r = io_ctx.stat(oid, NULL, mtime);
    if (r < 0)
      return r;
  }

  return 0;
}

/**
 * Write/overwrite an object to the bucket storage.
 * id: ignored
 * bucket: the bucket to store the object in
 * obj: the object name/key
 * data: the object contents/value
 * offset: the offet to write to in the object
 *         If this is -1, we will overwrite the whole object.
 * size: the amount of data to write (data must be this long)
 * mtime: if non-NULL, writes the given mtime to the bucket storage
 * attrs: all the given attrs are written to bucket storage for the given object
 * Returns: 0 on success, -ERR# otherwise.
 */
int RGWRados::put_obj_data(std::string& id, std::string& bucket, std::string& oid,
			   const char *data, off_t ofs, size_t len, time_t *mtime)
{
  librados::IoCtx io_ctx;

  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;

  bufferlist bl;
  bl.append(data, len);
  if (ofs == -1) {
    // write_full wants to write the complete bufferlist, not part of it
    assert(bl.length() == len);

    r = io_ctx.write_full(oid, bl);
  }
  else {
    r = io_ctx.write(oid, bl, len, ofs);
  }
  if (r < 0)
    return r;

  if (mtime) {
    r = io_ctx.stat(oid, NULL, mtime);
    if (r < 0)
      return r;
  }

  return 0;
}
/**
 * Copy an object.
 * id: unused (well, it's passed to put_obj)
 * dest_bucket: the bucket to copy into
 * dest_obj: the object to copy into
 * src_bucket: the bucket to copy from
 * src_obj: the object to copy from
 * mod_ptr, unmod_ptr, if_match, if_nomatch: as used in get_obj
 * attrs: these are placed on the new object IN ADDITION to
 *    (or overwriting) any attrs copied from the original object
 * err: stores any errors resulting from the get of the original object
 * Returns: 0 on success, -ERR# otherwise.
 */
int RGWRados::copy_obj(std::string& id, std::string& dest_bucket, std::string& dest_obj,
               std::string& src_bucket, std::string& src_obj,
               time_t *mtime,
               const time_t *mod_ptr,
               const time_t *unmod_ptr,
               const char *if_match,
               const char *if_nomatch,
               map<string, bufferlist>& attrs,  /* in/out */
               struct rgw_err *err)
{
  int ret, r;
  char *data;
  off_t end = -1;
  size_t total_len;
  time_t lastmod;
  map<string, bufferlist>::iterator iter;

  RGW_LOG(5) << "Copy object " << src_bucket << ":" << src_obj << " => " << dest_bucket << ":" << dest_obj << dendl;

  void *handle = NULL;

  map<string, bufferlist> attrset;
  ret = prepare_get_obj(src_bucket, src_obj, 0, &end, &attrset,
                mod_ptr, unmod_ptr, &lastmod, if_match, if_nomatch, &total_len, &handle, err);

  if (ret < 0)
    return ret;

  off_t ofs = 0;
  do {
    ret = get_obj(&handle, src_bucket, src_obj, &data, ofs, end);
    if (ret < 0)
      return ret;

    // In the first call to put_obj_data, we pass ofs == -1 so that it will do
    // a write_full, wiping out whatever was in the object before this
    r = put_obj_data(id, dest_bucket, dest_obj, data,
		     ((ofs == 0) ? -1 : ofs), ret, NULL);
    free(data);
    if (r < 0)
      goto done_err;

    ofs += ret;
  } while (ofs <= end);

  for (iter = attrs.begin(); iter != attrs.end(); ++iter) {
    attrset[iter->first] = iter->second;
  }
  attrs = attrset;

  ret = put_obj_meta(id, dest_bucket, dest_obj, mtime, attrs);

  finish_get_obj(&handle);

  return ret;
done_err:
  finish_get_obj(&handle);
  return r;
}

/**
 * Delete a bucket.
 * id: unused
 * bucket: the name of the bucket to delete
 * Returns 0 on success, -ERR# otherwise.
 */
int RGWRados::delete_bucket(std::string& id, std::string& bucket)
{
  librados::IoCtx list_ctx;
  int r = open_bucket_ctx(bucket, list_ctx);
  if (r < 0)
    return r;

  if (list_ctx.objects_begin() != list_ctx.objects_end())
    return -ENOTEMPTY;

  r = rados->pool_delete(bucket.c_str());
  if (r < 0)
    return r;

  librados::IoCtx io_ctx;
  r = rados->ioctx_create(RGW_ROOT_BUCKET, io_ctx);
  if (r < 0) {
    RGW_LOG(0) << "WARNING: failed to create context in delete_bucket, bucket object leaked" << dendl;
    return r;
  }

  r = io_ctx.remove(bucket);
  if (r < 0)
    return r;

  return 0;
}

/**
 * Delete an object.
 * id: unused
 * bucket: name of the bucket storing the object
 * obj: name of the object to delete
 * Returns: 0 on success, -ERR# otherwise.
 */
int RGWRados::delete_obj(std::string& id, std::string& bucket, std::string& oid)
{
  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;

  r = io_ctx.remove(oid);
  if (r < 0)
    return r;

  return 0;
}

/**
 * Get the attributes for an object.
 * bucket: name of the bucket holding the object.
 * obj: name of the object
 * name: name of the attr to retrieve
 * dest: bufferlist to store the result in
 * Returns: 0 on success, -ERR# otherwise.
 */
int RGWRados::get_attr(std::string& bucket, std::string& obj,
                       const char *name, bufferlist& dest)
{
  librados::IoCtx io_ctx;
  string actual_bucket = bucket;
  string actual_obj = obj;

  if (actual_obj.size() == 0) {
    actual_obj = bucket;
    actual_bucket = rgw_root_bucket;
  }

  int r = open_bucket_ctx(actual_bucket, io_ctx);
  if (r < 0)
    return r;

  r = io_ctx.getxattr(actual_obj, name, dest);
  if (r < 0)
    return r;

  return 0;
}

/**
 * Set an attr on an object.
 * bucket: name of the bucket holding the object
 * obj: name of the object to set the attr on
 * name: the attr to set
 * bl: the contents of the attr
 * Returns: 0 on success, -ERR# otherwise.
 */
int RGWRados::set_attr(std::string& bucket, std::string& oid,
                       const char *name, bufferlist& bl)
{
  librados::IoCtx io_ctx;
  string actual_bucket = bucket;
  string actual_obj = oid;

  if (actual_obj.size() == 0) {
    actual_obj = bucket;
    actual_bucket = rgw_root_bucket;
  }

  int r = open_bucket_ctx(actual_bucket, io_ctx);
  if (r < 0)
    return r;

  r = io_ctx.setxattr(actual_obj, name, bl);
  if (r < 0)
    return r;

  return 0;
}

/**
 * Get data about an object out of RADOS and into memory.
 * bucket: name of the bucket the object is in.
 * obj: name/key of the object to read
 * data: if get_data==true, this pointer will be set
 *    to an address containing the object's data/value
 * ofs: the offset of the object to read from
 * end: the point in the object to stop reading
 * attrs: if non-NULL, the pointed-to map will contain
 *    all the attrs of the object when this function returns
 * mod_ptr: if non-NULL, compares the object's mtime to *mod_ptr,
 *    and if mtime is smaller it fails.
 * unmod_ptr: if non-NULL, compares the object's mtime to *unmod_ptr,
 *    and if mtime is >= it fails.
 * if_match/nomatch: if non-NULL, compares the object's etag attr
 *    to the string and, if it doesn't/does match, fails out.
 * get_data: if true, the object's data/value will be read out, otherwise not
 * err: Many errors will result in this structure being filled
 *    with extra informatin on the error.
 * Returns: -ERR# on failure, otherwise
 *          (if get_data==true) length of read data,
 *          (if get_data==false) length of the object
 */
int RGWRados::prepare_get_obj(std::string& bucket, std::string& oid, 
            off_t ofs, off_t *end,
            map<string, bufferlist> *attrs,
            const time_t *mod_ptr,
            const time_t *unmod_ptr,
            time_t *lastmod,
            const char *if_match,
            const char *if_nomatch,
            size_t *total_size,
            void **handle,
            struct rgw_err *err)
{
  int r = -EINVAL;
  uint64_t size;
  bufferlist etag;
  time_t mtime;
  time_t ctime;

  map<string, bufferlist>::iterator iter;

  *handle = NULL;

  GetObjState *state = new GetObjState;
  if (!state)
    return -ENOMEM;

  *handle = state;

  r = open_bucket_ctx(bucket, state->io_ctx);
  if (r < 0)
    goto done_err;

  if (total_size || end) {
    r = state->io_ctx.stat(oid, &size, &mtime);
    if (r < 0)
      goto done_err;
  }

  if (attrs) {
    r = state->io_ctx.getxattrs(oid, *attrs);
    if (g_conf.rgw_log >= 20) {
      for (iter = attrs->begin(); iter != attrs->end(); ++iter) {
        RGW_LOG(20) << "Read xattr: " << iter->first << dendl;
      }
    }
    if (r < 0)
      goto done_err;
  }

  /* Convert all times go GMT to make them compatible */
  ctime = mktime(gmtime(&mtime));

  r = -ECANCELED;
  if (mod_ptr) {
    RGW_LOG(10) << "If-Modified-Since: " << *mod_ptr << " Last-Modified: " << ctime << dendl;
    if (ctime < *mod_ptr) {
      err->http_ret = 304;
      err->s3_code = "NotModified";

      goto done_err;
    }
  }

  if (unmod_ptr) {
    RGW_LOG(10) << "If-UnModified-Since: " << *unmod_ptr << " Last-Modified: " << ctime << dendl;
    if (ctime > *unmod_ptr) {
      err->http_ret = 412;
      err->s3_code = "PreconditionFailed";
      goto done_err;
    }
  }
  if (if_match || if_nomatch) {
    r = get_attr(bucket, oid, RGW_ATTR_ETAG, etag);
    if (r < 0)
      goto done_err;

    r = -ECANCELED;
    if (if_match) {
      RGW_LOG(10) << "ETag: " << etag.c_str() << " " << " If-Match: " << if_match << dendl;
      if (strcmp(if_match, etag.c_str())) {
        err->http_ret = 412;
        err->s3_code = "PreconditionFailed";
        goto done_err;
      }
    }

    if (if_nomatch) {
      RGW_LOG(10) << "ETag: " << etag.c_str() << " " << " If-NoMatch: " << if_nomatch << dendl;
      if (strcmp(if_nomatch, etag.c_str()) == 0) {
        err->http_ret = 304;
        err->s3_code = "NotModified";
        goto done_err;
      }
    }
  }

  if (end && *end < 0)
    *end = size - 1;

  if (total_size)
    *total_size = (ofs <= *end ? *end + 1 - ofs : 0);
  if (lastmod)
    *lastmod = mtime;

  return 0;

done_err:
  delete state;
  *handle = NULL;
  return r;
}

int RGWRados::get_obj(void **handle,
            std::string& bucket, std::string& oid, 
            char **data, off_t ofs, off_t end)
{
  uint64_t len;
  bufferlist bl;

  GetObjState *state = *(GetObjState **)handle;

  if (end <= 0)
    len = 0;
  else
    len = end - ofs + 1;

  if (len > RGW_MAX_CHUNK_SIZE)
    len = RGW_MAX_CHUNK_SIZE;

  RGW_LOG(20) << "rados->read ofs=" << ofs << " len=" << len << dendl;
  int r = state->io_ctx.read(oid, bl, len, ofs);
  RGW_LOG(20) << "rados->read r=" << r << dendl;

  if (r > 0) {
    *data = (char *)malloc(r);
    memcpy(*data, bl.c_str(), bl.length());
  }

  if (r < 0 || !len || ((off_t)(ofs + len - 1) == end)) {
    delete state;
    *handle = NULL;
  }

  return r;
}

void RGWRados::finish_get_obj(void **handle)
{
  if (*handle) {
    GetObjState *state = *(GetObjState **)handle;
    delete state;
    *handle = NULL;
  }
}

/* a simple object read */
int RGWRados::read(std::string& bucket, std::string& oid, off_t ofs, size_t size, bufferlist& bl)
{
  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;
  r = io_ctx.read(oid, bl, size, ofs);
  return r;
}

int RGWRados::obj_stat(std::string& bucket, std::string& obj, uint64_t *psize, time_t *pmtime)
{
  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;
  if (r < 0)
    return r;
  r = io_ctx.stat(obj, psize, pmtime);
  return r;
}

int RGWRados::tmap_set(std::string& bucket, std::string& obj, std::string& key, bufferlist& bl)
{
  bufferlist cmdbl, emptybl;
  __u8 c = CEPH_OSD_TMAP_SET;

  ::encode(c, cmdbl);
  ::encode(key, cmdbl);
  ::encode(bl, cmdbl);

  RGW_LOG(15) << "tmap_set bucket=" << bucket << " obj=" << obj << " key=" << key << dendl;

  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;
  r = io_ctx.tmap_update(obj, cmdbl);

  return r;
}

int RGWRados::tmap_create(std::string& bucket, std::string& obj, std::string& key, bufferlist& bl)
{
  bufferlist cmdbl, emptybl;
  __u8 c = CEPH_OSD_TMAP_CREATE;

  ::encode(c, cmdbl);
  ::encode(key, cmdbl);
  ::encode(bl, cmdbl);

  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;
  r = io_ctx.tmap_update(obj, cmdbl);
  return r;
}

int RGWRados::tmap_del(std::string& bucket, std::string& obj, std::string& key)
{
  bufferlist cmdbl;
  __u8 c = CEPH_OSD_TMAP_RM;

  ::encode(c, cmdbl);
  ::encode(key, cmdbl);

  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;
  r = io_ctx.tmap_update(obj, cmdbl);
  return r;
}
int RGWRados::update_containers_stats(map<string, RGWBucketEnt>& m)
{
  int count = 0;

  map<string, RGWBucketEnt>::iterator iter;
  list<string> buckets_list;
  for (iter = m.begin(); iter != m.end(); ++iter) {
    string bucket_name = iter->first;
    buckets_list.push_back(bucket_name);
  }
  map<std::string,librados::pool_stat_t> stats;
  int r = rados->get_pool_stats(buckets_list, stats);
  if (r < 0)
    return r;

  map<string,pool_stat_t>::iterator stats_iter = stats.begin();

  for (iter = m.begin(); iter != m.end(); ++iter) {
    string bucket_name = iter->first;
    if (stats_iter->first.compare(bucket_name) == 0) {
      RGWBucketEnt& ent = iter->second;
      pool_stat_t stat = stats_iter->second;
      ent.count = stat.num_objects;
      ent.size = stat.num_bytes;
      stats_iter++;
      count++;
    }
  }

  return count;
}

int RGWRados::append_async(std::string& bucket, std::string& oid, size_t size, bufferlist& bl)
{
  librados::IoCtx io_ctx;
  int r = open_bucket_ctx(bucket, io_ctx);
  if (r < 0)
    return r;
  librados::AioCompletion *completion = rados->aio_create_completion(NULL, NULL, NULL);
  r = io_ctx.aio_append(oid, completion, bl, size);
  completion->release();
  return r;
}

