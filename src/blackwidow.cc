//  Copyright (c) 2017-present The blackwidow Authors.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "blackwidow/blackwidow.h"
#include "blackwidow/util.h"

#include "src/mutex_impl.h"
#include "src/redis_strings.h"
#include "src/redis_hashes.h"
#include "src/redis_sets.h"
#include "src/redis_lists.h"
#include "src/redis_zsets.h"
#include "src/redis_hyperloglog.h"
#include "src/lru_cache.h"

namespace blackwidow {

BlackWidow::BlackWidow() :
  strings_db_(nullptr),
  hashes_db_(nullptr),
  sets_db_(nullptr),
  zsets_db_(nullptr),
  lists_db_(nullptr),
  is_opened_(false),
  bg_tasks_cond_var_(&bg_tasks_mutex_),
  current_task_type_(kNone),
  bg_tasks_should_exit_(false),
  scan_keynum_exit_(false) {
  cursors_store_ = new LRUCache<std::string, std::string>();
  cursors_store_->SetCapacity(5000);

  Status s = StartBGThread();
  if (!s.ok()) {
    fprintf(stderr,
        "[FATAL] start bg thread failed, %s\n", s.ToString().c_str());
    exit(-1);
  }
}

BlackWidow::~BlackWidow() {
  bg_tasks_should_exit_ = true;
  bg_tasks_cond_var_.Signal();

  if (is_opened_) {
    rocksdb::CancelAllBackgroundWork(strings_db_->GetDB(), true);
    rocksdb::CancelAllBackgroundWork(hashes_db_->GetDB(), true);
    rocksdb::CancelAllBackgroundWork(sets_db_->GetDB(), true);
    rocksdb::CancelAllBackgroundWork(lists_db_->GetDB(), true);
    rocksdb::CancelAllBackgroundWork(zsets_db_->GetDB(), true);
  }

  int ret = 0;
  if ((ret = pthread_join(bg_tasks_thread_id_, NULL)) != 0) {
    fprintf(stderr, "pthread_join failed with bgtask thread error %d\n", ret);
  }

  delete strings_db_;
  delete hashes_db_;
  delete sets_db_;
  delete lists_db_;
  delete zsets_db_;
  delete cursors_store_;
}

static std::string AppendSubDirectory(const std::string& db_path,
    const std::string& sub_db) {
  if (db_path.back() == '/') {
    return db_path + sub_db;
  } else {
    return db_path + "/" + sub_db;
  }
}

Status BlackWidow::Open(const BlackwidowOptions& bw_options,
                        const std::string& db_path) {
  mkpath(db_path.c_str(), 0755);

  strings_db_ = new RedisStrings(this, kStrings);
  Status s = strings_db_->Open(
      bw_options, AppendSubDirectory(db_path, "strings"));
  if (!s.ok()) {
    fprintf(stderr,
        "[FATAL] open kv db failed, %s\n", s.ToString().c_str());
    exit(-1);
  }

  hashes_db_ = new RedisHashes(this, kHashes);
  s = hashes_db_->Open(bw_options, AppendSubDirectory(db_path, "hashes"));
  if (!s.ok()) {
    fprintf(stderr,
        "[FATAL] open hashes db failed, %s\n", s.ToString().c_str());
    exit(-1);
  }

  sets_db_ = new RedisSets(this, kSets);
  s = sets_db_->Open(bw_options, AppendSubDirectory(db_path, "sets"));
  if (!s.ok()) {
    fprintf(stderr,
        "[FATAL] open set db failed, %s\n", s.ToString().c_str());
    exit(-1);
  }

  lists_db_ = new RedisLists(this, kLists);
  s = lists_db_->Open(bw_options, AppendSubDirectory(db_path, "lists"));
  if (!s.ok()) {
    fprintf(stderr,
        "[FATAL] open list db failed, %s\n", s.ToString().c_str());
    exit(-1);
  }

  zsets_db_ = new RedisZSets(this, kZSets);
  s = zsets_db_->Open(bw_options, AppendSubDirectory(db_path, "zsets"));
  if (!s.ok()) {
    fprintf(stderr,
        "[FATAL] open zset db failed, %s\n", s.ToString().c_str());
    exit(-1);
  }
  is_opened_.store(true);
  return Status::OK();
}

Status BlackWidow::GetStartKey(const DataType& dtype, int64_t cursor, std::string* start_key) {
  std::string index_key = DataTypeTag[dtype] + std::to_string(cursor);
  return cursors_store_->Lookup(index_key, start_key);
}

Status BlackWidow::StoreCursorStartKey(const DataType& dtype, int64_t cursor,
                                       const std::string& next_key) {
  std::string index_key = DataTypeTag[dtype] + std::to_string(cursor);
  return cursors_store_->Insert(index_key, next_key);
}

// Strings Commands
Status BlackWidow::Set(const Slice& key,
                       const Slice& value) {
  return strings_db_->Set(key, value);
}

Status BlackWidow::Setxx(const Slice& key,
                         const Slice& value,
                         int32_t* ret,
                         const int32_t ttl) {
  return strings_db_->Setxx(key, value, ret, ttl);
}

Status BlackWidow::Get(const Slice& key, std::string* value) {
  return strings_db_->Get(key, value);
}

Status BlackWidow::GetSet(const Slice& key, const Slice& value,
                          std::string* old_value) {
  return strings_db_->GetSet(key, value, old_value);
}

Status BlackWidow::SetBit(const Slice& key, int64_t offset,
                          int32_t value, int32_t* ret) {
  return strings_db_->SetBit(key, offset, value, ret);
}

Status BlackWidow::GetBit(const Slice& key, int64_t offset, int32_t* ret) {
  return strings_db_->GetBit(key, offset, ret);
}

Status BlackWidow::MSet(const std::vector<KeyValue>& kvs) {
  return strings_db_->MSet(kvs);
}

Status BlackWidow::MGet(const std::vector<std::string>& keys,
                        std::vector<ValueStatus>* vss) {
  return strings_db_->MGet(keys, vss);
}

Status BlackWidow::Setnx(const Slice& key, const Slice& value,
                         int32_t* ret, const int32_t ttl) {
  return strings_db_->Setnx(key, value, ret, ttl);
}

Status BlackWidow::MSetnx(const std::vector<KeyValue>& kvs,
                          int32_t* ret) {
  return strings_db_->MSetnx(kvs, ret);
}

Status BlackWidow::Setvx(const Slice& key, const Slice& value,
                         const Slice& new_value, int32_t* ret,
                         const int32_t ttl) {
  return strings_db_->Setvx(key, value, new_value, ret, ttl);
}

Status BlackWidow::Delvx(const Slice& key, const Slice& value, int32_t* ret) {
  return strings_db_->Delvx(key, value, ret);
}

Status BlackWidow::Setrange(const Slice& key, int64_t start_offset,
                            const Slice& value, int32_t* ret) {
  return strings_db_->Setrange(key, start_offset, value, ret);
}

Status BlackWidow::Getrange(const Slice& key, int64_t start_offset,
                            int64_t end_offset, std::string* ret) {
  return strings_db_->Getrange(key, start_offset, end_offset, ret);
}

Status BlackWidow::Append(const Slice& key, const Slice& value, int32_t* ret) {
  return strings_db_->Append(key, value, ret);
}

Status BlackWidow::BitCount(const Slice& key, int64_t start_offset,
                            int64_t end_offset, int32_t *ret, bool have_range) {
  return strings_db_->BitCount(key, start_offset, end_offset, ret, have_range);
}

Status BlackWidow::BitOp(BitOpType op, const std::string& dest_key,
                         const std::vector<std::string>& src_keys,
                         int64_t* ret) {
  return strings_db_->BitOp(op, dest_key, src_keys, ret);
}

Status BlackWidow::BitPos(const Slice& key, int32_t bit,
                          int64_t* ret) {
  return strings_db_->BitPos(key, bit, ret);
}

Status BlackWidow::BitPos(const Slice& key, int32_t bit,
                          int64_t start_offset, int64_t* ret) {
  return strings_db_->BitPos(key, bit, start_offset, ret);
}

Status BlackWidow::BitPos(const Slice& key, int32_t bit,
                          int64_t start_offset, int64_t end_offset,
                          int64_t* ret) {
  return strings_db_->BitPos(key, bit, start_offset, end_offset, ret);
}

Status BlackWidow::Decrby(const Slice& key, int64_t value, int64_t* ret) {
  return strings_db_->Decrby(key, value, ret);
}

Status BlackWidow::Incrby(const Slice& key, int64_t value, int64_t* ret) {
  return strings_db_->Incrby(key, value, ret);
}

Status BlackWidow::Incrbyfloat(const Slice& key, const Slice& value,
                               std::string* ret) {
  return strings_db_->Incrbyfloat(key, value, ret);
}

Status BlackWidow::Setex(const Slice& key, const Slice& value, int32_t ttl) {
  return strings_db_->Setex(key, value, ttl);
}

Status BlackWidow::Strlen(const Slice& key, int32_t* len) {
  return strings_db_->Strlen(key, len);
}

Status BlackWidow::PKSetexAt(const Slice& key,
                             const Slice& value,
                             int32_t timestamp) {
  return strings_db_->PKSetexAt(key, value, timestamp);
}

// Hashes Commands
Status BlackWidow::HSet(const Slice& key, const Slice& field,
    const Slice& value, int32_t* res) {
  return hashes_db_->HSet(key, field, value, res);
}

Status BlackWidow::HGet(const Slice& key, const Slice& field,
    std::string* value) {
  return hashes_db_->HGet(key, field, value);
}

Status BlackWidow::HMSet(const Slice& key,
                         const std::vector<FieldValue>& fvs) {
  return hashes_db_->HMSet(key, fvs);
}

Status BlackWidow::HMGet(const Slice& key,
                         const std::vector<std::string>& fields,
                         std::vector<ValueStatus>* vss) {
  return hashes_db_->HMGet(key, fields, vss);
}

Status BlackWidow::HGetall(const Slice& key,
                           std::vector<FieldValue>* fvs) {
  return hashes_db_->HGetall(key, fvs);
}

Status BlackWidow::HKeys(const Slice& key,
                         std::vector<std::string>* fields) {
  return hashes_db_->HKeys(key, fields);
}

Status BlackWidow::HVals(const Slice& key,
                         std::vector<std::string>* values) {
  return hashes_db_->HVals(key, values);
}

Status BlackWidow::HSetnx(const Slice& key, const Slice& field,
                          const Slice& value, int32_t* ret) {
  return hashes_db_->HSetnx(key, field, value, ret);
}

Status BlackWidow::HLen(const Slice& key, int32_t* ret) {
  return hashes_db_->HLen(key, ret);
}

Status BlackWidow::HStrlen(const Slice& key, const Slice& field, int32_t* len) {
  return hashes_db_->HStrlen(key, field, len);
}

Status BlackWidow::HExists(const Slice& key, const Slice& field) {
  return hashes_db_->HExists(key, field);
}

Status BlackWidow::HIncrby(const Slice& key, const Slice& field, int64_t value,
                           int64_t* ret) {
  return hashes_db_->HIncrby(key, field, value, ret);
}

Status BlackWidow::HIncrbyfloat(const Slice& key, const Slice& field,
                                const Slice& by, std::string* new_value) {
  return hashes_db_->HIncrbyfloat(key, field, by, new_value);
}

Status BlackWidow::HDel(const Slice& key,
                        const std::vector<std::string>& fields,
                        int32_t* ret) {
  return hashes_db_->HDel(key, fields, ret);
}

Status BlackWidow::HScan(const Slice& key, int64_t cursor,
                         const std::string& pattern, int64_t count,
                         std::vector<FieldValue>* field_values,
                         int64_t* next_cursor) {
  return hashes_db_->HScan(key, cursor,
      pattern, count, field_values, next_cursor);
}

Status BlackWidow::HScanx(const Slice& key, const std::string start_field,
                          const std::string& pattern, int64_t count,
                          std::vector<FieldValue>* field_values,
                          std::string* next_field) {
  return hashes_db_->HScanx(key, start_field,
      pattern, count, field_values, next_field);
}

Status BlackWidow::PKHScanRange(const Slice& key, const Slice& field_start,
                                const std::string& field_end,
                                const Slice& pattern, int32_t limit,
                                std::vector<FieldValue>* field_values,
                                std::string* next_field) {
  return hashes_db_->PKHScanRange(key, field_start,
      field_end, pattern, limit, field_values, next_field);
}

Status BlackWidow::PKHRScanRange(const Slice& key, const Slice& field_start,
                                 const std::string& field_end,
                                 const Slice& pattern, int32_t limit,
                                 std::vector<FieldValue>* field_values,
                                 std::string* next_field) {
  return hashes_db_->PKHRScanRange(key, field_start,
      field_end, pattern, limit, field_values, next_field);
}

// Sets Commands
Status BlackWidow::SAdd(const Slice& key,
                        const std::vector<std::string>& members,
                        int32_t* ret) {
  return sets_db_->SAdd(key, members, ret);
}

Status BlackWidow::SCard(const Slice& key,
                         int32_t* ret) {
  return sets_db_->SCard(key, ret);
}

Status BlackWidow::SDiff(const std::vector<std::string>& keys,
                         std::vector<std::string>* members) {
  return sets_db_->SDiff(keys, members);
}

Status BlackWidow::SDiffstore(const Slice& destination,
                              const std::vector<std::string>& keys,
                              int32_t* ret) {
  return sets_db_->SDiffstore(destination, keys, ret);
}

Status BlackWidow::SInter(const std::vector<std::string>& keys,
                          std::vector<std::string>* members) {
  return sets_db_->SInter(keys, members);
}

Status BlackWidow::SInterstore(const Slice& destination,
                               const std::vector<std::string>& keys,
                               int32_t* ret) {
  return sets_db_->SInterstore(destination, keys, ret);
}

Status BlackWidow::SIsmember(const Slice& key, const Slice& member,
                             int32_t* ret) {
  return sets_db_->SIsmember(key, member, ret);
}

Status BlackWidow::SMembers(const Slice& key,
                            std::vector<std::string>* members) {
  return sets_db_->SMembers(key, members);
}

Status BlackWidow::SMove(const Slice& source, const Slice& destination,
                         const Slice& member, int32_t* ret) {
  return sets_db_->SMove(source, destination, member, ret);
}

Status BlackWidow::SPop(const Slice& key, std::string* member) {
  bool need_compact = false;
  Status status = sets_db_->SPop(key, member, &need_compact);
  if (need_compact) {
    AddBGTask({kSets, kCompactKey, key.ToString()});
  }
  return status;
}

Status BlackWidow::SRandmember(const Slice& key, int32_t count,
                               std::vector<std::string>* members) {
  return sets_db_->SRandmember(key, count, members);
}

Status BlackWidow::SRem(const Slice& key,
                        const std::vector<std::string>& members,
                        int32_t* ret) {
  return sets_db_->SRem(key, members, ret);
}

Status BlackWidow::SUnion(const std::vector<std::string>& keys,
                          std::vector<std::string>* members) {
  return sets_db_->SUnion(keys, members);
}

Status BlackWidow::SUnionstore(const Slice& destination,
                               const std::vector<std::string>& keys,
                               int32_t* ret) {
  return sets_db_->SUnionstore(destination, keys, ret);
}

Status BlackWidow::SScan(const Slice& key, int64_t cursor,
                         const std::string& pattern, int64_t count,
                         std::vector<std::string>* members,
                         int64_t* next_cursor) {
  return sets_db_->SScan(key, cursor, pattern, count, members, next_cursor);
}

Status BlackWidow::LPush(const Slice& key,
                         const std::vector<std::string>& values,
                         uint64_t* ret) {
  return lists_db_->LPush(key, values, ret);
}

Status BlackWidow::RPush(const Slice& key,
                         const std::vector<std::string>& values,
                         uint64_t* ret) {
  return lists_db_->RPush(key, values, ret);
}

Status BlackWidow::LRange(const Slice& key, int64_t start, int64_t stop,
                          std::vector<std::string>* ret) {
  return lists_db_->LRange(key, start, stop, ret);
}

Status BlackWidow::LTrim(const Slice& key, int64_t start, int64_t stop) {
  return lists_db_->LTrim(key, start, stop);
}

Status BlackWidow::LLen(const Slice& key, uint64_t* len) {
  return lists_db_->LLen(key, len);
}

Status BlackWidow::LPop(const Slice& key, std::string* element) {
  return lists_db_->LPop(key, element);
}

Status BlackWidow::RPop(const Slice& key, std::string* element) {
  return lists_db_->RPop(key, element);
}

Status BlackWidow::LIndex(const Slice& key,
                          int64_t index,
                          std::string* element) {
  return lists_db_->LIndex(key, index, element);
}

Status BlackWidow::LInsert(const Slice& key,
                           const BeforeOrAfter& before_or_after,
                           const std::string& pivot,
                           const std::string& value,
                           int64_t* ret) {
  return lists_db_->LInsert(key, before_or_after, pivot, value, ret);
}

Status BlackWidow::LPushx(const Slice& key, const Slice& value, uint64_t* len) {
  return lists_db_->LPushx(key, value, len);
}

Status BlackWidow::RPushx(const Slice& key, const Slice& value, uint64_t* len) {
  return lists_db_->RPushx(key, value, len);
}

Status BlackWidow::LRem(const Slice& key, int64_t count,
                        const Slice& value, uint64_t* ret) {
  return lists_db_->LRem(key, count, value, ret);
}

Status BlackWidow::LSet(const Slice& key, int64_t index, const Slice& value) {
  return lists_db_->LSet(key, index, value);
}

Status BlackWidow::RPoplpush(const Slice& source,
                             const Slice& destination,
                             std::string* element) {
  return lists_db_->RPoplpush(source, destination, element);
}

Status BlackWidow::ZPopMax(const Slice& key,
			   const int64_t count,
			   std::vector<ScoreMember>* score_members){
  return zsets_db_->ZPopMax(key, count, score_members);
}

Status BlackWidow::ZPopMin(const Slice& key,
			   const int64_t count,
                           std::vector<ScoreMember>* score_members){
  return zsets_db_->ZPopMin(key, count, score_members);
}

Status BlackWidow::ZAdd(const Slice& key,
                        const std::vector<ScoreMember>& score_members,
                        int32_t* ret) {
  return zsets_db_->ZAdd(key, score_members, ret);
}

Status BlackWidow::ZCard(const Slice& key,
                         int32_t* ret) {
  return zsets_db_->ZCard(key, ret);
}

Status BlackWidow::ZCount(const Slice& key,
                          double min,
                          double max,
                          bool left_close,
                          bool right_close,
                          int32_t* ret) {
  return zsets_db_->ZCount(key, min, max, left_close, right_close, ret);
}

Status BlackWidow::ZIncrby(const Slice& key,
                           const Slice& member,
                           double increment,
                           double* ret) {
  return zsets_db_->ZIncrby(key, member, increment, ret);
}

Status BlackWidow::ZRange(const Slice& key,
                          int32_t start,
                          int32_t stop,
                          std::vector<ScoreMember>* score_members) {
  return zsets_db_->ZRange(key, start, stop, score_members);
}

Status BlackWidow::ZRangebyscore(const Slice& key,
                                 double min,
                                 double max,
                                 bool left_close,
                                 bool right_close,
                                 std::vector<ScoreMember>* score_members) {
  return zsets_db_->ZRangebyscore(key, min, max,
      left_close, right_close, score_members);
}

Status BlackWidow::ZRank(const Slice& key,
                         const Slice& member,
                         int32_t* rank) {
  return zsets_db_->ZRank(key, member, rank);
}

Status BlackWidow::ZRem(const Slice& key,
                        std::vector<std::string> members,
                        int32_t* ret) {
  return zsets_db_->ZRem(key, members, ret);
}

Status BlackWidow::ZRemrangebyrank(const Slice& key,
                                   int32_t start,
                                   int32_t stop,
                                   int32_t* ret) {
  return zsets_db_->ZRemrangebyrank(key, start, stop, ret);
}

Status BlackWidow::ZRemrangebyscore(const Slice& key,
                                    double min,
                                    double max,
                                    bool left_close,
                                    bool right_close,
                                    int32_t* ret) {
  return zsets_db_->ZRemrangebyscore(key, min, max,
      left_close, right_close, ret);
}

Status BlackWidow::ZRevrange(const Slice& key,
                             int32_t start,
                             int32_t stop,
                             std::vector<ScoreMember>* score_members) {
  return zsets_db_->ZRevrange(key, start, stop, score_members);
}

Status BlackWidow::ZRevrangebyscore(const Slice& key,
                                    double min,
                                    double max,
                                    bool left_close,
                                    bool right_close,
                                    std::vector<ScoreMember>* score_members) {
  return zsets_db_->ZRevrangebyscore(key, min, max,
      left_close, right_close, score_members);
}

Status BlackWidow::ZRevrank(const Slice& key,
                            const Slice& member,
                            int32_t* rank) {
  return zsets_db_->ZRevrank(key, member, rank);
}

Status BlackWidow::ZScore(const Slice& key,
                          const Slice& member,
                          double* ret) {
  return zsets_db_->ZScore(key, member, ret);
}

Status BlackWidow::ZUnionstore(const Slice& destination,
                               const std::vector<std::string>& keys,
                               const std::vector<double>& weights,
                               const AGGREGATE agg,
                               int32_t* ret) {
  return zsets_db_->ZUnionstore(destination, keys, weights, agg, ret);
}

Status BlackWidow::ZInterstore(const Slice& destination,
                               const std::vector<std::string>& keys,
                               const std::vector<double>& weights,
                               const AGGREGATE agg,
                               int32_t* ret) {
  return zsets_db_->ZInterstore(destination, keys, weights, agg, ret);
}

Status BlackWidow::ZRangebylex(const Slice& key,
                               const Slice& min,
                               const Slice& max,
                               bool left_close,
                               bool right_close,
                               std::vector<std::string>* members) {
  return zsets_db_->ZRangebylex(key, min, max,
      left_close, right_close, members);
}

Status BlackWidow::ZLexcount(const Slice& key,
                             const Slice& min,
                             const Slice& max,
                             bool left_close,
                             bool right_close,
                             int32_t* ret) {
  return zsets_db_->ZLexcount(key, min, max, left_close, right_close, ret);
}

Status BlackWidow::ZRemrangebylex(const Slice& key,
                                  const Slice& min,
                                  const Slice& max,
                                  bool left_close,
                                  bool right_close,
                                  int32_t* ret) {
  return zsets_db_->ZRemrangebylex(key, min, max, left_close, right_close, ret);
}

Status BlackWidow::ZScan(const Slice& key, int64_t cursor,
                         const std::string& pattern, int64_t count,
                         std::vector<ScoreMember>* score_members,
                         int64_t* next_cursor) {
  return zsets_db_->ZScan(key, cursor,
      pattern, count, score_members, next_cursor);
}


// Keys Commands
int32_t BlackWidow::Expire(const Slice& key, int32_t ttl,
                           std::map<DataType, Status>* type_status) {
  int32_t ret = 0;
  bool is_corruption = false;

  // Strings
  Status s = strings_db_->Expire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kStrings] = s;
  }

  // Hash
  s = hashes_db_->Expire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kHashes] = s;
  }

  // Sets
  s = sets_db_->Expire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kSets] = s;
  }

  // Lists
  s = lists_db_->Expire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kLists] = s;
  }

  // Zsets
  s = zsets_db_->Expire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kZSets] = s;
  }

  if (is_corruption) {
    return -1;
  } else {
    return ret;
  }
}

int64_t BlackWidow::Del(const std::vector<std::string>& keys,
                        std::map<DataType, Status>* type_status) {
  Status s;
  int64_t count = 0;
  bool is_corruption = false;

  for (const auto& key : keys) {
    // Strings
    Status s = strings_db_->Del(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kStrings] = s;
    }

    // Hashes
    s = hashes_db_->Del(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kHashes] = s;
    }

    // Sets
    s = sets_db_->Del(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kSets] = s;
    }

    // Lists
    s = lists_db_->Del(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kLists] = s;
    }

    // ZSets
    s = zsets_db_->Del(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kZSets] = s;
    }
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

int64_t BlackWidow::DelByType(const std::vector<std::string>& keys,
                              const DataType& type) {
  Status s;
  int64_t count = 0;
  bool is_corruption = false;

  for (const auto& key : keys) {
    switch (type) {
      // Strings
      case DataType::kStrings:
      {
        s = strings_db_->Del(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // Hashes
      case DataType::kHashes:
      {
        s = hashes_db_->Del(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // Sets
      case DataType::kSets:
      {
        s = sets_db_->Del(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // Lists
      case DataType::kLists:
      {
        s = lists_db_->Del(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // ZSets
      case DataType::kZSets:
      {
        s = zsets_db_->Del(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      case DataType::kAll:
      {
        return -1;
      }
    }
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}


int64_t BlackWidow::Exists(const std::vector<std::string>& keys,
                       std::map<DataType, Status>* type_status) {
  int64_t count = 0;
  int32_t ret;
  uint64_t llen;
  std::string value;
  Status s;
  bool is_corruption = false;

  for (const auto& key : keys) {
    s = strings_db_->Get(key, &value);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kStrings] = s;
    }

    s = hashes_db_->HLen(key, &ret);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kHashes] = s;
    }

    s = sets_db_->SCard(key, &ret);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kSets] = s;
    }

    s = lists_db_->LLen(key, &llen);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kLists] = s;
    }

    s = zsets_db_->ZCard(key, &ret);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
      (*type_status)[DataType::kZSets] = s;
    }
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

int64_t BlackWidow::Scan(const DataType& dtype, int64_t cursor,
                         const std::string& pattern, int64_t count,
                         std::vector<std::string>* keys) {
  keys->clear();
  bool is_finish;
  int64_t leftover_visits = count;
  int64_t step_length = count, cursor_ret = 0;
  std::string start_key, next_key, prefix;

  prefix = isTailWildcard(pattern) ?
    pattern.substr(0, pattern.size() - 1) : "";

  if (cursor < 0) {
    return cursor_ret;
  } else {
    Status s = GetStartKey(dtype, cursor, &start_key);
    if (s.IsNotFound()) {
      // If want to scan all the databases, we start with the strings database
      start_key = (dtype == DataType::kAll
          ? DataTypeTag[kStrings] : DataTypeTag[dtype]) + prefix;
      cursor = 0;
    }
  }

  char key_type = start_key.at(0);
  start_key.erase(start_key.begin());
  switch (key_type) {
    case 'k':
      is_finish = strings_db_->Scan(start_key, pattern,
                                    keys, &leftover_visits, &next_key);
      if (!leftover_visits && !is_finish) {
        cursor_ret = cursor + step_length;
        StoreCursorStartKey(dtype, cursor_ret, std::string("k") + next_key);
        break;
      } else if (is_finish) {
        if (DataType::kStrings == dtype) {
          cursor_ret = 0;
          break;
        } else if (!leftover_visits) {
          cursor_ret = cursor + step_length;
          StoreCursorStartKey(dtype, cursor_ret, std::string("h") + prefix);
          break;
        }
      }
      start_key = prefix;
    case 'h':
      is_finish = hashes_db_->Scan(start_key, pattern,
                                   keys, &leftover_visits, &next_key);
      if (!leftover_visits && !is_finish) {
        cursor_ret = cursor + step_length;
        StoreCursorStartKey(dtype, cursor_ret, std::string("h") + next_key);
        break;
      } else if (is_finish) {
        if (DataType::kHashes == dtype) {
          cursor_ret = 0;
          break;
        } else if (!leftover_visits) {
          cursor_ret = cursor + step_length;
          StoreCursorStartKey(dtype, cursor_ret, std::string("s") + prefix);
          break;
        }
      }
      start_key = prefix;
    case 's':
      is_finish = sets_db_->Scan(start_key, pattern,
                                 keys, &leftover_visits, &next_key);
      if (!leftover_visits && !is_finish) {
        cursor_ret = cursor + step_length;
        StoreCursorStartKey(dtype, cursor_ret, std::string("s") + next_key);
        break;
      } else if (is_finish) {
        if (DataType::kSets == dtype) {
          cursor_ret = 0;
          break;
        } else if (!leftover_visits) {
          cursor_ret = cursor + step_length;
          StoreCursorStartKey(dtype, cursor_ret, std::string("l") + prefix);
          break;
        }
      }
      start_key = prefix;
    case 'l':
      is_finish = lists_db_->Scan(start_key, pattern,
                                  keys, &leftover_visits, &next_key);
      if (!leftover_visits && !is_finish) {
        cursor_ret = cursor + step_length;
        StoreCursorStartKey(dtype, cursor_ret, std::string("l") + next_key);
        break;
      } else if (is_finish) {
        if (DataType::kLists == dtype) {
          cursor_ret = 0;
          break;
        } else if (!leftover_visits) {
          cursor_ret = cursor + step_length;
          StoreCursorStartKey(dtype, cursor_ret, std::string("z") + prefix);
          break;
        }
      }
      start_key = prefix;
    case 'z':
      is_finish = zsets_db_->Scan(start_key, pattern,
                                  keys, &leftover_visits, &next_key);
      if (!leftover_visits && !is_finish) {
        cursor_ret = cursor + step_length;
        StoreCursorStartKey(dtype, cursor_ret, std::string("z") + next_key);
        break;
      } else if (is_finish) {
        cursor_ret = 0;
        break;
      }
  }
  return cursor_ret;
}

Status BlackWidow::PKScanRange(const DataType& data_type,
                               const Slice& key_start, const Slice& key_end,
                               const Slice& pattern, int32_t limit,
                               std::vector<std::string>* keys,
                               std::vector<KeyValue>* kvs,
                               std::string* next_key) {
  Status s;
  keys->clear();
  next_key->clear();
  switch (data_type) {
    case DataType::kStrings:
      s = strings_db_->PKScanRange(key_start, key_end,
          pattern, limit, kvs, next_key);
      break;
    case DataType::kHashes:
      s = hashes_db_->PKScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    case DataType::kLists:
      s = lists_db_->PKScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    case DataType::kZSets:
      s = zsets_db_->PKScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    case DataType::kSets:
      s = sets_db_->PKScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    default:
      s = Status::Corruption("Unsupported data types");
      break;
  }
  return s;
}

Status BlackWidow::PKRScanRange(const DataType& data_type,
                                const Slice& key_start, const Slice& key_end,
                                const Slice& pattern, int32_t limit,
                                std::vector<std::string>* keys,
                                std::vector<KeyValue>* kvs,
                                std::string* next_key) {
  Status s;
  keys->clear();
  next_key->clear();
  switch (data_type) {
    case DataType::kStrings:
      s = strings_db_->PKRScanRange(key_start, key_end,
          pattern, limit, kvs, next_key);
      break;
    case DataType::kHashes:
      s = hashes_db_->PKRScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    case DataType::kLists:
      s = lists_db_->PKRScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    case DataType::kZSets:
      s = zsets_db_->PKRScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    case DataType::kSets:
      s = sets_db_->PKRScanRange(key_start, key_end,
          pattern, limit, keys, next_key);
      break;
    default:
      s = Status::Corruption("Unsupported data types");
      break;
  }
  return s;
}

Status BlackWidow::PKPatternMatchDel(const DataType& data_type,
                                     const std::string& pattern,
                                     int32_t* ret) {
  Status s;
  switch (data_type) {
    case DataType::kStrings:
      s = strings_db_->PKPatternMatchDel(pattern, ret);
      break;
    case DataType::kHashes:
      s = hashes_db_->PKPatternMatchDel(pattern, ret);
      break;
    case DataType::kLists:
      s = lists_db_->PKPatternMatchDel(pattern, ret);
      break;
    case DataType::kZSets:
      s = zsets_db_->PKPatternMatchDel(pattern, ret);
      break;
    case DataType::kSets:
      s = sets_db_->PKPatternMatchDel(pattern, ret);
      break;
    default:
      s = Status::Corruption("Unsupported data type");
      break;
  }
  return s;
}

Status BlackWidow::Scanx(const DataType& data_type,
                         const std::string& start_key,
                         const std::string& pattern,
                         int64_t count,
                         std::vector<std::string>* keys,
                         std::string* next_key) {
  Status s;
  keys->clear();
  next_key->clear();
  switch (data_type) {
    case DataType::kStrings:
      strings_db_->Scan(start_key, pattern, keys, &count, next_key);
      break;
    case DataType::kHashes:
      hashes_db_->Scan(start_key, pattern, keys, &count, next_key);
      break;
    case DataType::kLists:
      lists_db_->Scan(start_key, pattern, keys, &count, next_key);
      break;
    case DataType::kZSets:
      zsets_db_->Scan(start_key, pattern, keys, &count, next_key);
      break;
    case DataType::kSets:
      sets_db_->Scan(start_key, pattern, keys, &count, next_key);
      break;
    default:
      Status::Corruption("Unsupported data types");
      break;
  }
  return s;
}

int32_t BlackWidow::Expireat(const Slice& key, int32_t timestamp,
                             std::map<DataType, Status>* type_status) {
  Status s;
  int32_t count = 0;
  bool is_corruption = false;

  s = strings_db_->Expireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kStrings] = s;
  }

  s = hashes_db_->Expireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kHashes] = s;
  }

  s = sets_db_->Expireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kSets] = s;
  }

  s = lists_db_->Expireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kLists] = s;
  }

  s = zsets_db_->Expireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kLists] = s;
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

int32_t BlackWidow::Persist(const Slice& key,
                            std::map<DataType, Status>* type_status) {
  Status s;
  int32_t count = 0;
  bool is_corruption = false;

  s = strings_db_->Persist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kStrings] = s;
  }

  s = hashes_db_->Persist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kHashes] = s;
  }

  s = sets_db_->Persist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kSets] = s;
  }

  s = lists_db_->Persist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kLists] = s;
  }

  s = zsets_db_->Persist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kLists] = s;
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

std::map<DataType, int64_t> BlackWidow::TTL(const Slice& key,
                        std::map<DataType, Status>* type_status) {
  Status s;
  std::map<DataType, int64_t> ret;
  int64_t timestamp = 0;

  s = strings_db_->TTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kStrings] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kStrings] = -3;
    (*type_status)[DataType::kStrings] = s;
  }

  s = hashes_db_->TTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kHashes] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kHashes] = -3;
    (*type_status)[DataType::kHashes] = s;
  }

  s = lists_db_->TTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kLists] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kLists] = -3;
    (*type_status)[DataType::kLists] = s;
  }

  s = sets_db_->TTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kSets] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kSets] = -3;
    (*type_status)[DataType::kSets] = s;
  }

  s = zsets_db_->TTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kZSets] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kZSets] = -3;
    (*type_status)[DataType::kZSets] = s;
  }
  return ret;
}

// the sequence is kv, hash, list, zset, set
Status BlackWidow::Type(const std::string &key, std::string* type) {
  type->clear();

  Status s;
  std::string value;
  s = strings_db_->Get(key, &value);
  if (s.ok()) {
    *type = "string";
    return s;
  } else if (!s.IsNotFound()) {
    return s;
  }

  int32_t hashes_len = 0;
  s = hashes_db_->HLen(key, &hashes_len);
  if (s.ok() && hashes_len != 0) {
    *type = "hash";
    return s;
  } else if (!s.IsNotFound()) {
    return s;
  }

  uint64_t lists_len = 0;
  s = lists_db_->LLen(key, &lists_len);
  if (s.ok() && lists_len != 0) {
    *type = "list";
    return s;
  } else if (!s.IsNotFound()) {
    return s;
  }

  int32_t zsets_size = 0;
  s = zsets_db_->ZCard(key, &zsets_size);
  if (s.ok() && zsets_size != 0) {
    *type = "zset";
    return s;
  } else if (!s.IsNotFound()) {
    return s;
  }

  int32_t sets_size = 0;
  s = sets_db_->SCard(key, &sets_size);
  if (s.ok() && sets_size != 0) {
    *type = "set";
    return s;
  } else if (!s.IsNotFound()) {
    return s;
  }

  *type = "none";
  return Status::OK();
}

Status BlackWidow::Keys(const DataType& data_type,
                        const std::string& pattern,
                        std::vector<std::string>* keys) {
  Status s;
  if (data_type == DataType::kStrings) {
    s = strings_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
  } else if (data_type == DataType::kHashes) {
    s = hashes_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
  } else if (data_type == DataType::kZSets) {
    s = zsets_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
  } else if (data_type == DataType::kSets) {
    s = sets_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
  } else if (data_type == DataType::kLists) {
    s = lists_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
  } else {
    s = strings_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
    s = hashes_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
    s = zsets_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
    s = sets_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
    s = lists_db_->ScanKeys(pattern, keys);
    if (!s.ok()) return s;
  }
  return s;
}

void BlackWidow::ScanDatabase(const DataType& type) {
  switch (type) {
    case kStrings:
        strings_db_->ScanDatabase();
        break;
    case kHashes:
        hashes_db_->ScanDatabase();
        break;
    case kSets:
        sets_db_->ScanDatabase();
        break;
    case kZSets:
        zsets_db_->ScanDatabase();
        break;
    case kLists:
        lists_db_->ScanDatabase();
        break;
    case kAll:
        strings_db_->ScanDatabase();
        hashes_db_->ScanDatabase();
        sets_db_->ScanDatabase();
        zsets_db_->ScanDatabase();
        lists_db_->ScanDatabase();
        break;
  }
}

// HyperLogLog
Status BlackWidow::PfAdd(const Slice& key,
                         const std::vector<std::string>& values,
                         bool* update) {
  *update = false;
  if (values.size() >= kMaxKeys) {
    return Status::InvalidArgument("Invalid the number of key");
  }

  std::string value, registers, result = "";
  Status s = strings_db_->Get(key, &value);
  if (s.ok()) {
    registers = value;
  } else if (s.IsNotFound()) {
    registers = "";
  } else {
    return s;
  }
  HyperLogLog log(kPrecision, registers);
  int32_t previous = static_cast<int32_t>(log.Estimate());
  for (size_t i = 0; i < values.size(); ++i) {
    result = log.Add(values[i].data(), values[i].size());
  }
  HyperLogLog update_log(kPrecision, result);
  int32_t now = static_cast<int32_t>(update_log.Estimate());
  if (previous != now || (s.IsNotFound() && values.size() == 0)) {
    *update = true;
  }
  s = strings_db_->Set(key, result);
  return s;
}

Status BlackWidow::PfCount(const std::vector<std::string>& keys,
                           int64_t* result) {
  if (keys.size() >= kMaxKeys || keys.size() <= 0) {
    return Status::InvalidArgument("Invalid the number of key");
  }

  std::string value, first_registers;
  Status s = strings_db_->Get(keys[0], &value);
  if (s.ok()) {
    first_registers = std::string(value.data(), value.size());
  } else if (s.IsNotFound()) {
    first_registers = "";
  }

  HyperLogLog first_log(kPrecision, first_registers);
  for (size_t i = 1; i < keys.size(); ++i) {
    std::string value, registers;
    s = strings_db_->Get(keys[i], &value);
    if (s.ok()) {
      registers = value;
    } else if (s.IsNotFound()) {
      continue;
    } else {
      return s;
    }
    HyperLogLog log(kPrecision, registers);
    first_log.Merge(log);
  }
  *result = static_cast<int32_t>(first_log.Estimate());
  return Status::OK();
}

Status BlackWidow::PfMerge(const std::vector<std::string>& keys) {
  if (keys.size() >= kMaxKeys || keys.size() <= 0) {
    return Status::InvalidArgument("Invalid the number of key");
  }

  Status s;
  std::string value, first_registers, result;
  s = strings_db_->Get(keys[0], &value);
  if (s.ok()) {
    first_registers = std::string(value.data(), value.size());
  } else if (s.IsNotFound()) {
    first_registers = "";
  }

  result = first_registers;
  HyperLogLog first_log(kPrecision, first_registers);
  for (size_t i = 1; i < keys.size(); ++i) {
    std::string value, registers;
    s = strings_db_->Get(keys[i], &value);
    if (s.ok()) {
      registers = std::string(value.data(), value.size());
    } else if (s.IsNotFound()) {
      continue;
    } else {
      return s;
    }
    HyperLogLog log(kPrecision, registers);
    result = first_log.Merge(log);
  }
  s = strings_db_->Set(keys[0], result);
  return s;
}

static void* StartBGThreadWrapper(void* arg) {
  BlackWidow* bw = reinterpret_cast<BlackWidow*>(arg);
  bw->RunBGTask();
  return NULL;
}

Status BlackWidow::StartBGThread() {
  int result = pthread_create(&bg_tasks_thread_id_,
      NULL, StartBGThreadWrapper, this);
  if (result != 0) {
    char msg[128];
    snprintf(msg, sizeof(msg), "pthread create: %s", strerror(result));
    return Status::Corruption(msg);
  }
  return Status::OK();
}

Status BlackWidow::AddBGTask(const BGTask& bg_task) {
  bg_tasks_mutex_.Lock();
  if (bg_task.type == kAll) {
    // if current task it is global compact,
    // clear the bg_tasks_queue_;
    std::queue<BGTask> empty_queue;
    bg_tasks_queue_.swap(empty_queue);
  }
  bg_tasks_queue_.push(bg_task);
  bg_tasks_cond_var_.Signal();
  bg_tasks_mutex_.Unlock();
  return Status::OK();
}

Status BlackWidow::RunBGTask() {
  BGTask task;
  while (!bg_tasks_should_exit_) {
    bg_tasks_mutex_.Lock();
    while (bg_tasks_queue_.empty() && !bg_tasks_should_exit_) {
      bg_tasks_cond_var_.Wait();
    }

    if (!bg_tasks_queue_.empty()) {
      task = bg_tasks_queue_.front();
      bg_tasks_queue_.pop();
    }
    bg_tasks_mutex_.Unlock();

    if (bg_tasks_should_exit_) {
      return Status::Incomplete("bgtask return with bg_tasks_should_exit true");
    }

    if (task.operation == kCleanAll) {
      DoCompact(task.type);
    } else if (task.operation == kCompactKey) {
      CompactKey(task.type, task.argv);
    }
  }
  return Status::OK();
}

Status BlackWidow::Compact(const DataType& type, bool sync) {
  if (sync) {
    return DoCompact(type);
  } else {
    AddBGTask({type, kCleanAll});
  }
  return Status::OK();
}

Status BlackWidow::DoCompact(const DataType& type) {
  if (type != kAll
    && type != kStrings
    && type != kHashes
    && type != kSets
    && type != kZSets
    && type != kLists) {
    return Status::InvalidArgument("");
  }

  Status s;
  if (type == kStrings) {
    current_task_type_ = Operation::kCleanStrings;
    s = strings_db_->CompactRange(NULL, NULL);
  } else if (type == kHashes) {
    current_task_type_ = Operation::kCleanHashes;
    s = hashes_db_->CompactRange(NULL, NULL);
  } else if (type == kSets) {
    current_task_type_ = Operation::kCleanSets;
    s = sets_db_->CompactRange(NULL, NULL);
  } else if (type == kZSets) {
    current_task_type_ = Operation::kCleanZSets;
    s = zsets_db_->CompactRange(NULL, NULL);
  } else if (type == kLists) {
    current_task_type_ = Operation::kCleanLists;
    s = lists_db_->CompactRange(NULL, NULL);
  } else {
    current_task_type_ = Operation::kCleanAll;
    s = strings_db_->CompactRange(NULL, NULL);
    s = hashes_db_->CompactRange(NULL, NULL);
    s = sets_db_->CompactRange(NULL, NULL);
    s = zsets_db_->CompactRange(NULL, NULL);
    s = lists_db_->CompactRange(NULL, NULL);
  }
  current_task_type_ = Operation::kNone;
  return s;
}

Status BlackWidow::CompactKey(const DataType& type, const std::string& key) {
  std::string meta_start_key, meta_end_key;
  std::string data_start_key, data_end_key;
  CalculateMetaStartAndEndKey(key, &meta_start_key, &meta_end_key);
  CalculateDataStartAndEndKey(key, &data_start_key, &data_end_key);
  Slice slice_meta_begin(meta_start_key);
  Slice slice_meta_end(meta_end_key);
  Slice slice_data_begin(data_start_key);
  Slice slice_data_end(data_end_key);
  if (type == kSets) {
    sets_db_->CompactRange(&slice_meta_begin, &slice_meta_end, kMeta);
    sets_db_->CompactRange(&slice_data_begin, &slice_data_end, kData);
  } else if (type == kZSets) {
    zsets_db_->CompactRange(&slice_meta_begin, &slice_meta_end, kMeta);
    zsets_db_->CompactRange(&slice_data_begin, &slice_data_end, kData);
  } else if (type == kHashes) {
    hashes_db_->CompactRange(&slice_meta_begin, &slice_meta_end, kMeta);
    hashes_db_->CompactRange(&slice_data_begin, &slice_data_end, kData);
  } else if (type == kLists) {
    lists_db_->CompactRange(&slice_meta_begin, &slice_meta_end, kMeta);
    lists_db_->CompactRange(&slice_data_begin, &slice_data_end, kData);
  }
  return Status::OK();
}

Status BlackWidow::SetMaxCacheStatisticKeys(uint32_t max_cache_statistic_keys) {
  std::vector<Redis*> dbs = {sets_db_, zsets_db_, hashes_db_, lists_db_};
  for (const auto& db : dbs) {
    db->SetMaxCacheStatisticKeys(max_cache_statistic_keys);
  }
  return Status::OK();
}

Status BlackWidow::SetSmallCompactionThreshold(uint32_t small_compaction_threshold) {
  std::vector<Redis*> dbs = {sets_db_, zsets_db_, hashes_db_, lists_db_};
  for (const auto& db : dbs) {
    db->SetSmallCompactionThreshold(small_compaction_threshold);
  }
  return Status::OK();
}

std::string BlackWidow::GetCurrentTaskType() {
  int type = current_task_type_;
  switch (type) {
    case kCleanAll:
      return "All";
    case kCleanStrings:
      return "String";
    case kCleanHashes:
      return "Hash";
    case kCleanZSets:
      return "ZSet";
    case kCleanSets:
      return "Set";
    case kCleanLists:
      return "List";
    case kNone:
    default:
      return "No";
  }
}

Status BlackWidow::GetUsage(const std::string& property, uint64_t* const result) {
  *result = GetProperty(ALL_DB, property);
  return Status::OK();
}

Status BlackWidow::GetUsage(const std::string& property,
                            std::map<std::string, uint64_t>* const type_result) {
  type_result->clear();
  (*type_result)[STRINGS_DB] = GetProperty(STRINGS_DB, property);
  (*type_result)[HASHES_DB]  = GetProperty(HASHES_DB,  property);
  (*type_result)[LISTS_DB]   = GetProperty(LISTS_DB,   property);
  (*type_result)[ZSETS_DB]   = GetProperty(ZSETS_DB,   property);
  (*type_result)[SETS_DB]    = GetProperty(SETS_DB,    property);
  return Status::OK();
}

uint64_t BlackWidow::GetProperty(const std::string& db_type,
                                 const std::string& property) {
  uint64_t out = 0, result = 0;
  if (db_type == ALL_DB || db_type == STRINGS_DB) {
    strings_db_->GetProperty(property, &out);
    result += out;
  }
  if (db_type == ALL_DB || db_type == HASHES_DB) {
    hashes_db_->GetProperty(property, &out);
    result += out;
  }
  if (db_type == ALL_DB || db_type == LISTS_DB) {
    lists_db_->GetProperty(property, &out);
    result += out;
  }
  if (db_type == ALL_DB || db_type == ZSETS_DB) {
    zsets_db_->GetProperty(property, &out);
    result += out;
  }
  if (db_type == ALL_DB || db_type == SETS_DB) {
    sets_db_->GetProperty(property, &out);
    result += out;
  }
  return result;
}

Status BlackWidow::GetKeyNum(std::vector<KeyInfo>* key_infos) {
  KeyInfo key_info;
  // NOTE: keep the db order with string, hash, list, zset, set
  std::vector<Redis*> dbs = {strings_db_, hashes_db_,
    lists_db_, zsets_db_, sets_db_};
  for (const auto& db : dbs) {
    // check the scanner was stopped or not, before scanning the next db
    if (scan_keynum_exit_) {
      break;
    }
    db->ScanKeyNum(&key_info);
    key_infos->push_back(key_info);
  }
  if (scan_keynum_exit_) {
    scan_keynum_exit_ = false;
    return Status::Corruption("exit");
  }
  return Status::OK();
}

Status BlackWidow::StopScanKeyNum() {
  scan_keynum_exit_ = true;
  return Status::OK();
}

rocksdb::DB* BlackWidow::GetDBByType(const std::string& type) {
  if (type == STRINGS_DB) {
    return strings_db_->GetDB();
  } else if (type == HASHES_DB) {
    return hashes_db_->GetDB();
  } else if (type == LISTS_DB) {
    return lists_db_->GetDB();
  } else if (type == SETS_DB) {
    return sets_db_->GetDB();
  } else if (type == ZSETS_DB) {
    return zsets_db_->GetDB();
  } else {
    return NULL;
  }
}

}  //  namespace blackwidow
