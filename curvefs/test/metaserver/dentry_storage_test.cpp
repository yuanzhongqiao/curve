/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * @Project: curve
 * @Date: 2021-06-10 10:04:21
 * @Author: chenwei
 */

#include "curvefs/src/metaserver/dentry_storage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "curvefs/src/metaserver/storage/storage.h"
#include "curvefs/src/metaserver/storage/rocksdb_storage.h"
#include "curvefs/test/metaserver/storage/utils.h"
#include "src/fs/ext4_filesystem_impl.h"

namespace curvefs {
namespace metaserver {

using ::curvefs::metaserver::storage::KVStorage;
using ::curvefs::metaserver::storage::NameGenerator;
using ::curvefs::metaserver::storage::RandomStoragePath;
using ::curvefs::metaserver::storage::RocksDBStorage;
using ::curvefs::metaserver::storage::StorageOptions;

namespace {
auto localfs = curve::fs::Ext4FileSystemImpl::getInstance();
}

class DentryStorageTest : public ::testing::Test {
 protected:
    void SetUp() override {
        nameGenerator_ = std::make_shared<NameGenerator>(1);
        dataDir_ = RandomStoragePath();

        StorageOptions options;
        options.dataDir = dataDir_;
        options.localFileSystem = localfs.get();
        kvStorage_ = std::make_shared<RocksDBStorage>(options);
        ASSERT_TRUE(kvStorage_->Open());
        logIndex_ = 0;
    }

    void TearDown() override {
        ASSERT_TRUE(kvStorage_->Close());
        auto output = execShell("rm -rf " + dataDir_);
        ASSERT_EQ(output.size(), 0);
    }

    std::string execShell(const std::string& cmd) {
        std::array<char, 128> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                      pclose);
        if (!pipe) {
            throw std::runtime_error("popen() failed!");
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }

    Dentry GenDentry(uint32_t fsId, uint64_t parentId, const std::string& name,
                     uint64_t txId, uint64_t inodeId, bool deleteMarkFlag,
                     FsFileType type = FsFileType::TYPE_FILE) {
        Dentry dentry;
        dentry.set_fsid(fsId);
        dentry.set_parentinodeid(parentId);
        dentry.set_name(name);
        dentry.set_txid(txId);
        dentry.set_inodeid(inodeId);
        dentry.set_flag(deleteMarkFlag ? DentryFlag::DELETE_MARK_FLAG : 0);
        dentry.set_type(type);
        return dentry;
    }

    void InsertDentrys(DentryStorage* storage,
                       const std::vector<Dentry>&& dentrys) {
        // NOTE: store real transaction is unnecessary
        metaserver::TransactionRequest request;
        request.set_type(metaserver::TransactionRequest::None);
        request.set_rawpayload("");

        auto rc = storage->PrepareTx(dentrys, request, logIndex_++);
        ASSERT_EQ(rc, MetaStatusCode::OK);
        ASSERT_EQ(storage->Size(), dentrys.size());
    }

    void ASSERT_DENTRYS_EQ(const std::vector<Dentry>& lhs,
                           const std::vector<Dentry>&& rhs) {
        ASSERT_EQ(lhs, rhs);
    }

 protected:
    std::string dataDir_;
    std::shared_ptr<NameGenerator> nameGenerator_;
    std::shared_ptr<KVStorage> kvStorage_;
    int64_t logIndex_;
};

TEST_F(DentryStorageTest, Insert) {
    DentryStorage storage(kvStorage_, nameGenerator_, 0);
    ASSERT_TRUE(storage.Init());
    Dentry dentry;
    dentry.set_fsid(1);
    dentry.set_parentinodeid(1);
    dentry.set_name("A");
    dentry.set_inodeid(2);
    dentry.set_txid(0);

    Dentry dentry2;
    dentry2.set_fsid(1);
    dentry2.set_parentinodeid(1);
    dentry2.set_name("A");
    dentry2.set_inodeid(3);
    dentry2.set_txid(0);

    // CASE 1: insert success
    ASSERT_EQ(storage.Insert(dentry, logIndex_++), MetaStatusCode::OK);

    // CASE 2: insert with dentry exist
    ASSERT_EQ(storage.Insert(dentry2, logIndex_++),
              MetaStatusCode::DENTRY_EXIST);
    ASSERT_EQ(storage.Size(), 1);

    // CASE 3: insert dentry failed with higher txid
    dentry.set_txid(1);
    ASSERT_EQ(storage.Insert(dentry, logIndex_++),
              MetaStatusCode::IDEMPOTENCE_OK);
    ASSERT_EQ(storage.Size(), 1);

    // CASE 4: direct insert success by handle tx
    // NOTE: store real transaction is unnecessary
    metaserver::TransactionRequest request;
    request.set_type(metaserver::TransactionRequest::None);
    request.set_rawpayload("");
    auto rc = storage.PrepareTx({dentry}, request, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 2);

    // CASE 5: insert idempotence
    ASSERT_EQ(storage.Insert(dentry, logIndex_++),
              MetaStatusCode::IDEMPOTENCE_OK);
    ASSERT_EQ(storage.Size(), 1);
}

TEST_F(DentryStorageTest, Delete) {
    DentryStorage storage(kvStorage_, nameGenerator_, 0);
    ASSERT_TRUE(storage.Init());

    // NOTE: store real transaction is unnecessary
    metaserver::TransactionRequest request;
    request.set_type(metaserver::TransactionRequest::None);
    request.set_rawpayload("");
    Dentry dentry;
    dentry.set_fsid(1);
    dentry.set_parentinodeid(1);
    dentry.set_name("A");
    dentry.set_inodeid(2);
    dentry.set_txid(0);

    // CASE 1: dentry not found
    ASSERT_EQ(storage.Delete(dentry, logIndex_++), MetaStatusCode::NOT_FOUND);
    ASSERT_EQ(storage.Size(), 0);

    // CASE 2: delete success
    ASSERT_EQ(storage.Insert(dentry, logIndex_++), MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 1);

    ASSERT_EQ(storage.Delete(dentry, logIndex_++), MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 0);

    // CASE 3: delete multi-dentrys with different txid
    ASSERT_EQ(storage.Insert(dentry, logIndex_++), MetaStatusCode::OK);
    dentry.set_txid(1);
    // NOTE: store real transaction is unnecessary
    auto rc = storage.PrepareTx({dentry}, request, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 2);

    dentry.set_txid(2);
    ASSERT_EQ(storage.Delete(dentry, logIndex_++), MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 0);

    // CASE 4: delete by higher txid
    dentry.set_txid(2);
    ASSERT_EQ(storage.Insert(dentry, logIndex_++), MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 1);

    dentry.set_txid(1);
    ASSERT_EQ(storage.Delete(dentry, logIndex_++), MetaStatusCode::NOT_FOUND);
    ASSERT_EQ(storage.Size(), 1);

    dentry.set_txid(2);
    ASSERT_EQ(storage.Delete(dentry, logIndex_++), MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 0);

    // CASE 5: dentry deleted with DELETE_MARK_FLAG flag
    dentry.set_flag(DentryFlag::DELETE_MARK_FLAG);
    ASSERT_EQ(storage.Insert(dentry, logIndex_++), MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 1);

    ASSERT_EQ(storage.Delete(dentry, logIndex_++), MetaStatusCode::NOT_FOUND);
    ASSERT_EQ(storage.Size(), 0);

    // CASE 6: delete by last dentry with DELETE_MARK_FLAG flag
    dentry.set_txid(0);
    ASSERT_EQ(storage.Insert(dentry, logIndex_++), MetaStatusCode::OK);
    dentry.set_txid(1);
    dentry.set_flag(DentryFlag::DELETE_MARK_FLAG);
    // NOTE: store real transaction is unnecessary
    rc = storage.PrepareTx({dentry}, request, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 2);

    ASSERT_EQ(storage.Delete(dentry, logIndex_++), MetaStatusCode::NOT_FOUND);
    ASSERT_EQ(storage.Size(), 0);
}

TEST_F(DentryStorageTest, Get) {
    DentryStorage storage(kvStorage_, nameGenerator_, 0);
    ASSERT_TRUE(storage.Init());
    Dentry dentry;

    // CASE 1: dentry not found
    dentry = GenDentry(1, 0, "A", 0, 0, false);
    ASSERT_EQ(storage.Get(&dentry), MetaStatusCode::NOT_FOUND);

    // CASE 2: get success
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "B", 0, 2, false),
                  });

    dentry = GenDentry(1, 0, "A", 0, 0, false);
    ASSERT_EQ(storage.Get(&dentry), MetaStatusCode::OK);
    ASSERT_EQ(dentry.inodeid(), 1);
    ASSERT_EQ(storage.Size(), 2);

    dentry = GenDentry(1, 0, "B", 0, 0, false);
    ASSERT_EQ(storage.Get(&dentry), MetaStatusCode::OK);
    ASSERT_EQ(dentry.inodeid(), 2);
    ASSERT_EQ(storage.Size(), 2);

    // CASE 3: get multi-dentrys with different txid
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "A", 1, 2, false),
                  });

    dentry = GenDentry(1, 0, "A", 1, 0, false);
    ASSERT_EQ(storage.Get(&dentry), MetaStatusCode::OK);
    ASSERT_EQ(dentry.inodeid(), 2);
    ASSERT_EQ(storage.Size(), 2);

    // CASE 4: get dentry with DELETE_MARK_FLAG flag
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "A", 1, 1, true),
                  });

    dentry = GenDentry(1, 0, "A", 1, 0, false);
    ASSERT_EQ(storage.Get(&dentry), MetaStatusCode::NOT_FOUND);
    ASSERT_EQ(dentry.inodeid(), 0);
    ASSERT_EQ(storage.Size(), 2);
}

TEST_F(DentryStorageTest, List) {
    DentryStorage storage(kvStorage_, nameGenerator_, 0);
    ASSERT_TRUE(storage.Init());
    std::vector<Dentry> dentrys;
    Dentry dentry;

    // CASE 1: basic list
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A1", 0, 1, false),
                      GenDentry(1, 0, "A2", 0, 2, false),
                      GenDentry(1, 0, "A3", 0, 3, false),
                      GenDentry(1, 0, "A4", 0, 4, false),
                      GenDentry(1, 0, "A5", 0, 5, false),
                  });

    dentry = GenDentry(1, 0, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 5);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 0, "A1", 0, 1, false),
                                   GenDentry(1, 0, "A2", 0, 2, false),
                                   GenDentry(1, 0, "A3", 0, 3, false),
                                   GenDentry(1, 0, "A4", 0, 4, false),
                                   GenDentry(1, 0, "A5", 0, 5, false),
                               });

    // CASE 2: list by specify name
    dentrys.clear();
    dentry = GenDentry(1, 0, "A3", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 2);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 0, "A4", 0, 4, false),
                                   GenDentry(1, 0, "A5", 0, 5, false),
                               });

    // CASE 3: list by lower txid
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A1", 1, 1, false),
                      GenDentry(1, 0, "A2", 2, 2, false),
                      GenDentry(1, 0, "A3", 3, 3, false),
                  });

    dentrys.clear();
    dentry = GenDentry(1, 0, "", 2, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 2);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 0, "A1", 1, 1, false),
                                   GenDentry(1, 0, "A2", 2, 2, false),
                               });

    // CASE 4: list by higher txid
    dentrys.clear();
    dentry = GenDentry(1, 0, "", 4, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 3);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 0, "A1", 1, 1, false),
                                   GenDentry(1, 0, "A2", 2, 2, false),
                                   GenDentry(1, 0, "A3", 3, 3, false),
                               });

    // CASE 5: list dentrys which has DELETE_MARK_FLAG flag
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A1", 1, 1, false),
                      GenDentry(1, 0, "A2", 2, 2, true),
                      GenDentry(1, 0, "A3", 3, 3, false),
                  });

    dentrys.clear();
    dentry = GenDentry(1, 0, "", 3, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 2);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 0, "A1", 1, 1, false),
                                   GenDentry(1, 0, "A3", 3, 3, false),
                               });

    // CASE 6: list same dentrys with different txid
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "A", 1, 1, false),
                      GenDentry(1, 0, "A", 2, 1, false),
                  });

    dentrys.clear();
    dentry = GenDentry(1, 0, "", 2, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 1);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 0, "A", 2, 1, false),
                               });

    // CASE 7: list by dentry tree
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "B", 0, 2, false),
                      GenDentry(1, 2, "C", 0, 3, false),
                      GenDentry(1, 2, "D", 0, 4, false),
                      GenDentry(1, 2, "E", 0, 5, false),
                      GenDentry(1, 4, "F", 0, 6, true),
                      GenDentry(1, 4, "G", 0, 7, false),
                  });

    dentrys.clear();
    dentry = GenDentry(1, 2, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 3);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 2, "C", 0, 3, false),
                                   GenDentry(1, 2, "D", 0, 4, false),
                                   GenDentry(1, 2, "E", 0, 5, false),
                               });

    dentrys.clear();
    dentry = GenDentry(1, 4, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 1);
    ASSERT_DENTRYS_EQ(dentrys, std::vector<Dentry>{
                                   GenDentry(1, 4, "G", 0, 7, false),
                               });

    // CASE 8: list empty directory
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "B", 0, 2, false),
                      GenDentry(1, 2, "D", 0, 4, true),
                      GenDentry(1, 2, "E", 0, 5, true),
                  });

    dentrys.clear();
    dentry = GenDentry(1, 2, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 0);

    dentrys.clear();
    dentry = GenDentry(1, 3, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 0);

    dentrys.clear();
    dentry = GenDentry(2, 0, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 0);

    // CASE 9: list directory only
    storage.Clear();
    InsertDentrys(
        &storage,
        std::vector<Dentry>{
            // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
            GenDentry(1, 0, "A", 0, 1, false, FsFileType::TYPE_DIRECTORY),
            GenDentry(1, 0, "B", 0, 2, true, FsFileType::TYPE_DIRECTORY),
            GenDentry(1, 0, "D", 0, 3, false),
            GenDentry(1, 0, "E", 0, 4, false),
        });

    dentrys.clear();
    dentry = GenDentry(1, 0, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 0, true), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 1);

    // CASE 10: list directory only with limit
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "B", 0, 2, false),
                      GenDentry(1, 0, "D", 0, 3, false),
                  });

    dentrys.clear();
    dentry = GenDentry(1, 0, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 1, true), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 1);

    storage.Clear();
    InsertDentrys(
        &storage,
        std::vector<Dentry>{
            // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
            GenDentry(1, 0, "A", 0, 1, false, FsFileType::TYPE_DIRECTORY),
            GenDentry(1, 0, "B", 0, 2, false),
            GenDentry(1, 0, "D", 0, 3, false),
        });

    dentrys.clear();
    dentry = GenDentry(1, 0, "", 0, 0, false);
    ASSERT_EQ(storage.List(dentry, &dentrys, 3, true), MetaStatusCode::OK);
    ASSERT_EQ(dentrys.size(), 2);
}

TEST_F(DentryStorageTest, HandleTx) {
    DentryStorage storage(kvStorage_, nameGenerator_, 0);
    ASSERT_TRUE(storage.Init());
    std::vector<Dentry> dentrys;
    Dentry dentry;
    // NOTE: store real transaction is unnecessary
    metaserver::TransactionRequest request;
    request.set_type(metaserver::TransactionRequest::None);
    request.set_rawpayload("");
    // CASE 1: prepare success
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                  });

    dentry = GenDentry(1, 0, "A", 1, 2, false);
    // NOTE: store real transaction is unnecessary
    auto rc = storage.PrepareTx({dentry}, request, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 2);

    // CASE 2: prepare with dentry exist
    dentry = GenDentry(1, 0, "A", 1, 2, false);
    /// NOTE: store real transaction is unnecessary
    rc = storage.PrepareTx({dentry}, request, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 2);

    // CASE 3: commit success
    rc = storage.CommitTx({dentry}, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 1);

    dentry = GenDentry(1, 0, "A", 1, 0, false);
    ASSERT_EQ(storage.Get(&dentry), MetaStatusCode::OK);
    ASSERT_EQ(dentry.inodeid(), 2);

    // CASE 3: commit dentry with DELETE_MARK_FLAG flag
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "A", 1, 1, true),
                  });

    dentry = GenDentry(1, 0, "A", 1, 0, false);
    rc = storage.CommitTx({dentry}, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 0);

    // CASE 4: Rollback success
    storage.Clear();
    InsertDentrys(&storage,
                  std::vector<Dentry>{
                      // { fsId, parentId, name, txId, inodeId, deleteMarkFlag }
                      GenDentry(1, 0, "A", 0, 1, false),
                      GenDentry(1, 0, "A", 1, 2, false),
                  });
    ASSERT_EQ(storage.Size(), 2);

    dentry = GenDentry(1, 0, "A", 1, 2, false);
    rc = storage.RollbackTx({dentry}, logIndex_++);
    ASSERT_EQ(rc, MetaStatusCode::OK);
    ASSERT_EQ(storage.Size(), 1);

    dentry = GenDentry(1, 0, "A", 1, 0, false);
    ASSERT_EQ(storage.Get(&dentry), MetaStatusCode::OK);
    ASSERT_EQ(dentry.inodeid(), 1);
}

}  // namespace metaserver
}  // namespace curvefs
