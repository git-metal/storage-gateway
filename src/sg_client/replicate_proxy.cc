#include <thread>
#include <chrono>
#include "replicate_proxy.h"
#include "log/log.h"
using huawei::proto::SnapScene;
using huawei::proto::SnapType;

ReplicateProxy::ReplicateProxy(const Configure& conf, const string& vol_name,
            const size_t& vol_size,
            std::shared_ptr<SnapshotProxy> snapshot_proxy):
            vol_name_(vol_name),
            vol_size_(vol_size),
            snapshot_proxy_(snapshot_proxy){

    conf_ = conf;
    rep_inner_client_.reset(new RepInnerCtrlClient(grpc::CreateChannel(conf_.sg_server_addr(),
                grpc::InsecureChannelCredentials())));
}

ReplicateProxy::~ReplicateProxy(){
}

StatusCode ReplicateProxy::create_snapshot(const string& snap_name,
        JournalMarker& marker){
    CreateSnapshotReq req;
    CreateSnapshotAck ack;

    req.mutable_header()->set_seq_id(0);
    req.mutable_header()->set_scene(SnapScene::FOR_REPLICATION);
    req.mutable_header()->set_snap_type(SnapType::SNAP_LOCAL);
    req.mutable_header()->set_replication_uuid("");
    req.mutable_header()->set_checkpoint_uuid("");
    req.set_vol_name(vol_name_);
    req.set_vol_size(vol_size_);
    req.set_snap_name(snap_name);

    return snapshot_proxy_->create_snapshot(&req, &ack,marker);
}

StatusCode ReplicateProxy::create_transaction(const SnapReqHead& shead,
        const string& snap_name, const RepRole& role){
    StatusCode ret_code;

    LOG_DEBUG << "replay a replicate snapshot:" << snap_name;
    while(is_sync_item_exist(snap_name)){
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    };
    // report sg_server that replayer got a replicate snap;
    // sg_server will validate this snapshot
    bool is_discard;
    ret_code = rep_inner_client_->report_checkpoint(snap_name,
        vol_name_,role,is_discard);
    if(ret_code != StatusCode::sOk){
        LOG_ERROR << "report replicate checkpoint failed, volume:" << vol_name_;
        return ret_code;
    }
    if(!is_discard){
        ret_code = snapshot_proxy_->create_transaction(shead, snap_name);
        if(ret_code != StatusCode::sOk){
            LOG_ERROR << "snapshot proxy create transaction failed:" << ret_code;
        }
    }
    else{
        //if snapshot is not found in volume replicate operation records,delete it
        ret_code = snapshot_proxy_->do_update(shead, snap_name,
                UpdateEvent::DELETE_EVENT);
        if(ret_code != StatusCode::sOk){
            LOG_ERROR << "try to delete volume[" << vol_name_
                << "] replicate snapshot[" << snap_name << "]failed!";
        }
    }
    return ret_code;
}

// generate  snap_name by operate uuid
string ReplicateProxy::operate_uuid_to_snap_name(const string& operate_id){
    return operate_id;
}

// extract operate uuid from snap_name
string ReplicateProxy::snap_name_to_operate_uuid(const string& snap_name){
    return snap_name;
}

void ReplicateProxy::add_sync_item(const std::string& actor,const std::string& action){
    WriteLock write_lock(map_mtx_);
    sync_map_.insert(std::pair<string,string>(actor,action));
}

void ReplicateProxy::delete_sync_item(const std::string& actor){
    WriteLock write_lock(map_mtx_);
    sync_map_.erase(actor);
}

bool ReplicateProxy::is_sync_item_exist(const std::string& actor){
    ReadLock read_lock(map_mtx_);
    auto it = sync_map_.find(actor);
    return (it != sync_map_.end());
}
