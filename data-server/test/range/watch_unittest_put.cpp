﻿#include <gtest/gtest.h>
#include "helper/cpp_permission.h"

#include <fastcommon/shared_func.h>
#include "base/status.h"
#include "base/util.h"
#include "common/ds_config.h"
#include "frame/sf_util.h"
#include "proto/gen/watchpb.pb.h"
#include "proto/gen/schpb.pb.h"
#include "range/range.h"
#include "server/range_server.h"
#include "server/run_status.h"
#include "storage/store.h"

#include "helper/mock/raft_server_mock.h"
#include "helper/mock/socket_session_mock.h"
#include "range/range.h"

#include "watch/watcher.h"
#include "common/socket_base.h"

//extern void EncodeWatchKey(std::string *buf, const uint64_t &tableId, const std::vector<std::string *> &keys);


int main(int argc, char *argv[]) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

metapb::Range *genRange2();
metapb::Range *genRange1();

char level[8] = "debug";

using namespace sharkstore::dataserver;
using namespace sharkstore::dataserver::range;
using namespace sharkstore::dataserver::storage;

class SocketBaseMock: public common::SocketBase {

public:
    virtual int Send(response_buff_t *response) {
        FLOG_DEBUG("Send mock...%s", response->buff);
        return 0;
    }
};


std::string  DecodeSingleKey(const int16_t grpFlag, const std::string &encodeBuf) {
    std::vector<std::string *> vec;
    std::string key("");
    auto buf = new std::string(encodeBuf);

    watch::Watcher watcher(1, vec);
    watcher.DecodeKey(vec, encodeBuf);

        if(grpFlag) {
            for(auto it:vec) {
                key.append(*it);
            }
        } else {
            key.assign(*vec[0]);
        }

    
   //     FLOG_DEBUG("DecodeWatchKey exception(%d), %s", int(vec.size()), EncodeToHexString(*buf).c_str());
    

    if(vec.size() > 0 && key.empty())
        key.assign(*vec[0]);

    FLOG_DEBUG("DecodeKey: %s", key.c_str());
    return key;
}

class WatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        log_init2();
        set_log_level(level);

        strcpy(ds_config.rocksdb_config.path, "/tmp/sharkstore_ds_store_test_");
        strcat(ds_config.rocksdb_config.path, std::to_string(getticks()).c_str());

        sf_socket_thread_config_t config;
        sf_socket_status_t status = {0};

        socket_.Init(&config, &status);

        range_server_ = new server::RangeServer;

        context_ = new server::ContextServer;

        context_->node_id = 1;
        context_->range_server = range_server_;
        context_->socket_session = new SocketSessionMock;
        context_->raft_server = new RaftServerMock;
        context_->run_status = new server::RunStatus;

        range_server_->Init(context_);
        now = getticks();

        {
            // begin test create range
            auto msg = new common::ProtoMessage;
            schpb::CreateRangeRequest req;
            req.set_allocated_range(genRange1());

            auto len = req.ByteSizeLong();
            msg->body.resize(len);
            ASSERT_TRUE(req.SerializeToArray(msg->body.data(), len));

            range_server_->CreateRange(msg);
            ASSERT_FALSE(range_server_->ranges_.empty());

            ASSERT_TRUE(range_server_->find(1) != nullptr);

            std::vector<metapb::Range> metas;
            auto ret = range_server_->meta_store_->GetAllRange(&metas);

            ASSERT_TRUE(metas.size() == 1) << metas.size();
            // end test create range
        }

        {
            // begin test create range
            auto msg = new common::ProtoMessage;
            schpb::CreateRangeRequest req;
            req.set_allocated_range(genRange2());

            auto len = req.ByteSizeLong();
            msg->body.resize(len);
            ASSERT_TRUE(req.SerializeToArray(msg->body.data(), len));

            range_server_->CreateRange(msg);
            ASSERT_FALSE(range_server_->ranges_.empty());

            ASSERT_TRUE(range_server_->find(2) != nullptr);

            std::vector<metapb::Range> metas;
            auto ret = range_server_->meta_store_->GetAllRange(&metas);

            ASSERT_TRUE(metas.size() == 2) << metas.size();
            // end test create range
        }
    }

    void TearDown() override {
        DestroyDB(ds_config.rocksdb_config.path, rocksdb::Options());

        delete context_->range_server;
        delete context_->socket_session;
        delete context_->raft_server;
        delete context_->run_status;
        delete context_;
    }

    void justGet(const int16_t &rangeId, const std::string key1, const std::string &key2, const std::string& val, bool prefix = false)
    {
        FLOG_DEBUG("justGet...range:%d key1:%s  key2:%s  value:%s", rangeId, key1.c_str(), key2.c_str() , val.c_str());

        auto raft = static_cast<RaftMock *>(range_server_->ranges_[rangeId]->raft_.get());
        raft->ops_.leader = 1;
        range_server_->ranges_[rangeId]->setLeaderFlag(true);

        // begin test pure_get(ok)
        auto msg = new common::ProtoMessage;
        msg->expire_time = getticks() + 1000;
        msg->session_id = 1;
        msg->socket = &socket_;
        watchpb::DsKvWatchGetMultiRequest req;

        req.set_prefix(prefix);
        req.mutable_header()->set_range_id(rangeId);
        req.mutable_header()->mutable_range_epoch()->set_conf_ver(1);
        req.mutable_header()->mutable_range_epoch()->set_version(1);

        req.mutable_kv()->set_version(0);
        req.mutable_kv()->set_tableid(1);

        req.mutable_kv()->add_key(key1);
        if(!key2.empty())
            req.mutable_kv()->add_key(key2);


        auto len = req.ByteSizeLong();
        msg->body.resize(len);
        ASSERT_TRUE(req.SerializeToArray(msg->body.data(), len));

        range_server_->PureGet(msg);

        watchpb::DsKvWatchGetMultiResponse resp;
        auto session_mock = static_cast<SocketSessionMock *>(context_->socket_session);
        ASSERT_TRUE(session_mock->GetResult(&resp));

        FLOG_DEBUG("PureGet RESP:%s", resp.DebugString().c_str());

        ASSERT_FALSE(resp.header().has_error());
        EXPECT_TRUE(resp.kvs(0).value() == val);

    }

    void justPut(const int16_t &rangeId, const std::string &key1, const std::string &key2,const std::string &value)
    {
        FLOG_DEBUG("justPut...range:%d key1:%s  key2:%s  value:%s", rangeId, key1.c_str(), key2.c_str() , value.c_str());

        auto raft = static_cast<RaftMock *>(range_server_->ranges_[rangeId]->raft_.get());
        raft->ops_.leader = 1;
        range_server_->ranges_[rangeId]->setLeaderFlag(true);

        // begin test watch_get (ok)
        auto msg1 = new common::ProtoMessage;
        //put first
        msg1->expire_time = getticks() + 1000;
        msg1->session_id = 1;
        msg1->socket = &socket_;
        watchpb::DsKvWatchPutRequest req1;

        req1.mutable_header()->set_range_id(rangeId);
        req1.mutable_header()->mutable_range_epoch()->set_conf_ver(1);
        req1.mutable_header()->mutable_range_epoch()->set_version(1);

        req1.mutable_req()->mutable_kv()->add_key(key1);
        if(!key2.empty())
            req1.mutable_req()->mutable_kv()->add_key(key2);
        req1.mutable_req()->mutable_kv()->set_value(value);
        req1.mutable_req()->mutable_kv()->set_version(99);

        auto len1 = req1.ByteSizeLong();
        msg1->body.resize(len1);
        ASSERT_TRUE(req1.SerializeToArray(msg1->body.data(), len1));

        range_server_->WatchPut(msg1);
        watchpb::DsKvWatchPutResponse resp1;
        auto session_mock = static_cast<SocketSessionMock *>(context_->socket_session);
        ASSERT_TRUE(session_mock->GetResult(&resp1));
        FLOG_DEBUG("watch_put first response: %s", resp1.DebugString().c_str());

        return;
    }


protected:
    server::ContextServer *context_;
    server::RangeServer *range_server_;
    int64_t now;
    SocketBaseMock socket_;
};

metapb::Range *genRange1() {
    //watch::Watcher watcher;
    auto meta = new metapb::Range;
    
    std::vector<std::string*> keys;
    keys.clear();
    std::string keyStart("");
    std::string keyEnd("");
    std::string k1("01003"), k2("01004");

    keys.push_back(&k1);
    watch::Watcher watcher1(1, keys);
    watcher1.EncodeKey(&keyStart, 1, keys);

    keys.clear();
    keys.push_back(&k2);
    watch::Watcher watcher2(1, keys);
    watcher2.EncodeKey(&keyEnd, 1, keys);

    meta->set_id(1);
    //meta->set_start_key("01003");
    //meta->set_end_key("01004");
    meta->set_start_key(keyStart);
    meta->set_end_key(keyEnd);

    meta->mutable_range_epoch()->set_conf_ver(1);
    meta->mutable_range_epoch()->set_version(1);

    meta->set_table_id(1);

    auto peer = meta->add_peers();
    peer->set_id(1);
    peer->set_node_id(1);

    peer = meta->add_peers();
    peer->set_id(2);
    peer->set_node_id(2);

    return meta;
}

metapb::Range *genRange2() {
    //watch::Watcher watcher;
    auto meta = new metapb::Range;

    std::vector<std::string*> keys;
    keys.clear();
    std::string keyStart("");
    std::string keyEnd("");
    std::string k1("01004"), k2("01005");

    keys.push_back(&k1);
    watch::Watcher watcher1(1, keys);
    watcher1.EncodeKey(&keyStart, 1, keys);

    keys.clear();
    keys.push_back(&k2);
    watch::Watcher watcher2(1, keys);
    watcher2.EncodeKey(&keyEnd, 1, keys);

    meta->set_id(2);
    //meta->set_start_key("01004");
    //meta->set_end_key("01005");
    meta->set_start_key(keyStart);
    meta->set_end_key(keyEnd);

    meta->mutable_range_epoch()->set_conf_ver(1);
    meta->mutable_range_epoch()->set_version(1);

    meta->set_table_id(1);

    auto peer = meta->add_peers();
    peer->set_id(1);
    peer->set_node_id(1);

    return meta;
}

TEST_F(WatchTest, watch_put_group_get_group) {

    {
        // begin test watch_put group (key ok)
        FLOG_DEBUG("watch_put group mode.");
        metapb::Range* rng = new metapb::Range;
        range_server_->meta_store_->GetRange(1, rng);
        FLOG_DEBUG("RANGE1  %s---%s", EncodeToHexString(rng->start_key()).c_str(), EncodeToHexString(rng->end_key()).c_str());

        range_server_->meta_store_->GetRange(2, rng);
        FLOG_DEBUG("RANGE2  %s---%s", EncodeToHexString(rng->start_key()).c_str(), EncodeToHexString(rng->end_key()).c_str());

        for(auto i = 0; i < 110; i ++) {
            char szKey2[1000] = {0};
            sprintf(szKey2, "01004001%d", i);
            std::string key2(szKey2);
            justPut(2, "01004001", key2, "01004001:value");
            justPut(2, "0100400101", key2, "01004001:value");
        }
        /*
        for(auto i = 0; i < 110; i ++) {
            char szKey2[1000] = {0};
            sprintf(szKey2, "01004001%d", i);
            std::string key2(szKey2);
            justGet(2, "01004001", key2, "01004001:value");
        }*/
    }

    //test get group


}
