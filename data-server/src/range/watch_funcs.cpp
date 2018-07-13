#include "range.h"

#include "server/range_server.h"
#include "watch.h"

namespace sharkstore {
namespace dataserver {
namespace range {

watchpb::DsWatchResponse *Range::WatchGetResp(const std::string &key) {
    if (is_leader_ && KeyInRange(key)) {
        auto resp = new watchpb::DsWatchResponse;
        auto ret = store_->Get(key, resp->mutable_value());
        if (ret.ok()) {
            resp->set_code(0);
        } else {
            resp->set_code(static_cast<int>(ret.code()));
        }
        return resp;
    }

    return nullptr;
}

void Range::WatchGet(common::ProtoMessage *msg, watchpb::DsWatchRequest &req) {
    errorpb::Error *err = nullptr;

    auto btime = get_micro_second();
    context_->run_status->PushTime(monitor::PrintTag::Qwait, btime - msg->begin_time);

    auto ds_resp = new watchpb::DsWatchResponse;
    auto header = ds_resp->mutable_header();
    std::string dbKey{""};
    std::string dbValue{""};
    int64_t version{0};

    FLOG_DEBUG("range[%" PRIu64 "] WatchGet begin", meta_.id());

    do {
        if (!VerifyLeader(err)) {
            break;
        }

        if( 0 != WatchCode::EncodeKv(kFuncWatchGet, meta_, req.req().mutable_kv(), dbKey, err) ) {
            break;
        }

        FLOG_DEBUG("range[%"PRIu64" %s-%s] WatchGet key:%s", meta_.id(), meta_.start_key().c_str(), meta_.end_key().c_str(), dbKey.c_str());
        
        auto epoch = req.header().range_epoch();
        bool in_range = KeyInRange(dbKey);
        bool is_equal = EpochIsEqual(epoch);

        if (!in_range) {
            if (is_equal) {
                err = KeyNotInRange(dbKey);
                break;
            }
        }

        auto btime = get_micro_second();
        
        //get from rocksdb
        auto ret = store_->Get(dbKey, &dbValue);
        context_->run_status->PushTime(monitor::PrintTag::Store,
                                       get_micro_second() - btime);

        //decode value and response to client 
        auto resp = ds_resp->mutable_resp();
        resp->set_WatchId(msg->session_id);
        resp->set_code(Status::kOk);
        resp->set_code(static_cast<int>(ret.code()));
        
        auto val = std::make_shared<std::string>();
        auto ext = std::make_shared<std::string>();
        auto evt = resp->mutable_events()->add_events();
        auto tmpKv = req.req().mutable_kv();
        //decode value
        WatchCode::DecodeKv(kFuncWatchGet, meta_, tmpKv, dbKey, dbValue, err);

        evt->set_type(PUT);
        evt->set_allocated_kv(tmpKv);
        
    } while (false);

    if (err != nullptr) {
        FLOG_WARN("range[%" PRIu64 "] WatchGet error: %s", meta_.id(),
                  err->message().c_str());
    } else {

        //add watch if client version is not equal to ds side
        auto &start_version = req.req().startVersion();
        //to do 暂不支持前缀watch
        auto &prefix = req.req().prefix();

        //decode version from value
        FLOG_DEBUG("range[%" PRIu64 "] WatchGet [%s]-%s ok.", 
                   meta_.id(), evt.kv().key(0).data(), evt.kv().value().data());
        if(start_version >= version) {
            //to do add watch
            AddKeyWatcher(dbKey, msg);
        }
    }

    context_->socket_session->SetResponseHeader(req.header(), header, err);
    context_->socket_session->Send(msg, ds_resp);
}

void Range::PureGet(common::ProtoMessage *msg, watchpb::DsKvWatchGetMultiRequest &req) {
    errorpb::Error *err = nullptr;

    auto btime = get_micro_second();
    context_->run_status->PushTime(monitor::PrintTag::Qwait, btime - msg->begin_time);

    auto ds_resp = new watchpb::DsWatchResponse;
    auto header = ds_resp->mutable_header();
    std::string dbKey{""};
    std::string dbValue{""};
    //int64_t version{0};
    uint64_t minVersion(0);
    auto prefix = req.req().prefix();

    FLOG_DEBUG("range[%" PRIu64 "] PureGet begin", meta_.id());

    do {
        if (!VerifyLeader(err)) {
            break;
        }

        auto &key = req.req().kv().key();
        if (key.empty()) {
            FLOG_WARN("range[%" PRIu64 "] PureGet error: key empty", meta_.id());
            err = KeyNotInRange(key);
            break;
        }

        //encode key
        if( 0 != WatchCode::EncodeKv(kFuncWatchGet, meta_, req.req().mutable_kv(), dbKey, err) ) {
            break;
        }

        auto epoch = req.header().range_epoch();
        bool in_range = KeyInRange(dbKey);
        bool is_equal = EpochIsEqual(epoch);

        if (!in_range) {
            if (is_equal) {
                err = KeyNotInRange(dbKey);
                break;
            }
        }

        auto resp = ds_resp->mutable_resp();
        auto btime = get_micro_second();
        Iterator *it = nullptr;
        Status::Code code;

        if (prefix) {
            //need to encode and decode
            std::shared_ptr<storage::Iterator> iterator(store_->NewIterator(dbKey, dbKey));
            uint32_t count{0};

            for (int i = 0; iterator->Valid() ; ++i) {
                count++;
                auto evt = resp->mutable_events()->add_events();
                auto kv = evt->mutable_kv();

                WatchCode::DecodeKv(kFuncPureGet, meta_, kv, dbKey, iterator->value());
                if (minVersion > kv->version()) {
                    minVersion = kv->version();
                }

                iterator->Next();
            }

            FLOG_DEBUG("range[%" PRIu64 "] PureGet ok:%d ", meta_.id(), count);
            code = kOk;
        } else {
            auto kv = resp->mutable_events()->add_events()->mutable_kv();
            auto ret = store_->Get(dbKey, &dbValue);
            //to do decode value version             
            WatchCode::DecodeKv(kFuncPureGet, meta_, kv, dbKey, dbValue);
            
            FLOG_DEBUG("range[%" PRIu64 "] PureGet code:%d msg:%s ", meta_.id(), code, ret.ToString().data());
            code = ret.code();
        }
        context_->run_status->PushTime(monitor::PrintTag::Store,
                                       get_micro_second() - btime);

        resp->set_code(static_cast<int>(code));
    } while (false);

    if (err != nullptr) {
        FLOG_WARN("range[%" PRIu64 "] PureGet error: %s", meta_.id(),
                  err->message().c_str());
    }

    context_->socket_session->SetResponseHeader(req.header(), header, err);
    context_->socket_session->Send(msg, ds_resp);
}

void Range::WatchPut(common::ProtoMessage *msg, watchpb::DsKvWatchPutRequest &req) {
    //TO DO
    errorpb::Error *err = nullptr;
    auto dbKey(std::make_shared<std::string>(""));
    auto dbValue(std::make_shared<std::string>(""));
    auto extPtr(std::make_shared<std::string>(""));
    int64_t version{0};

    auto btime = get_micro_second();
    context_->run_status->PushTime(monitor::PrintTag::Qwait, btime - msg->begin_time);

    FLOG_DEBUG("range[%" PRIu64 "] WatchPut begin", meta_.id());

    if (!CheckWriteable()) {
        auto resp = new watchpb::DsKvWatchPutResponse;
        resp->mutable_resp()->set_code(Status::kNoLeftSpace);
        return SendError(msg, req.header(), resp, nullptr);
    }

    do {
        if (!VerifyLeader(err)) {
            break;
        }

        auto kv = req.req().mutable_kv();
        if (kv->empty()) {
            FLOG_WARN("range[%" PRIu64 "] WatchPut error: key empty", meta_.id());
            err = KeyNotInRange("-");
            break;
        }

        version = version_seq_.fetch_add(1);
        //encode key
        kv->set_version(version);
        if( 0 != WatchCode::EncodeKv(kFuncWatchPut, meta_, kv, dbKey, dbValue, err) ) {
            break;
        }
        
        //increase key version
        kv->set_version(version);
        kv->clear_key();
        kv->add_key(dbKey);
        kv->set_value(dbValue);
        
        auto epoch = req.header().range_epoch();
        bool in_range = KeyInRange(dbKey);
        bool is_equal = EpochIsEqual(epoch);

        if (!in_range) {
            if (is_equal) {
                err = KeyNotInRange(dbKey);
                break;
            }
        }

        //raft propagate at first, propagate KV after encodding
        if (!WatchPutSubmit(msg, req)) {
            err = RaftFailError();
        }
        
    } while (false);

    if (err != nullptr) {
        FLOG_WARN("range[%" PRIu64 "] WatchPut error: %s", meta_.id(),
                  err->message().c_str());

        auto resp = new watchpb::DsKvWatchPutResponse;
        return SendError(msg, req.header(), resp, err);
    }

}

void Range::WatchDel(common::ProtoMessage *msg, watchpb::DsKvWatchDeleteRequest &req) {
    errorpb::Error *err = nullptr;
    auto dbKey(std::make_shared<std::string>(""));
    auto dbValue(std::make_shared<std::string>(""));
    auto extPtr(std::make_shared<std::string>(""));

    auto btime = get_micro_second();
    context_->run_status->PushTime(monitor::PrintTag::Qwait, btime - msg->begin_time);

    FLOG_DEBUG("range[%" PRIu64 "] WatchDel begin", meta_.id());

    if (!CheckWriteable()) {
        auto resp = new watchpb::DsKvWatchDeleteResponse;
        resp->mutable_resp()->set_code(Status::kNoLeftSpace);
        return SendError(msg, req.header(), resp, nullptr);
    }

    do {
        if (!VerifyLeader(err)) {
            break;
        }

        auto &kv = req.req().mutable_kv();

        if (kv->key_size() < 1) {
            FLOG_WARN("range[%" PRIu64 "] WatchDel error: key empty", meta_.id());
            err = KeyNotInRange("-");
            break;
        }
        
        if( 0 != WatchCode::EncodeKv(kFuncWatchDel, meta_, kv, dbKey, dbValue, err) ) {
            break;
        }

        auto epoch = req.header().range_epoch();
        bool in_range = KeyInRange(dbKey);
        bool is_equal = EpochIsEqual(epoch);

        if (!in_range) {
            if (is_equal) {
                err = KeyNotInRange(dbKey);
                break;
            }
        }
        //set encoding value to request
        kv->mutable_key()->clear();
        kv->add_key(std::move(dbKey));
        kv->set_value(std::move(dbValue));

        if (!WatchDeleteSubmit(msg, req)) {
            err = RaftFailError();
        }
    } while (false);

    if (err != nullptr) {
        FLOG_WARN("range[%" PRIu64 "] WatchDel error: %s", meta_.id(),
                  err->message().c_str());

        auto resp = new watchpb::DsKvWatchDeleteResponse;
        return SendError(msg, req.header(), resp, err);
    }
    
}

bool Range::WatchPutSubmit(common::ProtoMessage *msg, watchpb::DsKvWatchPutRequest &req) {
    auto &key = req.req().key();

    if (is_leader_ && KeyInRange(key)) {
        
        auto ret = SubmitCmd(msg, req, [&req](raft_cmdpb::Command &cmd) {
            cmd.set_cmd_type(raft_cmdpb::CmdType::KvWatchPut);
            cmd.set_allocated_kv_watch_put_req(req.release_req());
        });

        return ret.ok() ? true : false;
    }

    return false;
}

bool Range::WatchDeleteSubmit(common::ProtoMessage *msg,
                            watchpb::DsKvWatchDeleteRequest &req) {
    auto &key = req.req().key();

    if (is_leader_ && KeyInRange(key)) {
        auto ret = SubmitCmd(msg, req, [&req](raft_cmdpb::Command &cmd) {
            cmd.set_cmd_type(raft_cmdpb::CmdType::KvWatchDel);
            cmd.set_allocated_kv_watch_del_req(req.release_req());
        });

        return ret.ok() ? true : false;
    }

    return false;
}

Status Range::ApplyWatchPut(const raft_cmdpb::Command &cmd) {
    Status ret;

    FLOG_DEBUG("range [%" PRIu64 "]ApplyWatchPut begin", meta_.id());
    auto &req = cmd.kv_watch_put_req();
    auto dbKey = req.kv().key(0);
    auto dbValue = req.kv().value();

    errorpb::Error *err = nullptr;

    do {

        if (!KeyInRange(dbKey, err)) {
            FLOG_WARN("Apply WatchPut failed, epoch is changed");
            ret = std::move(Status(Status::kInvalidArgument, "key not int range", ""));
            break;
        }

        auto btime = get_micro_second();
        //save to db
        ret = store_->Put(dbKey, dbValue);
        context_->run_status->PushTime(monitor::PrintTag::Store,
                                       get_micro_second() - btime);

        if (!ret.ok()) {
            FLOG_ERROR("ApplyWatchPut failed, code:%d, msg:%s", ret.code(),
                       ret.ToString().data());
            break;
        }

        if (cmd.cmd_id().node_id() == node_id_) {
            auto len = static_cast<uint64_t>(req.kv().ByteSizeLong());
            CheckSplit(len);
        }
    } while (false);

    if (cmd.cmd_id().node_id() == node_id_) {
        auto resp = new watchpb::DsKvWatchPutResponse;
        SendResponse(resp, cmd, static_cast<int>(ret.code()), err);
    } else if (err != nullptr) {
        delete err;
    }

    //notify watcher
    ret = WatchNotify(PUT, req.req().kv());
    if (!ret.ok()) {
        FLOG_ERROR("WatchNotify failed, code:%d, msg:%s", ret.code(), ret.ToString().c_str());
    }
    
    return ret;
}

Status Range::ApplyWatchDel(const raft_cmdpb::Command &cmd) {
    Status ret;
    errorpb::Error *err = nullptr;

    FLOG_DEBUG("range[%" PRIu64 "] ApplyWatchDel begin", meta_.id());

    auto &req = cmd.kv_watch_del_req();

    do {
        if (!KeyInRange(req.kv().key(0), err)) {
            FLOG_WARN("ApplyWatchDel failed, epoch is changed");
            break;
        }

        auto btime = get_micro_second();
        ret = store_->Delete(req.kv().key(0));
        context_->run_status->PushTime(monitor::PrintTag::Store,
                                       get_micro_second() - btime);

        if (!ret.ok()) {
            FLOG_ERROR("ApplyWatchDel failed, code:%d, msg:%s", ret.code(),
                       ret.ToString().c_str());
            break;
        }
        // ignore delete CheckSplit
    } while (false);

    if (cmd.cmd_id().node_id() == node_id_) {
        auto resp = new watchpb::DsKvWatchDeleteResponse;
        SendResponse(resp, cmd, static_cast<int>(ret.code()), err);
    } else if (err != nullptr) {
        delete err;
    }

    //notify watcher
    uint64_t version( req.kv().version());
    
    ret = WatchNotify(DELETE, req.kv());
    if (!ret.ok()) {
        FLOG_ERROR("WatchNotify failed, code:%d, msg:%s", ret.code(), ret.ToString().c_str());
    }
    
    return ret;
}

Status Range::WatchNotify(const EventType evtType, const watchpb::WatchKeyValue& kv) {
    Status ret;

    std::shared_ptr<watchpb::WatchKeyValue> tmpKv = std::make_shared<watchpb::WatchKeyValue>();
    tmpKv->CopyFrom(kv);

    std::vector<common::ProtoMessage*> vecProtoMsg;
    auto &dbKey = tmpKv->key_size()>0?tmpKv->key(0):"NOFOUND";
    auto &dbValue = tmpKv->value();

    std::string key{""};
    std::string value{""};
    errorpb::Error *err = nullptr;

    FunctionID funcId;
    if (PUT == evtType) {
        funcId = kFuncWatchPut;
    } else if (DELETE == evtType) {
        funcId = kFuncWatchDel;
    } else {
        funcId = kFuncHeartbeat;
    }

    if( Status::kOk != WatchCode::DecodeKv(funcId, meta_, tmpKv, dbKey, dbValue, err)) {
        return Status(kUnknown, err->message(), "");
    }
    
    uint32_t watchCnt = GetKeyWatchers(vecProtoMsg, dbKey);
    if (watchCnt > 0) {
        //to do 遍历watcher 发送通知
        auto ds_resp = new watchpb::DsWatchResponse;
        auto resp = ds_resp->mutable_resp();
        resp->set_code(kOk);
        resp->set_watchId(msg->session_id);

        auto evt = resp->mutable_events()->add_events();
        evt->set_type(evtType);
        evt->set_allocated_kv(tmpKv);

        int32_t idx{0};
        for(auto &pMsg : vecProtoMsg) {
            idx++;
            FLOG_DEBUG("range[%" PRIu64 "] WatchPut-Notify[key][%s] (%d/%d)>>>[session][%lld]", 
                       meta_.id(), key, idx, watchCnt, pMsg->session_id);

            if(0 != pMsg->socket->Send(ds_resp)) {
                FLOG_ERROR("range[%" PRIu64 "] WatchPut-Notify error:[key][%s] (%d/%d)>>>[session][%lld]", 
                           meta_.id(), key, idx, watchCnt, pMsg->session_id);
            } else {
                //delete watch
                if (WATCH_OK != DelKeyWatcher(pMsg->session_id, key)) {
                    FLOG_WARN("range[%" PRIu64 "] WatchPut-Notify DelKeyWatcher WARN:[key][%s] (%d/%d)>>>[session][%lld]", 
                           meta_.id(), key, idx, watchCnt, pMsg->session_id);
                }
            }
        }
    }

    return ret;

}

}  // namespace range
}  // namespace dataserver
}  // namespace sharkstore
