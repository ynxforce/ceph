// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/Mutex.h"

#include "librbd/AioCompletion.h"
#include "librbd/ImageCtx.h"

#include "librbd/AioRequest.h"
#include "librbd/CopyupRequest.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::CopyupRequest: "

namespace librbd {

  CopyupRequest::CopyupRequest(ImageCtx *ictx, const std::string &oid,
                               uint64_t objectno, bool send_copyup)
    : m_ictx(ictx), m_oid(oid), m_object_no(objectno), m_send_copyup(send_copyup)
  {
  }

  CopyupRequest::~CopyupRequest() {
    assert(m_ictx->copyup_list_lock.is_locked());
    assert(m_pending_requests.empty());

    ldout(m_ictx->cct, 20) << __func__ << " removing the slot " << dendl;
    map<uint64_t, CopyupRequest*>::iterator it =
      m_ictx->copyup_list.find(m_object_no);
    assert(it != m_ictx->copyup_list.end());
    m_ictx->copyup_list.erase(it);

    if (m_ictx->copyup_list.empty()) {
      m_ictx->copyup_list_cond.Signal();
    }

    ldout(m_ictx->cct, 20) <<  __func__ << " remove the slot " << m_object_no
                           << " in copyup_list, size = " << m_ictx->copyup_list.size()
                           << dendl;
  }

  ceph::bufferlist& CopyupRequest::get_copyup_data() {
    return m_copyup_data;
  }

  void CopyupRequest::append_request(AioRequest *req) {
    m_pending_requests.push_back(req);
  }

  void CopyupRequest::complete_all(int r) {
    while (!m_pending_requests.empty()) {
      vector<AioRequest *>::iterator it = m_pending_requests.begin();
      AioRequest *req = *it;
      req->complete(r);
      m_pending_requests.erase(it);
    }
  }

  void CopyupRequest::send_copyup(int r) {
    ldout(m_ictx->cct, 20) << __func__ << dendl;

    m_ictx->snap_lock.get_read();
    ::SnapContext snapc = m_ictx->snapc;
    m_ictx->snap_lock.put_read();

    std::vector<librados::snap_t> snaps;
    snaps.insert(snaps.end(), snapc.snaps.begin(), snapc.snaps.end());

    librados::ObjectWriteOperation copyup_op;
    copyup_op.exec("rbd", "copyup", m_copyup_data);

    librados::AioCompletion *comp =
      librados::Rados::aio_create_completion(NULL, NULL, NULL);
    m_ictx->md_ctx.aio_operate(m_oid, comp, &copyup_op, snapc.seq.val, snaps);
    comp->release();
  }

  void CopyupRequest::read_from_parent(vector<pair<uint64_t,uint64_t> >& image_extents)
  {
    AioCompletion *comp = aio_create_completion_internal(
      this, &CopyupRequest::read_from_parent_cb);
    ldout(m_ictx->cct, 20) << __func__ << " this = " << this
                           << " parent completion " << comp
                           << " extents " << image_extents
                           << dendl;

    int r = aio_read(m_ictx->parent, image_extents, NULL, &m_copyup_data,
		     comp, 0);
    if (r < 0) {
      comp->release();
      delete this;
    }
  }

  void CopyupRequest::queue_read_from_parent(vector<pair<uint64_t,uint64_t> >& image_extents)
  {
    // TODO: once the ObjectCacher allows reentrant read requests, the finisher
    // should be eliminated
    C_ReadFromParent *ctx = new C_ReadFromParent(this, image_extents);
    m_ictx->copyup_finisher->queue(ctx);
  }

  void CopyupRequest::read_from_parent_cb(completion_t cb, void *arg)
  {
    CopyupRequest *req = reinterpret_cast<CopyupRequest *>(arg);
    AioCompletion *comp = reinterpret_cast<AioCompletion *>(cb);

    ldout(req->m_ictx->cct, 20) << __func__ << dendl;
    req->complete_all(comp->get_return_value());

    // If this entry is created by a read request, then copyup operation will
    // be performed asynchronously. Perform cleaning up from copyup callback.
    // If this entry is created by a write request, then copyup operation will
    // be performed synchronously by AioWrite. After extracting data, perform
    // cleaning up here
    if (req->m_send_copyup) {
      req->send_copyup(comp->get_return_value());
    }

    Mutex::Locker l(req->m_ictx->copyup_list_lock);
    delete req;
  }
}
