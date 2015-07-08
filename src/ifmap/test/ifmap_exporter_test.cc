/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "ifmap/ifmap_exporter.h"

#include "base/logging.h"
#include "base/task.h"
#include "base/test/task_test_util.h"
#include "db/db.h"
#include "db/db_graph.h"
#include "io/event_manager.h"
#include "ifmap/ifmap_client.h"
#include "ifmap/ifmap_link.h"
#include "ifmap/ifmap_link_table.h"
#include "ifmap/ifmap_server.h"
#include "ifmap/ifmap_server_parser.h"
#include "ifmap/ifmap_server_table.h"
#include "ifmap/ifmap_update.h"
#include "ifmap/ifmap_update_queue.h"
#include "ifmap/ifmap_update_sender.h"
#include "ifmap/test/ifmap_test_util.h"
#include "schema/bgp_schema_types.h"
#include "schema/vnc_cfg_types.h"
#include "testing/gunit.h"

#include <iostream>
#include <fstream>

using namespace boost::asio;
using namespace std;

class TestClient : public IFMapClient {
public:
    TestClient(const string &addr)
        : identifier_(addr) {
    }

    virtual const string &identifier() const {
        return identifier_;
    }

    virtual bool SendUpdate(const std::string &msg) {
        return true;
    }

private:
    string identifier_;
};

class IFMapUpdateSenderMock : public IFMapUpdateSender {
public:
    // Use the original server and its queue
    IFMapUpdateSenderMock(IFMapServer *server) : 
        IFMapUpdateSender(server, server->queue()) {
    }
    virtual void QueueActive() { return; }
    virtual void SendActive(int index) { return; }
};

class IFMapServerTest : public IFMapServer {
public:
    IFMapServerTest(DB *db, DBGraph *graph, boost::asio::io_service *io_service)
        : IFMapServer(db, graph, io_service) {
    }
    void SetSender(IFMapUpdateSender *sender) {
        sender_.reset(sender);
    }
};

class IFMapExporterTest : public ::testing::Test {
protected:
    IFMapExporterTest()
            : server_(&db_, &graph_, evm_.io_service()),
              exporter_(server_.exporter()), parser_(NULL) {
    }

    virtual void SetUp() {
        IFMapLinkTable_Init(&db_, &graph_);
        parser_ = IFMapServerParser::GetInstance("vnc_cfg");
        vnc_cfg_ParserInit(parser_);
        vnc_cfg_Server_ModuleInit(&db_, &graph_);
        bgp_schema_ParserInit(parser_);
        server_.Initialize();
    }

    virtual void TearDown() {
        server_.Shutdown();
        task_util::WaitForIdle();
        IFMapLinkTable_Clear(&db_);
        IFMapTable::ClearTables(&db_);
        task_util::WaitForIdle();
        db_.Clear();
        parser_->MetadataClear("vnc_cfg");
        evm_.Shutdown();
    }

    void IFMapMsgLink(const string &ltype, const string &rtype,
                  const string &lid, const string &rid) {
        string metadata = ltype + "-" + rtype;
        ifmap_test_util::IFMapMsgLink(&db_, ltype, lid, rtype, rid, metadata);
    }

    void IFMapMsgUnlink(const string &ltype, const string &rtype,
                  const string &lid, const string &rid) {
        string metadata = ltype + "-" + rtype;
        ifmap_test_util::IFMapMsgUnlink(&db_, ltype, lid, rtype, rid, metadata);
    }

    void IFMapMsgNodeAdd(const string &type, const string &id,
                         uint64_t sequence_number, const string &metadata,
                         AutogenProperty *content) {
        ifmap_test_util::IFMapMsgNodeAdd(&db_, type, id, sequence_number,
                                         metadata, content);
    }

    void IFMapMsgNodeDelete(const string &type, const string &id,
                         uint64_t sequence_number, const string &metadata,
                         AutogenProperty *content) {
        ifmap_test_util::IFMapMsgNodeDelete(&db_, type, id, sequence_number,
                                         metadata, content);
    }

    IFMapNode *TableLookup(const string &type, const string &name) {
        IFMapTable *tbl = IFMapTable::FindTable(&db_, type);
        if (tbl == NULL) {
            return NULL;
        }
        return tbl->FindNode(name);
    }

    IFMapLink *LinkTableLookup(const string &name) {
        IFMapLinkTable *table = static_cast<IFMapLinkTable *>
            (db_.FindTable("__ifmap_metadata__.0"));
        if (table == NULL) {
            return NULL;
        }
        return table->FindLink(name);
    }

    size_t LinkTableSize() {
        IFMapLinkTable *table = static_cast<IFMapLinkTable *>
            (db_.FindTable("__ifmap_metadata__.0"));
        if (table == NULL) {
            return 0;
        }
        return table->Size();
    }

    // Read all the updates in the queue and consider them sent.
    void ProcessQueue() {
        IFMapUpdateQueue *queue = server_.queue();
        IFMapListEntry *next = NULL;
        for (IFMapListEntry *iter = queue->tail_marker(); iter != NULL;
             iter = next) {
            next = queue->Next(iter);
            if (iter->type == IFMapListEntry::MARKER) {
                continue;
            }
            IFMapUpdate *update = static_cast<IFMapUpdate *>(iter);
            BitSet adv = update->advertise();
            update->AdvertiseReset(update->advertise());
            queue->Dequeue(update);
            exporter_->StateUpdateOnDequeue(update, adv, update->IsDelete());
        }
    }

    DB db_;
    DBGraph graph_;
    EventManager evm_;
    IFMapServerTest server_;
    IFMapExporter *exporter_;
    IFMapServerParser *parser_;
};

static string FileRead(const string &filename) {
    ifstream file(filename.c_str());
    string content((istreambuf_iterator<char>(file)),
                   istreambuf_iterator<char>());
    return content;
}

TEST_F(IFMapExporterTest, Basic) {
    server_.SetSender(new IFMapUpdateSenderMock(&server_));
    TestClient c1("192.168.1.1");
    server_.ClientRegister(&c1);

    IFMapMsgLink("domain", "project", "user1", "vnc");
    IFMapMsgLink("project", "virtual-network", "vnc", "blue");
    IFMapMsgLink("project", "virtual-network", "vnc", "red");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_x", "vm_x:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_x:veth0", "blue");

    task_util::WaitForIdle();
    IFMapNode *idn = TableLookup("virtual-network", "blue");
    ASSERT_TRUE(idn != NULL);
    IFMapNodeState *state = exporter_->NodeStateLookup(idn);
    if (state != NULL) {
        EXPECT_TRUE(state->interest().empty());
    }

    IFMapMsgLink("virtual-router", "virtual-machine",
                 "192.168.1.1", "vm_x");

    task_util::WaitForIdle();
    
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    EXPECT_FALSE(state->interest().empty());

    IFMapMsgUnlink("virtual-router", "virtual-machine",
                   "192.168.1.1", "vm_x");

    task_util::WaitForIdle();

    idn = TableLookup("virtual-network", "blue");
    if (idn != NULL) {
        state = exporter_->NodeStateLookup(idn);
        if (state != NULL) {
            EXPECT_TRUE(state->interest().empty());
            EXPECT_TRUE(state->update_list().empty());
        }
    }
}

// interest change: subgraph was to be sent to a subset of peers and that
// subset changes (overlapping and non overlapping case).
TEST_F(IFMapExporterTest, InterestChangeIntersect) {
    server_.SetSender(new IFMapUpdateSenderMock(&server_));
    TestClient c1("192.168.1.1");
    TestClient c2("192.168.1.2");
    TestClient c3("192.168.1.3");
    TestClient c4("192.168.1.4");

    server_.ClientRegister(&c1);
    server_.ClientRegister(&c2);
    server_.ClientRegister(&c3);
    server_.ClientRegister(&c4);

    IFMapMsgLink("domain", "project", "user1", "vnc");
    IFMapMsgLink("project", "virtual-network", "vnc", "blue");
    IFMapMsgLink("project", "virtual-network", "vnc", "red");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_x", "vm_x:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_x:veth0", "blue");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_w", "vm_w:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_w:veth0", "red");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_y", "vm_y:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_y:veth0", "blue");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_z", "vm_z:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_z:veth0", "red");

    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.1", "vm_x");
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.2", "vm_w");
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.3", "vm_y");
    task_util::WaitForIdle();

    IFMapNode *blue = TableLookup("virtual-network", "blue");
    ASSERT_TRUE(blue != NULL);
    IFMapNodeState *state = exporter_->NodeStateLookup(blue);
    ASSERT_TRUE(state != NULL);

    IFMapUpdate *update = state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(update != NULL);
    TASK_UTIL_EXPECT_TRUE(update->advertise().test(c1.index()));
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c2.index()));
    TASK_UTIL_EXPECT_TRUE(update->advertise().test(c3.index()));
    // Call ProcessQueue() since our QueueActive() does not do anything
    ProcessQueue();

    IFMapMsgUnlink("virtual-router", "virtual-machine", "192.168.1.2", "vm_w");
    IFMapMsgUnlink("virtual-router", "virtual-machine", "192.168.1.3", "vm_y");
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.4", "vm_z");
    task_util::WaitForIdle();

    // Check that only c3 will receive a delete for blue.
    state = exporter_->NodeStateLookup(blue);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::DELETE) != NULL);
    update = state->GetUpdate(IFMapListEntry::DELETE);
    ASSERT_TRUE(update != NULL);
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c1.index()));
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c2.index()));
    TASK_UTIL_EXPECT_TRUE(update->advertise().test(c3.index()));
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c4.index()));

    // Check that only c4 will receive an add for red.
    IFMapNode *red = TableLookup("virtual-network", "red");
    ASSERT_TRUE(red != NULL);
    state = exporter_->NodeStateLookup(red);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(update != NULL);
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c1.index()));
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c2.index()));
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c3.index()));
    TASK_UTIL_EXPECT_TRUE(update->advertise().test(c4.index()));

    // Check that only c2 will receive a delete for red.
    state = exporter_->NodeStateLookup(red);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::DELETE) != NULL);
    update = state->GetUpdate(IFMapListEntry::DELETE);
    ASSERT_TRUE(update != NULL);
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c1.index()));
    TASK_UTIL_EXPECT_TRUE(update->advertise().test(c2.index()));
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c3.index()));
    TASK_UTIL_EXPECT_FALSE(update->advertise().test(c4.index()));

    // Check that there will be no update for blue.
    state = exporter_->NodeStateLookup(blue);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);

    // Call ProcessQueue() since our QueueActive() does not do anything
    ProcessQueue();

    IFMapMsgUnlink("virtual-machine-interface", "virtual-network",
                   "vm_z:veth0", "red");
    IFMapMsgLink("virtual-machine-interface", "virtual-network",
                 "vm_z:veth0", "blue");
    task_util::WaitForIdle();

    state = exporter_->NodeStateLookup(blue);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(update != NULL);
    EXPECT_FALSE(update->advertise().test(c1.index()));
    EXPECT_FALSE(update->advertise().test(c2.index()));
    EXPECT_FALSE(update->advertise().test(c3.index()));
    EXPECT_TRUE(update->advertise().test(c4.index()));

    state = exporter_->NodeStateLookup(red);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::DELETE) != NULL);
    update = state->GetUpdate(IFMapListEntry::DELETE);
    ASSERT_TRUE(update != NULL);
    EXPECT_FALSE(update->advertise().test(c1.index()));
    EXPECT_FALSE(update->advertise().test(c2.index()));
    EXPECT_FALSE(update->advertise().test(c3.index()));
    EXPECT_TRUE(update->advertise().test(c4.index()));

    // Call ProcessQueue() since our QueueActive() does not do anything
    ProcessQueue();

    red = TableLookup("virtual-network", "red");
    ASSERT_TRUE(red != NULL);
    state = exporter_->NodeStateLookup(red);
    ASSERT_TRUE(state != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update == NULL);
    update = state->GetUpdate(IFMapListEntry::DELETE);
    EXPECT_TRUE(update == NULL);

    blue = TableLookup("virtual-network", "blue");
    ASSERT_TRUE(blue != NULL);
    state = exporter_->NodeStateLookup(blue);
    ASSERT_TRUE(state != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update == NULL);
    update = state->GetUpdate(IFMapListEntry::DELETE);
    EXPECT_TRUE(update == NULL);
}

// Verify dependency on add.
TEST_F(IFMapExporterTest, NodeAddDependency) {
    server_.SetSender(new IFMapUpdateSenderMock(&server_));
    TestClient c1("192.168.1.1");
    server_.ClientRegister(&c1);
    
    IFMapMsgLink("domain", "project", "user1", "vnc");
    IFMapMsgLink("project", "virtual-network", "vnc", "blue");
    IFMapMsgLink("project", "virtual-network", "vnc", "red");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_x", "vm_x:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_x:veth0", "blue");

    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.1", "vm_x");
    task_util::WaitForIdle();

    IFMapUpdateQueue *queue = server_.queue();
    
    set<IFMapNode *> seen;
    for (IFMapListEntry *iter = queue->tail_marker(); iter != NULL;
         iter = queue->Next(iter)) {
        if (iter->type == IFMapListEntry::MARKER) {
            continue;
        }
        IFMapUpdate *update = static_cast<IFMapUpdate *>(iter);
        EXPECT_TRUE(update->type == IFMapListEntry::UPDATE);
        if (update->data().type == IFMapObjectPtr::NODE) {
            seen.insert(update->data().u.node);
        } else {
            IFMapLink *link = update->data().u.link;
            EXPECT_TRUE(seen.find(link->left()) != seen.end())
                << link->ToString() << " before " << link->left()->ToString();
            EXPECT_TRUE(seen.find(link->right()) != seen.end())
                << link->ToString() << " before " << link->right()->ToString();
        }
    }
    EXPECT_EQ(4, seen.size());
}

// Link is deleted.
TEST_F(IFMapExporterTest, LinkDeleteDependency) {
    server_.SetSender(new IFMapUpdateSenderMock(&server_));
    TestClient c1("192.168.1.1");
    server_.ClientRegister(&c1);
 
    IFMapMsgLink("domain", "project", "user1", "vnc");
    IFMapMsgLink("project", "virtual-network", "vnc", "blue");
    IFMapMsgLink("project", "virtual-network", "vnc", "red");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_x", "vm_x:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_x:veth0", "blue");
    
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.1", "vm_x");
    task_util::WaitForIdle();
 
    // Call ProcessQueue() since our QueueActive() does not do anything
    ProcessQueue();
    IFMapMsgUnlink("virtual-router", "virtual-machine", "192.168.1.1", "vm_x");
    task_util::WaitForIdle();

    set<IFMapNode *> seen;
    IFMapUpdateQueue *queue = server_.queue();
    for (IFMapListEntry *iter = queue->tail_marker(); iter != NULL;
         iter = queue->Next(iter)) {
        if (iter->type == IFMapListEntry::MARKER) {
            continue;
        }
        IFMapUpdate *update = static_cast<IFMapUpdate *>(iter);
        EXPECT_TRUE(update->type == IFMapListEntry::DELETE);
        if (update->data().type == IFMapObjectPtr::NODE) {
            seen.insert(update->data().u.node);
        } else {
            IFMapLink *link = update->data().u.link;
            EXPECT_TRUE(seen.find(link->left()) == seen.end())
                << link->ToString() << " after " << link->left()->ToString();
            EXPECT_TRUE(seen.find(link->right()) == seen.end())
                << link->ToString() << " after " << link->right()->ToString();
        }
    }
}

TEST_F(IFMapExporterTest, CrcChecks) {
    // Round 1 of reading config
    string content = FileRead("controller/src/ifmap/testdata/crc.xml");
    assert(content.size() != 0);
    parser_->Receive(&db_, content.c_str(), content.size(), 0);
    task_util::WaitForIdle();

    IFMapNode *idn = TableLookup("virtual-router", "host1");
    ASSERT_TRUE(idn != NULL);
    IFMapNodeState *state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_uuid1 = state->crc();

    idn = TableLookup("virtual-router", "host2");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_perm1 = state->crc();

    idn = TableLookup("virtual-router", "host3");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_bool1 = state->crc();

    idn = TableLookup("virtual-router", "host4");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_string1 = state->crc();

    idn = TableLookup("virtual-router", "host5");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_idperms1 = state->crc();

    idn = TableLookup("network-policy", "policy1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_np_vec_complex1 = state->crc();

    idn = TableLookup("virtual-machine-interface", "vm1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_vm_vec_simple1 = state->crc();

    // Round 2 of reading config
    content = FileRead("controller/src/ifmap/testdata/crc1.xml");
    assert(content.size() != 0);
    parser_->Receive(&db_, content.c_str(), content.size(), 0);
    task_util::WaitForIdle();

    idn = TableLookup("virtual-router", "host1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_uuid2 = state->crc();

    idn = TableLookup("virtual-router", "host2");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_perm2 = state->crc();

    idn = TableLookup("virtual-router", "host3");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_bool2 = state->crc();

    idn = TableLookup("virtual-router", "host4");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_string2 = state->crc();

    idn = TableLookup("virtual-router", "host5");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_idperms2 = state->crc();

    idn = TableLookup("network-policy", "policy1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_np_vec_complex2 = state->crc();

    idn = TableLookup("virtual-machine-interface", "vm1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_vm_vec_simple2 = state->crc();

    ASSERT_TRUE(crc_uuid1 != crc_uuid2);
    ASSERT_TRUE(crc_perm1 != crc_perm2);
    ASSERT_TRUE(crc_bool1 != crc_bool2);
    ASSERT_TRUE(crc_string1 != crc_string2);
    // both should be same since config is same
    ASSERT_TRUE(crc_idperms1 == crc_idperms2);
    ASSERT_TRUE(crc_np_vec_complex1 != crc_np_vec_complex2);
    ASSERT_TRUE(crc_vm_vec_simple1 != crc_vm_vec_simple2);

    // Round 3 of reading config
    // Read crc.xml again. After reading, all the crc's should match with the
    // crc's calculated during round 1.
    content = FileRead("controller/src/ifmap/testdata/crc.xml");
    assert(content.size() != 0);
    parser_->Receive(&db_, content.c_str(), content.size(), 0);
    task_util::WaitForIdle();

    idn = TableLookup("virtual-router", "host1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_uuid3 = state->crc();

    idn = TableLookup("virtual-router", "host2");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_perm3 = state->crc();

    idn = TableLookup("virtual-router", "host3");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_bool3 = state->crc();

    idn = TableLookup("virtual-router", "host4");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_string3 = state->crc();

    idn = TableLookup("virtual-router", "host5");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_idperms3 = state->crc();

    idn = TableLookup("network-policy", "policy1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_np_vec_complex3 = state->crc();

    idn = TableLookup("virtual-machine-interface", "vm1");
    ASSERT_TRUE(idn != NULL);
    state = exporter_->NodeStateLookup(idn);
    ASSERT_TRUE(state != NULL);
    IFMapState::crc32type crc_vm_vec_simple3 = state->crc();

    ASSERT_TRUE(crc_uuid1 == crc_uuid3);
    ASSERT_TRUE(crc_perm1 == crc_perm3);
    ASSERT_TRUE(crc_bool1 == crc_bool3);
    ASSERT_TRUE(crc_string1 == crc_string3);
    ASSERT_TRUE(crc_idperms1 == crc_idperms3);
    ASSERT_TRUE(crc_np_vec_complex1 == crc_np_vec_complex3);
    ASSERT_TRUE(crc_vm_vec_simple1 == crc_vm_vec_simple3);
}

TEST_F(IFMapExporterTest, ChangePropertiesIncrementally) {
    server_.SetSender(new IFMapUpdateSenderMock(&server_));
    TestClient c1("vr-test");
    server_.ClientRegister(&c1);

    IFMapMsgLink("domain", "project", "user1", "vnc");
    IFMapMsgLink("project", "virtual-network", "vnc", "blue");
    IFMapMsgLink("project", "virtual-network", "vnc", "red");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface", 
                 "vm_x", "vm_x:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network", 
                 "vm_x:veth0", "blue");
    IFMapMsgLink("virtual-router", "virtual-machine", "vr-test", "vm_x");
    task_util::WaitForIdle();

    // Check that c1's advertise bit is set
    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-router", "vr-test") != NULL);
    IFMapNode *vrnode = TableLookup("virtual-router", "vr-test");
    TASK_UTIL_EXPECT_TRUE(exporter_->NodeStateLookup(vrnode) != NULL);
    IFMapNodeState *state = exporter_->NodeStateLookup(vrnode);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    IFMapUpdate *update = state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(update != NULL);
    EXPECT_TRUE(update->advertise().test(c1.index()));
    IFMapState::crc32type crc0 = state->crc();
    ProcessQueue();
    EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
 
    // Add the 'id-perms' property
    autogen::IdPermsType *prop1 = new autogen::IdPermsType();
    IFMapMsgNodeAdd("virtual-router", "vr-test", 1, "id-perms", prop1);
    task_util::WaitForIdle();
    state = exporter_->NodeStateLookup(vrnode);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update->advertise().test(c1.index()));
    IFMapState::crc32type crc1 = state->crc();
    ProcessQueue();
    EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    // Checks. Only 'id-perms' should be set.
    vrnode = TableLookup("virtual-router", "vr-test");
    ASSERT_TRUE(vrnode != NULL);
    TASK_UTIL_EXPECT_TRUE(
        vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER)) != NULL);
    IFMapObject *obj = vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER));
    ASSERT_TRUE(obj != NULL);
    autogen::VirtualRouter *vrobj = dynamic_cast<autogen::VirtualRouter *>(obj);
    ASSERT_TRUE(vrobj != NULL);
    EXPECT_TRUE(vrobj->IsPropertySet(autogen::VirtualRouter::ID_PERMS));
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::DISPLAY_NAME));
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::IP_ADDRESS));
    ASSERT_TRUE(crc0 != crc1);

    // Add the 'display-name' property
    autogen::VirtualRouter::StringProperty *prop2 =
        new autogen::VirtualRouter::StringProperty();
    prop2->data = "myDisplayName";
    IFMapMsgNodeAdd("virtual-router", "vr-test", 1, "display-name", prop2);
    task_util::WaitForIdle();
    state = exporter_->NodeStateLookup(vrnode);
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update->advertise().test(c1.index()));
    IFMapState::crc32type crc2 = state->crc();
    ProcessQueue();
    EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    // Checks. 'id-perms' and 'display-name' should be set.
    vrnode = TableLookup("virtual-router", "vr-test");
    ASSERT_TRUE(vrnode != NULL);
    EXPECT_TRUE(vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER)) != NULL);
    obj = vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER));
    ASSERT_TRUE(obj != NULL);
    vrobj = dynamic_cast<autogen::VirtualRouter *>(obj);
    ASSERT_TRUE(vrobj != NULL);
    EXPECT_TRUE(vrobj->IsPropertySet(autogen::VirtualRouter::ID_PERMS));
    EXPECT_TRUE(vrobj->IsPropertySet(autogen::VirtualRouter::DISPLAY_NAME));
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::IP_ADDRESS));
    ASSERT_TRUE(crc1 != crc2);

    // Remove the 'display-name' property
    prop2 = new autogen::VirtualRouter::StringProperty();
    prop2->data = "myDisplayName";
    IFMapMsgNodeDelete("virtual-router", "vr-test", 1, "display-name", prop2);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update->advertise().test(c1.index()));
    IFMapState::crc32type crc3 = state->crc();
    ProcessQueue();
    EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    // Checks. Only 'id-perms' should be set.
    vrnode = TableLookup("virtual-router", "vr-test");
    ASSERT_TRUE(vrnode != NULL);
    EXPECT_TRUE(vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER)) != NULL);
    obj = vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER));
    ASSERT_TRUE(obj != NULL);
    vrobj = dynamic_cast<autogen::VirtualRouter *>(obj);
    ASSERT_TRUE(vrobj != NULL);
    EXPECT_TRUE(vrobj->IsPropertySet(autogen::VirtualRouter::ID_PERMS));
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::DISPLAY_NAME));
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::IP_ADDRESS));
    ASSERT_TRUE(crc2 != crc3);
    ASSERT_TRUE(crc1 == crc3);

    // Add the 'display-name' property again
    prop2 = new autogen::VirtualRouter::StringProperty();
    prop2->data = "myDisplayName";
    IFMapMsgNodeAdd("virtual-router", "vr-test", 1, "display-name", prop2);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update->advertise().test(c1.index()));
    IFMapState::crc32type crc4 = state->crc();
    ProcessQueue();
    EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    // Checks. 'id-perms' and 'display-name' should be set.
    vrnode = TableLookup("virtual-router", "vr-test");
    ASSERT_TRUE(vrnode != NULL);
    EXPECT_TRUE(vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER)) != NULL);
    obj = vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER));
    ASSERT_TRUE(obj != NULL);
    vrobj = dynamic_cast<autogen::VirtualRouter *>(obj);
    ASSERT_TRUE(vrobj != NULL);
    EXPECT_TRUE(vrobj->IsPropertySet(autogen::VirtualRouter::ID_PERMS));
    EXPECT_TRUE(vrobj->IsPropertySet(autogen::VirtualRouter::DISPLAY_NAME));
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::IP_ADDRESS));
    ASSERT_TRUE(crc3 != crc4);
    ASSERT_TRUE(crc2 == crc4);

    // Remove the 'id-perms' property
    prop1 = new autogen::IdPermsType();
    IFMapMsgNodeDelete("virtual-router", "vr-test", 1, "id-perms", prop1);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update->advertise().test(c1.index()));
    IFMapState::crc32type crc5 = state->crc();
    ProcessQueue();
    EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    // Checks. Only 'display-name' should be set.
    vrnode = TableLookup("virtual-router", "vr-test");
    ASSERT_TRUE(vrnode != NULL);
    EXPECT_TRUE(vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER)) != NULL);
    obj = vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER));
    ASSERT_TRUE(obj != NULL);
    vrobj = dynamic_cast<autogen::VirtualRouter *>(obj);
    ASSERT_TRUE(vrobj != NULL);
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::ID_PERMS));
    EXPECT_TRUE(vrobj->IsPropertySet(autogen::VirtualRouter::DISPLAY_NAME));
    EXPECT_FALSE(vrobj->IsPropertySet(autogen::VirtualRouter::IP_ADDRESS));
    ASSERT_TRUE(crc4 != crc5);

    // Remove the 'display-name' property
    prop2 = new autogen::VirtualRouter::StringProperty();
    prop2->data = "myDisplayName";
    IFMapMsgNodeDelete("virtual-router", "vr-test", 1, "display-name", prop2);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    update = state->GetUpdate(IFMapListEntry::UPDATE);
    EXPECT_TRUE(update->advertise().test(c1.index()));
    IFMapState::crc32type crc6 = state->crc();
    ProcessQueue();
    EXPECT_TRUE(state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    // Checks. The node should exist since it has a neighbor. But, the object
    // should be gone since all the properties are gone.
    vrnode = TableLookup("virtual-router", "vr-test");
    ASSERT_TRUE(vrnode != NULL);
    TASK_UTIL_EXPECT_TRUE(
            vrnode->Find(IFMapOrigin(IFMapOrigin::MAP_SERVER)) == NULL);
    ASSERT_TRUE(crc5 != crc6);
    ASSERT_TRUE(crc0 == crc6);
}

// Create links between VM and IPAM, one with both nodes having the same name
// and another with them having different names.
TEST_F(IFMapExporterTest, PR1383393) {
    std::string samename = "samename";
    std::string name1 = "name1";
    std::string name2 = "name2";
    IFMapMsgLink("virtual-network", "network-ipam", samename, samename);
    IFMapMsgLink("virtual-network", "network-ipam", name1, name2);

    IFMapTable *vn_tbl = IFMapTable::FindTable(&db_, "virtual-network");
    TASK_UTIL_EXPECT_EQ(2, vn_tbl->Size());
    IFMapTable *ni_tbl = IFMapTable::FindTable(&db_, "network-ipam");
    TASK_UTIL_EXPECT_EQ(2, ni_tbl->Size());
    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-network", samename) != NULL);
    TASK_UTIL_EXPECT_TRUE(TableLookup("network-ipam", samename) != NULL);
    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-network", name1) != NULL);
    TASK_UTIL_EXPECT_TRUE(TableLookup("network-ipam", name2) != NULL);

    IFMapMsgUnlink("virtual-network", "network-ipam", samename, samename);
    IFMapMsgUnlink("virtual-network", "network-ipam", name1, name2);

    TASK_UTIL_EXPECT_EQ(0, vn_tbl->Size());
    TASK_UTIL_EXPECT_EQ(0, ni_tbl->Size());
    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-network", samename) == NULL);
    TASK_UTIL_EXPECT_TRUE(TableLookup("network-ipam", samename) == NULL);
    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-network", name1) == NULL);
    TASK_UTIL_EXPECT_TRUE(TableLookup("network-ipam", name2) == NULL);
}

// Delete-link followed by add-link before delete-link completely cleaned up
// the link.
TEST_F(IFMapExporterTest, PR1454380) {
    server_.SetSender(new IFMapUpdateSenderMock(&server_));
    TestClient c1("vr-test");
    server_.ClientRegister(&c1);

    IFMapMsgLink("domain", "project", "user1", "vnc");
    IFMapMsgLink("project", "virtual-network", "vnc", "blue");
    IFMapMsgLink("project", "virtual-network", "vnc", "red");
    IFMapMsgLink("virtual-machine", "virtual-machine-interface",
                 "vm_x", "vm_x:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network",
                 "vm_x:veth0", "blue");
    IFMapMsgLink("virtual-router", "virtual-machine", "vr-test", "vm_x");
    task_util::WaitForIdle();

    IFMapLinkTable *link_table = static_cast<IFMapLinkTable *>
        (db_.FindTable("__ifmap_metadata__.0"));
    if (link_table == NULL) {
        assert(false);
    }

    // Check node, state and update for VR.
    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-router", "vr-test") != NULL);
    IFMapNode *vr_node = TableLookup("virtual-router", "vr-test");
    TASK_UTIL_EXPECT_TRUE(exporter_->NodeStateLookup(vr_node) != NULL);
    IFMapNodeState *vr_state = exporter_->NodeStateLookup(vr_node);
    TASK_UTIL_EXPECT_TRUE(vr_state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    IFMapUpdate *vr_update = vr_state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(vr_update != NULL);
    TASK_UTIL_EXPECT_TRUE(vr_update->advertise().test(c1.index()));

    // Check node, state and update for VM.
    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-machine", "vm_x") != NULL);
    IFMapNode *vm_node = TableLookup("virtual-machine", "vm_x");
    TASK_UTIL_EXPECT_TRUE(exporter_->NodeStateLookup(vm_node) != NULL);
    IFMapNodeState *vm_state = exporter_->NodeStateLookup(vm_node);
    TASK_UTIL_EXPECT_TRUE(vm_state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    IFMapUpdate *vm_update = vm_state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(vm_update != NULL);
    TASK_UTIL_EXPECT_TRUE(vm_update->advertise().test(c1.index()));

    // Check node, state and update for link VR-VM.
    string link_name = link_table->LinkKey("virtual-router-virtual-machine",
                                           vr_node, vm_node);
    ASSERT_TRUE(link_name.size() != 0);
    TASK_UTIL_EXPECT_TRUE(LinkTableLookup(link_name) != NULL);
    IFMapLink *vr_vm_link = LinkTableLookup(link_name);
    ASSERT_TRUE(vr_vm_link != NULL);
    TASK_UTIL_EXPECT_TRUE(exporter_->LinkStateLookup(vr_vm_link) != NULL);
    IFMapLinkState *link_state = exporter_->LinkStateLookup(vr_vm_link);
    ASSERT_TRUE(link_state != NULL);
    TASK_UTIL_EXPECT_TRUE(link_state->GetUpdate(IFMapListEntry::UPDATE) != NULL);
    IFMapUpdate *link_update = link_state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(link_update != NULL);
    TASK_UTIL_EXPECT_TRUE(link_update->advertise().test(c1.index()));

    // Now drain the Q.
    ProcessQueue();
    EXPECT_TRUE(vr_state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    EXPECT_TRUE(vm_state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    EXPECT_TRUE(link_state->GetUpdate(IFMapListEntry::UPDATE) == NULL);

    // Delete the link between VR-VM but dont process the Q. The delete-update
    // should remain in the state's list.
    EXPECT_TRUE(link_state->GetUpdate(IFMapListEntry::DELETE) == NULL);
    IFMapMsgUnlink("virtual-router", "virtual-machine", "vr-test", "vm_x");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(
        link_state->GetUpdate(IFMapListEntry::DELETE) != NULL);
    link_update = link_state->GetUpdate(IFMapListEntry::DELETE);
    ASSERT_TRUE(link_update != NULL);
    TASK_UTIL_EXPECT_TRUE(link_update->advertise().test(c1.index()));
    link_state = exporter_->LinkStateLookup(vr_vm_link);
    EXPECT_TRUE(link_state->IsInvalid());
    EXPECT_FALSE(link_state->HasDependency());

    // We have not processed the Q and so that delete-update is still in the
    // queue. Add the VR-VM link again. Since, advertised and interest are the
    // same, add-update will not be added and delete-update will be dequeued.
    link_update = link_state->GetUpdate(IFMapListEntry::UPDATE);
    ASSERT_TRUE(link_update == NULL);
    IFMapMsgLink("virtual-router", "virtual-machine", "vr-test", "vm_x");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(link_state->IsValid());
    TASK_UTIL_EXPECT_TRUE(link_state->HasDependency());
    TASK_UTIL_EXPECT_TRUE(
        link_state->GetUpdate(IFMapListEntry::UPDATE) == NULL);
    TASK_UTIL_EXPECT_TRUE(
        link_state->GetUpdate(IFMapListEntry::DELETE) == NULL);
}

TEST_F(IFMapExporterTest, ConfigTracker) {
    server_.SetSender(new IFMapUpdateSenderMock(&server_));
    TestClient c1("192.168.1.1");
    TestClient c2("192.168.1.2");
    TestClient c3("192.168.1.3");
    TestClient c4("192.168.1.4");

    server_.ClientRegister(&c1);
    server_.ClientRegister(&c2);
    server_.ClientRegister(&c3);
    server_.ClientRegister(&c4);

    IFMapMsgLink("domain", "project", "user1", "vnc");
    IFMapMsgLink("project", "virtual-network", "vnc", "blue");
    // vm-vmi and vmi-vn for c1.
    IFMapMsgLink("virtual-machine", "virtual-machine-interface",
                 "vm_c1", "vm_c1:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network",
                 "vm_c1:veth0", "blue");
    // vm-vmi and vmi-vn for c2.
    IFMapMsgLink("virtual-machine", "virtual-machine-interface",
                 "vm_c2", "vm_c2:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network",
                 "vm_c2:veth0", "blue");
    // vm-vmi and vmi-vn for c3.
    IFMapMsgLink("virtual-machine", "virtual-machine-interface",
                 "vm_c3", "vm_c3:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network",
                 "vm_c3:veth0", "blue");
    // vm-vmi and vmi-vn for c4.
    IFMapMsgLink("virtual-machine", "virtual-machine-interface",
                 "vm_c4", "vm_c4:veth0");
    IFMapMsgLink("virtual-machine-interface", "virtual-network",
                 "vm_c4:veth0", "blue");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(LinkTableSize(), 10);

    EXPECT_TRUE(c1.ConfigTrackerEmpty());
    EXPECT_TRUE(c2.ConfigTrackerEmpty());
    EXPECT_TRUE(c3.ConfigTrackerEmpty());
    EXPECT_TRUE(c4.ConfigTrackerEmpty());

    // Add the vr-vm link for c1. The state for VN 'blue' must have c1.
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.1", "vm_c1");
    task_util::WaitForIdle();

    TASK_UTIL_EXPECT_TRUE(TableLookup("virtual-network", "blue") != NULL);
    IFMapNode *blue = TableLookup("virtual-network", "blue");
    ASSERT_TRUE(blue != NULL);
    TASK_UTIL_EXPECT_TRUE(exporter_->NodeStateLookup(blue) != NULL);
    IFMapNodeState *state = exporter_->NodeStateLookup(blue);
    ASSERT_TRUE(state != NULL);
    TASK_UTIL_EXPECT_TRUE(state->interest().test(c1.index()));
    TASK_UTIL_EXPECT_TRUE(c1.ConfigTrackerHasState(state));

    // Add the vr-vm link for c2. The state for VN 'blue' must have c2.
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.2", "vm_c2");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(state->interest().test(c2.index()));
    TASK_UTIL_EXPECT_TRUE(c2.ConfigTrackerHasState(state));

    // Add the vr-vm link for c3. The state for VN 'blue' must have c3.
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.3", "vm_c3");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(state->interest().test(c3.index()));
    TASK_UTIL_EXPECT_TRUE(c3.ConfigTrackerHasState(state));

    // Add the vr-vm link for c4. The state for VN 'blue' must have c4.
    IFMapMsgLink("virtual-router", "virtual-machine", "192.168.1.4", "vm_c4");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_TRUE(state->interest().test(c4.index()));
    TASK_UTIL_EXPECT_TRUE(c4.ConfigTrackerHasState(state));

    // Check if all the bits are set for VN 'blue' and all the clients have
    // 'blue' in their config-tracker.
    EXPECT_TRUE(state->interest().test(c1.index()));
    EXPECT_TRUE(state->interest().test(c2.index()));
    EXPECT_TRUE(state->interest().test(c3.index()));
    EXPECT_TRUE(state->interest().test(c4.index()));
    EXPECT_TRUE(c1.ConfigTrackerHasState(state));
    EXPECT_TRUE(c2.ConfigTrackerHasState(state));
    EXPECT_TRUE(c3.ConfigTrackerHasState(state));
    EXPECT_TRUE(c4.ConfigTrackerHasState(state));
    // VR, VM, VMI, VN, VR-VM, VM-VMI, VMI-VN i.e. 7
    EXPECT_EQ(c1.ConfigTrackerSize(), 7);
    EXPECT_EQ(c2.ConfigTrackerSize(), 7);
    EXPECT_EQ(c3.ConfigTrackerSize(), 7);
    EXPECT_EQ(c4.ConfigTrackerSize(), 7);

    ProcessQueue();
    task_util::WaitForIdle();

    // Remove the vr-vm link for c1.
    IFMapMsgUnlink("virtual-router", "virtual-machine", "192.168.1.1", "vm_c1");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_FALSE(state->interest().test(c1.index()));
    TASK_UTIL_EXPECT_FALSE(c1.ConfigTrackerHasState(state));

    // Remove the vr-vm link for c2.
    IFMapMsgUnlink("virtual-router", "virtual-machine", "192.168.1.2", "vm_c2");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_FALSE(state->interest().test(c2.index()));
    TASK_UTIL_EXPECT_FALSE(c2.ConfigTrackerHasState(state));

    // Remove the vr-vm link for c3.
    IFMapMsgUnlink("virtual-router", "virtual-machine", "192.168.1.3", "vm_c3");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_FALSE(state->interest().test(c3.index()));
    TASK_UTIL_EXPECT_FALSE(c3.ConfigTrackerHasState(state));

    // Remove the vr-vm link for c4.
    IFMapMsgUnlink("virtual-router", "virtual-machine", "192.168.1.4", "vm_c4");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_FALSE(state->interest().test(c4.index()));
    TASK_UTIL_EXPECT_FALSE(c4.ConfigTrackerHasState(state));

    // The config-tracker must be empty for all clients.
    EXPECT_TRUE(state->interest().empty());
    EXPECT_TRUE(c1.ConfigTrackerEmpty());
    EXPECT_TRUE(c2.ConfigTrackerEmpty());
    EXPECT_TRUE(c3.ConfigTrackerEmpty());
    EXPECT_TRUE(c4.ConfigTrackerEmpty());

    ProcessQueue();
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    LoggingInit();
    bool success = RUN_ALL_TESTS();
    TaskScheduler::GetInstance()->Terminate();
    return success;
}
