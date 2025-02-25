//===- llvm/unittest/CAS/UnifiedOnDiskCacheTest.cpp -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/UnifiedOnDiskCache.h"
#include "OnDiskCommonUtils.h"
#include "llvm/Testing/Support/Error.h"
#include "llvm/Testing/Support/SupportHelpers.h"
#include "gtest/gtest.h"

#if LLVM_ENABLE_ONDISK_CAS

using namespace llvm;
using namespace llvm::cas;
using namespace llvm::cas::ondisk;
using namespace llvm::unittest::cas;

/// Visits all the files of a directory recursively and returns the sum of their
/// sizes.
static Expected<size_t> countFileSizes(StringRef Path) {
  size_t TotalSize = 0;
  std::error_code EC;
  for (sys::fs::directory_iterator DirI(Path, EC), DirE; !EC && DirI != DirE;
       DirI.increment(EC)) {
    if (DirI->type() == sys::fs::file_type::directory_file) {
      Expected<size_t> Subsize = countFileSizes(DirI->path());
      if (!Subsize)
        return Subsize.takeError();
      TotalSize += *Subsize;
      continue;
    }
    ErrorOr<sys::fs::basic_file_status> Stat = DirI->status();
    if (!Stat)
      return createFileError(DirI->path(), Stat.getError());
    TotalSize += Stat->getSize();
  }
  if (EC)
    return createFileError(Path, EC);
  return TotalSize;
}

TEST(UnifiedOnDiskCacheTest, Basic) {
  unittest::TempDir Temp("ondisk-unified", /*Unique=*/true);
  std::unique_ptr<UnifiedOnDiskCache> UniDB;

  auto reopenDB = [&]() {
    UniDB.reset();
    const uint64_t SizeLimit = 1024ull * 64;
    ASSERT_THAT_ERROR(UnifiedOnDiskCache::open(Temp.path(), SizeLimit, "blake3",
                                               sizeof(HashType))
                          .moveInto(UniDB),
                      Succeeded());
  };

  reopenDB();

  HashType RootHash;
  HashType OtherHash;
  HashType Key1Hash;
  HashType Key2Hash;
  {
    OnDiskGraphDB &DB = UniDB->getGraphDB();
    std::optional<ObjectID> ID1;
    ASSERT_THAT_ERROR(store(DB, "1", {}).moveInto(ID1), Succeeded());
    std::optional<ObjectID> ID2;
    ASSERT_THAT_ERROR(store(DB, "2", {}).moveInto(ID2), Succeeded());
    std::optional<ObjectID> IDRoot;
    ASSERT_THAT_ERROR(store(DB, "root", {*ID1, *ID2}).moveInto(IDRoot),
                      Succeeded());
    ArrayRef<uint8_t> Digest = DB.getDigest(*IDRoot);
    ASSERT_EQ(Digest.size(), RootHash.size());
    llvm::copy(Digest, RootHash.data());

    std::optional<ObjectID> IDOther;
    ASSERT_THAT_ERROR(store(DB, "other", {}).moveInto(IDOther), Succeeded());
    Digest = DB.getDigest(*IDOther);
    ASSERT_EQ(Digest.size(), OtherHash.size());
    llvm::copy(Digest, OtherHash.data());

    Key1Hash = digest("key1");
    std::optional<ObjectID> Val;
    ASSERT_THAT_ERROR(UniDB->KVPut(Key1Hash, *IDRoot).moveInto(Val),
                      Succeeded());
    EXPECT_EQ(IDRoot, Val);

    Key2Hash = digest("key2");
    ASSERT_THAT_ERROR(
        UniDB->KVPut(DB.getReference(Key2Hash), *ID1).moveInto(Val),
        Succeeded());
  }

  auto checkTree = [&](const HashType &Digest, StringRef ExpectedTree) {
    OnDiskGraphDB &DB = UniDB->getGraphDB();
    ObjectID ID = DB.getReference(Digest);
    std::string PrintedTree;
    raw_string_ostream OS(PrintedTree);
    ASSERT_THAT_ERROR(printTree(DB, ID, OS), Succeeded());
    EXPECT_EQ(PrintedTree, ExpectedTree);
  };
  auto checkRootTree = [&]() {
    return checkTree(RootHash, "root\n  1\n  2\n");
  };

  auto checkKey = [&](const HashType &Key, StringRef ExpectedData) {
    OnDiskGraphDB &DB = UniDB->getGraphDB();
    std::optional<ObjectID> Val;
    ASSERT_THAT_ERROR(UniDB->KVGet(Key).moveInto(Val), Succeeded());
    ASSERT_TRUE(Val.has_value());
    std::optional<ondisk::ObjectHandle> Obj;
    ASSERT_THAT_ERROR(DB.load(*Val).moveInto(Obj), Succeeded());
    EXPECT_EQ(toStringRef(DB.getObjectData(*Obj)), ExpectedData);
  };

  checkRootTree();
  checkTree(OtherHash, "other\n");
  checkKey(Key1Hash, "root");
  checkKey(Key2Hash, "1");

  auto storeBigObject = [&](unsigned Index) {
    SmallString<1000> Buf;
    Buf.append(970, 'a');
    raw_svector_ostream(Buf) << Index;
    std::optional<ObjectID> ID;
    ASSERT_THAT_ERROR(store(UniDB->getGraphDB(), Buf, {}).moveInto(ID),
                      Succeeded());
  };

  unsigned Index = 0;
  while (!UniDB->hasExceededSizeLimit()) {
    storeBigObject(Index++);
  }

  reopenDB();

  EXPECT_FALSE(UniDB->hasExceededSizeLimit());
  EXPECT_FALSE(UniDB->needsGarbaseCollection());

  checkRootTree();
  checkKey(Key1Hash, "root");

  while (!UniDB->hasExceededSizeLimit()) {
    storeBigObject(Index++);
  }
  ASSERT_THAT_ERROR(UniDB->close(), Succeeded());
  EXPECT_TRUE(UniDB->needsGarbaseCollection());

  reopenDB();
  EXPECT_TRUE(UniDB->needsGarbaseCollection());

  std::optional<size_t> DirSizeBefore;
  ASSERT_THAT_ERROR(countFileSizes(Temp.path()).moveInto(DirSizeBefore),
                    Succeeded());

  ASSERT_THAT_ERROR(UnifiedOnDiskCache::collectGarbage(Temp.path()),
                    Succeeded());

  std::optional<size_t> DirSizeAfter;
  ASSERT_THAT_ERROR(countFileSizes(Temp.path()).moveInto(DirSizeAfter),
                    Succeeded());
  EXPECT_LT(*DirSizeAfter, *DirSizeBefore);

  reopenDB();
  EXPECT_FALSE(UniDB->needsGarbaseCollection());

  checkRootTree();
  checkKey(Key1Hash, "root");

  // 'Other' tree and 'Key2' got garbage-collected.
  {
    OnDiskGraphDB &DB = UniDB->getGraphDB();
    EXPECT_FALSE(DB.containsObject(DB.getReference(OtherHash)));
    std::optional<ObjectID> Val;
    ASSERT_THAT_ERROR(UniDB->KVGet(Key2Hash).moveInto(Val), Succeeded());
    EXPECT_FALSE(Val.has_value());
  }
}

#endif // LLVM_ENABLE_ONDISK_CAS
