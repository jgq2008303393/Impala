// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <vector>

#include <gtest/gtest.h>

#include "common/compiler-util.h"
#include "exec/hash-table.inline.h"
#include "exprs/expr.h"
#include "runtime/mem-pool.h"
#include "runtime/string-value.h"
#include "util/cpu-info.h"
#include "util/perf-counters.h"
#include "util/runtime-profile.h"

using namespace std;

namespace impala {

class HashTableTest : public testing::Test {
 protected:
  ObjectPool pool_;
  MemPool mem_pool_;
  vector<Expr*> build_expr_;
  vector<Expr*> probe_expr_;

  virtual void SetUp() {
    RowDescriptor desc;
    Status status;

    // Not very easy to test complex tuple layouts so this test will use the 
    // simplest.  The purpose of these tests is to exercise the hash map
    // internals so a simple build/probe expr is fine.
    build_expr_.push_back(pool_.Add(new SlotRef(TYPE_INT, 0)));
    status = Expr::Prepare(build_expr_, NULL, desc);
    EXPECT_TRUE(status.ok());
    
    probe_expr_.push_back(pool_.Add(new SlotRef(TYPE_INT, 0)));
    status = Expr::Prepare(probe_expr_, NULL, desc);
    EXPECT_TRUE(status.ok());
  }

  TupleRow* CreateTupleRow(int32_t val) {
    uint8_t* tuple_row_mem = mem_pool_.Allocate(sizeof(int32_t*));
    uint8_t* tuple_mem = mem_pool_.Allocate(sizeof(int32_t));
    *reinterpret_cast<int32_t*>(tuple_mem) = val;
    TupleRow* row = reinterpret_cast<TupleRow*>(tuple_row_mem);
    row->SetTuple(0, reinterpret_cast<Tuple*>(tuple_mem));
    return row;
  }

  // Wrapper to call private methods on HashTable
  // TODO: understand google testing, there must be a more natural way to do this
  void ResizeTable(HashTable* table, int64_t new_size) {
    table->ResizeBuckets(new_size);
  }

  // Do a full table scan on table.  All values should be between [min,max).  If
  // all_unique, then each key(int value) should only appear once.  Results are
  // stored in results, indexed by the key.  Results must have been preallocated to
  // be at least max size.
  void FullScan(HashTable* table, int min, int max, bool all_unique, 
      TupleRow** results, TupleRow** expected) {
    HashTable::Iterator iter = table->Begin();
    while (iter != table->End()) {
      TupleRow* row = iter.GetRow();
      int32_t val = *reinterpret_cast<int32_t*>(build_expr_[0]->GetValue(row));
      EXPECT_GE(val, min);
      EXPECT_LT(val, max);
      if (all_unique) EXPECT_TRUE(results[val] == NULL);
      EXPECT_EQ(row->GetTuple(0), expected[val]->GetTuple(0));
      results[val] = row;
      iter.Next<false>();
    }
  }

  // Validate that probe_row evaluates overs probe_exprs is equal to build_row 
  // evaluated over build_exprs
  void ValidateMatch(TupleRow* probe_row, TupleRow* build_row) {
    EXPECT_TRUE(probe_row != build_row);
    int32_t build_val = *reinterpret_cast<int32_t*>(build_expr_[0]->GetValue(probe_row));
    int32_t probe_val = *reinterpret_cast<int32_t*>(probe_expr_[0]->GetValue(build_row));
    EXPECT_EQ(build_val, probe_val);
  }

  struct ProbeTestData {
    TupleRow* probe_row;
    vector<TupleRow*> expected_build_rows;
  };

  void ProbeTest(HashTable* table, ProbeTestData* data, int num_data, bool scan) {
    for (int i = 0; i < num_data; ++i) {
      TupleRow* row = data[i].probe_row;
      
      HashTable::Iterator iter;
      iter = table->Find(row);

      if (data[i].expected_build_rows.size() == 0) {
        EXPECT_TRUE(iter == table->End());
      } else {
        if (scan) {
          map<TupleRow*, bool> matched;
          while (iter != table->End()) {
            EXPECT_TRUE(matched.find(iter.GetRow()) == matched.end());
            matched[iter.GetRow()] = true;
            iter.Next<true>();
          }
          EXPECT_EQ(matched.size(), data[i].expected_build_rows.size());
          for (int j = 0; i < data[j].expected_build_rows.size(); ++j) {
            EXPECT_TRUE(matched[data[i].expected_build_rows[j]]);
          }
        } else {
          EXPECT_EQ(data[i].expected_build_rows.size(), 1);
          EXPECT_TRUE(
              data[i].expected_build_rows[0]->GetTuple(0) == iter.GetRow()->GetTuple(0));
          ValidateMatch(row, iter.GetRow());
        }
      }
    }
  }
};

TEST_F(HashTableTest, SetupTest) {
  TupleRow* build_row1 = CreateTupleRow(1);
  TupleRow* build_row2 = CreateTupleRow(2);
  TupleRow* probe_row3 = CreateTupleRow(3);
  TupleRow* probe_row4 = CreateTupleRow(4);

  int32_t* val_row1 = reinterpret_cast<int32_t*>(build_expr_[0]->GetValue(build_row1));
  int32_t* val_row2 = reinterpret_cast<int32_t*>(build_expr_[0]->GetValue(build_row2));
  int32_t* val_row3 = reinterpret_cast<int32_t*>(probe_expr_[0]->GetValue(probe_row3));
  int32_t* val_row4 = reinterpret_cast<int32_t*>(probe_expr_[0]->GetValue(probe_row4));

  EXPECT_EQ(*val_row1, 1);
  EXPECT_EQ(*val_row2, 2);
  EXPECT_EQ(*val_row3, 3);
  EXPECT_EQ(*val_row4, 4);
}

// This tests inserts the build rows [0->5) to hash table.  It validates that they
// are all there using a full table scan.  It also validates that Find() is correct
// testing for probe rows that are both there and not.
// The hash table is rehashed a few times and the scans/finds are tested again.
TEST_F(HashTableTest, BasicTest) {
  TupleRow* build_rows[5];
  TupleRow* scan_rows[5] = {0};
  for (int i = 0; i < 5; ++i) {
    build_rows[i] = CreateTupleRow(i);
  }

  ProbeTestData probe_rows[10];
  for (int i = 0; i < 10; ++i) {
    probe_rows[i].probe_row = CreateTupleRow(i);
    if (i < 5) {
      probe_rows[i].expected_build_rows.push_back(build_rows[i]);
    } 
  }
  
  // Create the hash table and insert the build rows
  HashTable hash_table(build_expr_, probe_expr_, 1, false);
  for (int i = 0; i < 5; ++i) {
    hash_table.Insert(build_rows[i]);
  }
  EXPECT_EQ(hash_table.size(), 5);

  // Do a full table scan and validate returned pointers
  FullScan(&hash_table, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, probe_rows, 10, false);

  // Resize and scan again
  ResizeTable(&hash_table, 64);
  EXPECT_EQ(hash_table.num_buckets(), 64);
  EXPECT_EQ(hash_table.size(), 5);
  memset(scan_rows, 0, sizeof(scan_rows));
  FullScan(&hash_table, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, probe_rows, 10, false);

  // Resize to two and cause some collisions
  ResizeTable(&hash_table, 2);
  EXPECT_EQ(hash_table.num_buckets(), 2);
  EXPECT_EQ(hash_table.size(), 5);
  memset(scan_rows, 0, sizeof(scan_rows));
  FullScan(&hash_table, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, probe_rows, 10, false);

  // Resize to one and turn it into a linked list
  ResizeTable(&hash_table, 1);
  EXPECT_EQ(hash_table.num_buckets(), 1);
  EXPECT_EQ(hash_table.size(), 5);
  memset(scan_rows, 0, sizeof(scan_rows));
  FullScan(&hash_table, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, probe_rows, 10, false);
}

// This tests makes sure we can scan ranges of buckets
TEST_F(HashTableTest, ScanTest) {
  HashTable hash_table(build_expr_, probe_expr_, 1, false);
  // Add 1 row with val 1, 2 with val 2, etc 
  vector<TupleRow*> build_rows;
  ProbeTestData probe_rows[15];
  probe_rows[0].probe_row = CreateTupleRow(0);
  for (int val = 1; val <= 10; ++val) {
    probe_rows[val].probe_row = CreateTupleRow(val);
    for (int i = 0; i < val; ++i) {
      TupleRow* row = CreateTupleRow(val);
      hash_table.Insert(row);
      build_rows.push_back(row);
      probe_rows[val].expected_build_rows.push_back(row);
    }
  }

  // Add some more probe rows that aren't there
  for (int val = 11; val < 15; ++val) {
    probe_rows[val].probe_row = CreateTupleRow(val);
  }

  // Test that all the builds were found
  ProbeTest(&hash_table, probe_rows, 15, true);

  // Resize and try again
  ResizeTable(&hash_table, 128);
  EXPECT_EQ(hash_table.num_buckets(), 128);
  ProbeTest(&hash_table, probe_rows, 15, true);

  ResizeTable(&hash_table, 20);
  EXPECT_EQ(hash_table.num_buckets(), 20);
  ProbeTest(&hash_table, probe_rows, 15, true);

  ResizeTable(&hash_table, 1);
  EXPECT_EQ(hash_table.num_buckets(), 1);
  ProbeTest(&hash_table, probe_rows, 15, true);
}

// This test continues adding to the hash table to trigger the resize code paths
TEST_F(HashTableTest, GrowTableTest) {
  int build_row_val = 0;
  int num_to_add = 5;
  int expected_size = 0;
  HashTable hash_table(build_expr_, probe_expr_, 1, false, num_to_add);

  // This inserts about 5M entries
  for (int i = 0; i < 20; ++i) {
    for (int j = 0; j < num_to_add; ++build_row_val, ++j) {
      hash_table.Insert(CreateTupleRow(build_row_val));
    }
    expected_size += num_to_add;
    num_to_add *= 2;
    EXPECT_EQ(hash_table.size(), expected_size);
  }

  // Validate that we can find the entries
  for (int i = 0; i < expected_size * 5; i += 100000) {
    TupleRow* probe_row = CreateTupleRow(i);
    HashTable::Iterator iter = hash_table.Find(probe_row);
    if (i < expected_size) {
      EXPECT_TRUE(iter != hash_table.End());
      ValidateMatch(probe_row, iter.GetRow());
    } else {
      EXPECT_TRUE(iter == hash_table.End());
    }
  }
}

}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  impala::CpuInfo::Init();
  return RUN_ALL_TESTS();
}
