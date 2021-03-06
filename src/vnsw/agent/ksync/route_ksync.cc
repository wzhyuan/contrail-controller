/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include <db/db_entry.h>
#include <db/db_table.h>
#include <db/db_table_partition.h>
#include <ksync/ksync_index.h>
#include <ksync/ksync_entry.h>
#include <ksync/ksync_object.h>
#include <ksync/ksync_sock.h>

#include "oper/interface.h"
#include "oper/nexthop.h"
#include "oper/inet4_ucroute.h"
#include "oper/inet4_mcroute.h"
#include "oper/mirror_table.h"

#include "ksync/interface_ksync.h"
#include "ksync/nexthop_ksync.h"
#include "ksync/route_ksync.h"

#include "ksync_init.h"
#include "vr_types.h"
#include "vr_defs.h"
#include "vr_nexthop.h"

VrfKSyncObject *VrfKSyncObject::singleton_;

void vr_route_req::Process(SandeshContext *context) {
    AgentSandeshContext *ioc = static_cast<AgentSandeshContext *>(context);
    ioc->RouteMsgHandler(this);
}

KSyncDBObject *RouteKSyncEntry::GetObject() {
    return static_cast<KSyncDBObject*>
        (VrfKSyncObject::GetKSyncObject()->GetRouteKSyncObject(vrf_id_,
                                                               rt_type_));
}

RouteKSyncEntry::RouteKSyncEntry(const Inet4Route *rt) :
    KSyncNetlinkDBEntry(kInvalidIndex),  rt_type_(rt->IsMcast() ? RT_MCAST : RT_UCAST), 
    vrf_id_(rt->GetVrfId()),
    addr_(rt->GetIpAddress()), src_addr_(rt->GetSrcIpAddress()), 
    plen_(rt->GetPlen()), nh_(NULL), label_(0), proxy_arp_(false)
{
}

RouteKSyncObject::~RouteKSyncObject() {
    UnregisterDb(GetDBTable());
    table_delete_ref_.Reset(NULL);
}

bool RouteKSyncEntry::UcIsLess(const KSyncEntry &rhs) const {
    const RouteKSyncEntry &entry = static_cast<const RouteKSyncEntry &>(rhs);
    if (vrf_id_ != entry.vrf_id_) {
        return vrf_id_ < entry.vrf_id_;
    }

    if (addr_ != entry.addr_) {
        return addr_ < entry.addr_;
    }

    return (plen_ < entry.plen_);
}

bool RouteKSyncEntry::McIsLess(const KSyncEntry &rhs) const {
    const RouteKSyncEntry &entry = static_cast<const RouteKSyncEntry &>(rhs);

    if (vrf_id_ != entry.vrf_id_) {
        return vrf_id_ < entry.vrf_id_;
    }

    if (src_addr_ != entry.src_addr_) {
        return src_addr_ < entry.src_addr_;
    }

    return (addr_ < entry.addr_);
}

bool RouteKSyncEntry::IsLess(const KSyncEntry &rhs) const {
    const RouteKSyncEntry &entry = static_cast<const RouteKSyncEntry &>(rhs);

    if (rt_type_ != entry.rt_type_) 
        return rt_type_ < entry.rt_type_;

    //First unicast
    if (rt_type_ == RT_UCAST) {
        return UcIsLess(rhs);
    }

    return McIsLess(rhs);
}

std::string RouteKSyncEntry::ToString() const {
    std::stringstream s;
    NHKSyncEntry *nh;
    nh = GetNH();

    s << "Route : " << vrf_id_ << " : " << addr_.to_string() << " / " 
        << plen_ << " Type:" << rt_type_;

    if (label_ != MplsTable::kInvalidLabel) {
       s << " Label : " << label_;
    }
    
    if (nh) {
        s << " NH : " << nh->GetIndex();
    } else {
        s << " NH : <NULL>";
    }

    return s.str();
}

bool RouteKSyncEntry::Sync(DBEntry *e) {
    bool ret = false;
    const Inet4Route *route;
  

    route = static_cast<Inet4Route *>(e);
    NHKSyncObject *nh_object = NHKSyncObject::GetKSyncObject();
    NHKSyncEntry nh(route->GetActiveNextHop());
    NHKSyncEntry *old_nh = GetNH();

    nh_ = static_cast<NHKSyncEntry *>(nh_object->GetReference(&nh));
    if (old_nh != GetNH()) {
        ret = true;
    }

    //Bother for label only if unicast route
    if (rt_type_ == RT_UCAST) {
        uint32_t old_label = label_;
        const AgentPath *path = (static_cast <Inet4UcRoute *>(e))->GetActivePath();
        label_ = path->GetLabel();

        if (label_ != old_label) {
            ret = true;
        }

        proxy_arp_ = path->GetProxyArp();
    }

    return ret;
};

void RouteKSyncEntry::FillObjectLog(sandesh_op::type type, KSyncRouteInfo &info) {
    info.set_ip(addr_.to_string());
    info.set_vrf(vrf_id_);

    if (type == sandesh_op::ADD) {
        info.set_operation("ADD/CHANGE");
    } else {
        info.set_operation("DELETE");
    }

    if (GetNH()) {
        info.set_nh_idx(GetNH()->GetIndex());
        if (GetNH()->GetType() == NextHop::TUNNEL) {
            info.set_label(label_);
        }
    } else {
        info.set_nh_idx(NH_DISCARD_ID);
    }
}

int RouteKSyncEntry::Encode(sandesh_op::type op, char *buf, int buf_len) {
    vr_route_req encoder;
    int encode_len, error;
    NHKSyncEntry *nh = GetNH();

    encoder.set_h_op(op);
    encoder.set_rtr_rid(0);
    encoder.set_rtr_rt_type(rt_type_);
    encoder.set_rtr_vrf_id(vrf_id_);
    encoder.set_rtr_family(AF_INET);
    encoder.set_rtr_prefix(addr_.to_v4().to_ulong());
    encoder.set_rtr_src(src_addr_.to_v4().to_ulong());
    encoder.set_rtr_prefix_len(plen_);

    int label = 0;
    int flags = 0;
    if (rt_type_ == RT_UCAST && nh != NULL) {
        if (nh->GetType() == NextHop::TUNNEL) {
            label = label_;
            flags |= VR_RT_LABEL_VALID_FLAG;
        }
    }

    if (proxy_arp_) {
        flags |= VR_RT_HOSTED_FLAG;
    }
    encoder.set_rtr_label_flags(flags);
    encoder.set_rtr_label(label);

    if (nh != NULL) {
        encoder.set_rtr_nh_id(nh->GetIndex());
    } else {
        encoder.set_rtr_nh_id(NH_DISCARD_ID);
    }

    encode_len = encoder.WriteBinary((uint8_t *)buf, buf_len, &error);
    return encode_len;
}


int RouteKSyncEntry::AddMsg(char *buf, int buf_len) {
    KSyncRouteInfo info;
    FillObjectLog(sandesh_op::ADD, info);
    KSYNC_TRACE(Route, info);
    return Encode(sandesh_op::ADD, buf, buf_len);
}

int RouteKSyncEntry::ChangeMsg(char *buf, int buf_len){
    KSyncRouteInfo info;
    FillObjectLog(sandesh_op::ADD, info);
    KSYNC_TRACE(Route, info);

    return Encode(sandesh_op::ADD, buf, buf_len);
}

int RouteKSyncEntry::DeleteMsg(char *buf, int buf_len) {

    RouteKSyncEntry key(this, KSyncEntry::kInvalidIndex);
    KSyncEntry *found = NULL;
    RouteKSyncEntry *route = NULL;
    NHKSyncEntry *ksync_nh = NULL;

    // IF multicast delete unconditionally
    if (rt_type_ == RT_MCAST) {
        return DeleteInternal(GetNH(), 0, false, buf, buf_len);
    }

    for (int plen = (GetPLen() - 1); plen >= 0; plen--) {
        uint32_t mask = plen ? (0xFFFFFFFF << (32 - plen)) : 0;
        if (!addr_.is_v4()) 
            continue;
        Ip4Address v4 = addr_.to_v4();
        Ip4Address addr = boost::asio::ip::address_v4(v4.to_ulong() & mask);

        key.SetPLen(plen);
        key.SetIp(addr);
        found = GetObject()->Find(&key);
        
        if (found) {
            route = static_cast<RouteKSyncEntry *>(found);
            if (route->IsResolved()) {
                ksync_nh = route->GetNH();
                if(ksync_nh && ksync_nh->IsResolved()) {
                    return DeleteInternal(ksync_nh, route->GetLabel(),
                                          route->GetProxyArp(), buf, buf_len);
                }
                ksync_nh = NULL;
            }
        }
    }

    /* If better route is not found, send discardNH for route */
    DiscardNHKey nh_oper_key;
    NextHop *nh = static_cast<NextHop *>
        (Agent::GetInstance()->GetNextHopTable()->FindActiveEntry(&nh_oper_key));
    if (nh != NULL) {
        NHKSyncEntry nh_key(nh);
        ksync_nh = static_cast<NHKSyncEntry *>
            (NHKSyncObject::GetKSyncObject()->Find(&nh_key));
    }

    return DeleteInternal(ksync_nh, 0, false, buf, buf_len);
}


int RouteKSyncEntry::DeleteInternal(NHKSyncEntry *nh, uint32_t lbl,
                                    bool proxy_arp, char *buf, int buf_len) {
    nh_ = nh;
    label_ = lbl;
    proxy_arp_ = proxy_arp;

    KSyncRouteInfo info;
    FillObjectLog(sandesh_op::DELETE, info);
    KSYNC_TRACE(Route, info);

    return Encode(sandesh_op::DELETE, buf, buf_len);
}

KSyncEntry *RouteKSyncEntry::UnresolvedReference() {

    NHKSyncEntry *nh = GetNH();
    if (!nh->IsResolved()) {
        return nh;
    }
    return NULL;
}

RouteKSyncObject::RouteKSyncObject(Inet4RouteTable *rt_table):
    KSyncDBObject(), marked_delete_(false), 
    table_delete_ref_(this, rt_table->deleter()) {
    rt_table_ = rt_table;
    RegisterDb(rt_table);
}

void RouteKSyncObject::Unregister() {
    if (IsEmpty() == true && marked_delete_ == true) {
        KSYNC_TRACE(Trace, "Destroying ksync object: " + rt_table_->name());
        VrfKSyncObject::GetKSyncObject()->DelFromVrfMap(this);
        KSyncObjectManager::Unregister(this);
    }
}

void RouteKSyncObject::ManagedDelete() {
    marked_delete_ = true;
    Unregister();
}

void RouteKSyncObject::EmptyTable() {
    if (marked_delete_ == true) {
        Unregister();
    }
}

RouteKSyncObject *VrfKSyncObject::GetRouteKSyncObject(uint32_t vrf_id,
                                                      unsigned int table) {
    VrfRtObjectMap::iterator it;

    if (table == RT_UCAST) {
        it = vrf_ucrt_object_map_.find(vrf_id);
        if (it != vrf_ucrt_object_map_.end()) {
            return it->second;
        }
    } else {
        it = vrf_mcrt_object_map_.find(vrf_id);
        if (it != vrf_mcrt_object_map_.end()) {
            return it->second;
        }
    }
    return NULL;
}

void VrfKSyncObject::AddToVrfMap(uint32_t vrf_id, RouteKSyncObject *rt,
                                 unsigned int table) {
    if (table == RT_UCAST) {
        vrf_ucrt_object_map_.insert(make_pair(vrf_id, rt));
    } else {
        vrf_mcrt_object_map_.insert(make_pair(vrf_id, rt));
    }
}

void VrfKSyncObject::DelFromVrfMap(RouteKSyncObject *rt) {
    VrfRtObjectMap::iterator it;
    for (it = vrf_ucrt_object_map_.begin(); it != vrf_ucrt_object_map_.end(); 
        ++it) {
        if (it->second == rt) {
            vrf_ucrt_object_map_.erase(it);
            return;
        }
    }

    for (it = vrf_mcrt_object_map_.begin(); it != vrf_mcrt_object_map_.end(); 
        ++it) {
        if (it->second == rt) {
            vrf_mcrt_object_map_.erase(it);
            return;
        }
    }
}

void VrfKSyncObject::VrfNotify(DBTablePartBase *partition, DBEntryBase *e) {
    VrfEntry *vrf = static_cast<VrfEntry *>(e);
    VrfState *state = static_cast<VrfState *>
        (vrf->GetState(partition->parent(), singleton_->vrf_listener_id_));
    if (vrf->IsDeleted()) {
        if (state) {
            vrf->ClearState(partition->parent(), singleton_->vrf_listener_id_);
            delete state;
        }
        return;
    }

    if (state == NULL) {
        KSYNC_TRACE(Trace, "Subscribing to route table " + vrf->GetName());
        state = new VrfState();
        state->seen_ = true;
        vrf->SetState(partition->parent(), singleton_->vrf_listener_id_, state);

        // Get Inet4 Route table and register with KSync
        Inet4RouteTable *rt_table = vrf->GetInet4UcRouteTable();
        RouteKSyncObject *ksync = new RouteKSyncObject(rt_table);
        singleton_->AddToVrfMap(vrf->GetVrfId(), ksync, RT_UCAST);

        // Now for multicast table. Ksync object for multicast table is not
        // maintained in vrf list
        // TODO Enhance ksyncobject for UC/MC, currently there is only one entry
        // in MC so just use the UC object for time being.
        rt_table = vrf->GetInet4McRouteTable();
        ksync = new RouteKSyncObject(rt_table);
        singleton_->AddToVrfMap(vrf->GetVrfId(), ksync, RT_MCAST);
    }
}

void VrfKSyncObject::Init(VrfTable *vrf_table) {
    assert(singleton_ == NULL);
    singleton_ = new VrfKSyncObject();

    singleton_->vrf_listener_id_ = Agent::GetInstance()->GetVrfTable()->Register
            (boost::bind(&VrfKSyncObject::VrfNotify, _1, _2));
}

void VrfKSyncObject::Shutdown() {
    Agent::GetInstance()->GetVrfTable()->Unregister
        (singleton_->vrf_listener_id_);
    singleton_->vrf_listener_id_ = -1;
    delete singleton_;
    singleton_ = NULL;
}
