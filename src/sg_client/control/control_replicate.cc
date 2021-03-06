/**********************************************
* Copyright (c) 2016 Huawei Technologies Co., Ltd. All rights reserved.
* 
* File name:    replication_control.cc
* Author: 
* Date:         2017/01/11
* Version:      1.0
* Description:
* 
************************************************/
#include <string>
#include <boost/uuid/uuid.hpp> // uuid class
#include <boost/uuid/uuid_io.hpp> // streaming operators
#include "control_replicate.h"
#include "common/config_option.h"
#include "common/utils.h"
#include "log/log.h"
using std::string;
using huawei::proto::sOk;
using huawei::proto::sInternalError;
using huawei::proto::sVolumeNotExist;
using huawei::proto::sVolumeMetaPersistError;
using huawei::proto::sVolumeAlreadyExist;

ReplicateCtrl::ReplicateCtrl(std::map<string, std::shared_ptr<Volume>>& volumes)
             : volumes_(volumes) {
    
   std::string meta_rpc_addr = rpc_address(g_option.meta_server_ip, 
                                           g_option.meta_server_port);

   rep_ctrl_client_.reset(new RepInnerCtrlClient(grpc::CreateChannel(
                meta_rpc_addr,
                grpc::InsecureChannelCredentials())));
   vol_ctrl_client_.reset(new VolInnerCtrlClient(grpc::CreateChannel(
                meta_rpc_addr,
                grpc::InsecureChannelCredentials())));
}

ReplicateCtrl::~ReplicateCtrl() {
}

std::shared_ptr<ReplicateProxy> ReplicateCtrl::get_replicate_proxy(
                const string& vol_name) {
    auto it = volumes_.find(vol_name);
    if (it != volumes_.end()) {
        return it->second->get_replicate_proxy();
    }
    LOG_ERROR << "get_replicate_proxy vol-id:" << vol_name << " failed";
    return nullptr;
}

std::shared_ptr<JournalWriter> ReplicateCtrl::get_journal_writer(
        const string& vol_name) {
    auto it = volumes_.find(vol_name);
    if (it != volumes_.end()) {
        return it->second->get_writer();
    }
    LOG_ERROR << "get_journal_writer vol-id:" << vol_name << " failed";
    return nullptr;
}

void ReplicateCtrl::update_volume_attr(const string& volume) {
    VolumeInfo volume_info;
    StatusCode ret = vol_ctrl_client_->get_volume(volume, volume_info);
    if (ret == StatusCode::sOk)
    {
        auto it = volumes_.find(volume);
        if(it == volumes_.end()){
            LOG_ERROR << "volume[" << volume << "] not found when update attr!";
        }
        else{
            it->second->update_volume_attr(volume_info);
        }
    }
    else{
        LOG_ERROR << "get volume[" << volume << "] info failed!";
    }
}

StatusCode ReplicateCtrl::do_replicate_operation(const string& vol,
        const string& op_id, const RepRole& role, const ReplicateOperation& op,
        const string& snap_name){
    StatusCode res = sOk;
    // 1. hold producer marker
    //  for enable, here hold producer marker to confirm that the replicator
    //  should not replicate any data written before the snapshot synced
    std::shared_ptr<JournalWriter> writer = get_journal_writer(vol);
    if(writer == nullptr){
        LOG_ERROR << "hold producer marker failed, vol[" << vol
            << "] not found.";
        return (StatusCode::sInternalError);
    }

    std::shared_ptr<ReplicateProxy> rep_proxy = get_replicate_proxy(vol);
    if(rep_proxy == nullptr){
        LOG_ERROR << "replicate proxy not found for volume: " << vol;
        return (StatusCode::sInternalError);
    }

    writer->hold_producer_marker();
    // 2. hold replayer until replicate operation was finished
    rep_proxy->add_sync_item(snap_name,"enable");

    do{
        JournalMarker marker;
        // 3. create snapshot for replication
        if(op == ReplicateOperation::REPLICATION_FAILOVER){
            /*if failover, update volum repstatus to failing over,
                reject io after snapshot entry was persisted*/
            LOG_INFO << "update rep status to failingover,volume:" << vol;
            writer->get_vol_attr().set_replicate_status(RepStatus::REP_FAILING_OVER);
            // TODO:connection should reject write io or BlockingQueue provides clear api

            res = rep_proxy->create_snapshot(snap_name,marker,op_id,
                SnapScene::FOR_REPLICATION_FAILOVER,SnapType::SNAP_REMOTE);
        }
        else{
            res = rep_proxy->create_snapshot(snap_name,marker,op_id,
                SnapScene::FOR_REPLICATION,SnapType::SNAP_LOCAL);
        }
        if(res){
            LOG_ERROR << "create snapshot[" << snap_name << "] for volume["
                << vol << "] enable failed!";
            break;
        }
        // 4. update producer marker to checkpoint/snapshot entry
        if(0 != writer->update_producer_marker(marker)){
            LOG_ERROR << "update producer marker of volume" << vol << "] failed!";
            res = sInternalError;
            break;
        }
        // 5. do replicate operation, update replication meta
        switch(op){
            case ReplicateOperation::REPLICATION_ENABLE:
                res = rep_ctrl_client_->enable_replication(op_id,
                    vol,role,marker,snap_name);
                break;
            case ReplicateOperation::REPLICATION_DISABLE:
                res = rep_ctrl_client_->disable_replication(op_id,
                    vol,role,marker,snap_name);
                break;
            case ReplicateOperation::REPLICATION_FAILOVER:
                res = rep_ctrl_client_->failover_replication(op_id,
                    vol,role,marker,true,snap_name);
                break;
            default:
                break;
        }
        if(res){
            LOG_ERROR << "do replication operation failed,vol_id=" << vol
                << ",operation type:" << op;
            break;
        }
    }while(false);

    //5. unhold replayer & producer marker
    rep_proxy->delete_sync_item(snap_name);
    writer->unhold_producer_marker();
    return res;
}

Status ReplicateCtrl::CreateReplication(ServerContext* context,
        const CreateReplicationReq* request,
        ReplicationCommonRes* response){
    const string& rep_id = request->rep_uuid();
    const string& local_vol = request->local_volume();
    const RepRole& role = request->role();
    string operate_id = boost::uuids::to_string(uuid_generator_());
    LOG_INFO << "create replication:\n"
        << "local volume:" << local_vol << "\n"
        << "replication uuid:" << rep_id << "\n"
        << "role:" << role << "\n"
        << "peer volume :" << "\n";
    std::list<string> peer_vols;
    for(int i=0;i<request->peer_volumes_size();i++){
        peer_vols.push_back(request->peer_volumes(i));
        LOG_INFO << "\t" << request->peer_volumes(i);
    }

    StatusCode res = rep_ctrl_client_->create_replication(operate_id,
        rep_id,local_vol,peer_vols,role);
    if(res){
        LOG_ERROR << "create replication failed,vol_id=" << local_vol;
    }

    update_volume_attr(local_vol);
    response->set_status(res);
    // TODO:check whether volume has old data
    return Status::OK;
}

Status ReplicateCtrl::EnableReplication(ServerContext* context,
        const EnableReplicationReq* request,
        ReplicationCommonRes* response){
    const string& local_vol = request->vol_id();
    const RepRole& role = request->role();
    StatusCode res = sOk;

    string operate_id = boost::uuids::to_string(uuid_generator_());
    LOG_INFO << "enable replication:\n"
        << "local volume:" << local_vol << "\n"
        << "operation uuid:" << operate_id << "\n"
        << "role:" << role;

    if(RepRole::REP_PRIMARY == role){
        string snap_name = operate_uuid_to_snap_name(operate_id);
        res = do_replicate_operation(local_vol,operate_id,role,
            ReplicateOperation::REPLICATION_ENABLE,snap_name);
    }
    else{
        // update replication meta directly on secondary role
        JournalMarker marker; // not use
        string snap_name; // not use
        res = rep_ctrl_client_->enable_replication(operate_id,
            local_vol,role,marker,snap_name);
        if(res){
            LOG_ERROR << "enable replication failed,vol_id=" << local_vol;
        }
    }

    update_volume_attr(local_vol);
    response->set_status(res);
    return Status::OK;
}

Status ReplicateCtrl::DisableReplication(ServerContext* context,
        const DisableReplicationReq* request,
        ReplicationCommonRes* response){
    const string& local_vol = request->vol_id();
    const RepRole& role = request->role();
    StatusCode res;

    string operate_id = boost::uuids::to_string(uuid_generator_());
    LOG_INFO << "disable replication:\n"
        << "local volume:" << local_vol << "\n"
        << "operation uuid:" << operate_id << "\n"
        << "role:" << role;

    if(RepRole::REP_PRIMARY == role){
        string snap_name = operate_uuid_to_snap_name(operate_id);
        res = do_replicate_operation(local_vol,operate_id,role,
            ReplicateOperation::REPLICATION_DISABLE,snap_name);
    }
    else{
        // update replication meta directly on secondary role
        JournalMarker marker;
        string snap_name; // not use for secondary
        res = rep_ctrl_client_->disable_replication(operate_id,
            local_vol,role,marker,snap_name);
        if(res){
            LOG_ERROR << "disable replication failed,vol_id=" << local_vol;
        }
    }

    update_volume_attr(local_vol);
    response->set_status(res);
    return Status::OK;
}

Status ReplicateCtrl::FailoverReplication(ServerContext* context,
        const FailoverReplicationReq* request,
        ReplicationCommonRes* response){
    const string& local_vol = request->vol_id();
    const RepRole& role = request->role();
    const string& cp_id = request->checkpoint_id();
    string snap_id = request->snap_id();
    StatusCode res;
    string operate_id;
    bool need_sync = false;
    if(cp_id.length() == 0 || snap_id.length() == 0){
        operate_id = boost::uuids::to_string(uuid_generator_());
        snap_id = operate_uuid_to_snap_name(operate_id);
    }
    else{
        operate_id = cp_id;
        need_sync = true;
    }
    LOG_INFO << "failover replication:\n"
        << "local volume:" << local_vol << "\n"
        << "operation uuid:" << operate_id << "\n"
        << "checkpoint uuid:" << cp_id << "\n"
        << "snapshot uuid:" << snap_id << "\n"
        << "role:" << role << "\n"
        << "need sync:" << need_sync;

    if(RepRole::REP_PRIMARY == role){
        res = do_replicate_operation(local_vol,operate_id,role,
            ReplicateOperation::REPLICATION_FAILOVER,snap_id);
    }
    else{
        JournalMarker marker; // not use
        if(need_sync){
            // create remote snapshot meta at secondary site
            std::shared_ptr<ReplicateProxy> rep_proxy = get_replicate_proxy(local_vol);
            if(nullptr != rep_proxy){
                res = rep_proxy->create_snapshot(snap_id,marker,operate_id,
                    SnapScene::FOR_REPLICATION_FAILOVER,SnapType::SNAP_REMOTE);
                if(res){
                    LOG_ERROR << "create remote snapshot meta failed for volume:"
                        << local_vol;
                }
            }
            else{
                LOG_ERROR << "replicate proxy not found for volume: " << local_vol;
            }
        }
        // update replication meta directly on secondary role
        res = rep_ctrl_client_->failover_replication(operate_id,
            local_vol,role,marker,need_sync,snap_id);
        if(res){
            LOG_ERROR << "failover replication failed,vol_id=" << local_vol;
        }
    }

    update_volume_attr(local_vol);
    response->set_status(res);
    return Status::OK;
}

Status ReplicateCtrl::ReverseReplication(ServerContext* context,
        const ReverseReplicationReq* request,
        ReplicationCommonRes* response){
    const string& local_vol = request->vol_id();
    const RepRole& role = request->role();

    string operate_id = boost::uuids::to_string(uuid_generator_());
    LOG_INFO << "reverse replication:\n"
        << "local volume:" << local_vol << "\n"
        << "operation uuid:" << operate_id << "\n"
        << "role:" << role;

    StatusCode res = rep_ctrl_client_->reverse_replication(operate_id,
        local_vol,role);
    if(res){
        LOG_ERROR << "reverse replication failed,vol_id=" << local_vol;
    }

    update_volume_attr(local_vol);
    response->set_status(res);
    return Status::OK;
}

Status ReplicateCtrl::DeleteReplication(ServerContext* context,
        const DeleteReplicationReq* request,
        ReplicationCommonRes* response){
    const string& local_vol = request->vol_id();
    const RepRole& role = request->role();

    string operate_id = boost::uuids::to_string(uuid_generator_());
    LOG_INFO << "delete replication:\n"
        << "local volume:" << local_vol << "\n"
        << "operation uuid:" << operate_id << "\n"
        << "role:" << role;

    StatusCode res = rep_ctrl_client_->delete_replication(operate_id,
        local_vol,role);
    if(res){
        LOG_ERROR << "delete replication failed,vol_id=" << local_vol;
    }

    update_volume_attr(local_vol);
    response->set_status(res);
    return Status::OK;
}
