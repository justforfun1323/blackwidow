//  Copyright (c) 2017-present The blackwidow Authors.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "src/redis_sets.h"

#include <map>
#include <memory>
#include <random>
#include <algorithm>

#include "blackwidow/util.h"
#include "src/base_filter.h"
#include "src/scope_snapshot.h"
#include "src/scope_record_lock.h"

namespace blackwidow {

RedisSets::RedisSets(BlackWidow* const bw, const DataType& type)
    : Redis(bw, type) {
  spop_counts_store_ = new LRUCache<std::string, size_t>();
  spop_counts_store_->SetCapacity(1000);
}

RedisSets::~RedisSets() {
  std::vector<rocksdb::ColumnFamilyHandle*> tmp_handles = handles_;
  handles_.clear();
  for (auto handle : tmp_handles) {
    delete handle;
  }
  delete spop_counts_store_;
}

Status RedisSets::Open(const BlackwidowOptions& bw_options,
                       const std::string& db_path) {
  statistics_store_->SetCapacity(bw_options.statistics_max_size);
  small_compaction_threshold_ = bw_options.small_compaction_threshold;

  rocksdb::Options ops(bw_options.options);
  Status s = rocksdb::DB::Open(ops, db_path, &db_);
  if (s.ok()) {
    // create column family
    rocksdb::ColumnFamilyHandle* cf;
    rocksdb::ColumnFamilyOptions cfo;
    s = db_->CreateColumnFamily(cfo, "member_cf", &cf);
    if (!s.ok()) {
      return s;
    }
    // close DB
    delete cf;
    delete db_;
  }

  // Open
  rocksdb::DBOptions db_ops(bw_options.options);
  rocksdb::ColumnFamilyOptions meta_cf_ops(bw_options.options);
  rocksdb::ColumnFamilyOptions member_cf_ops(bw_options.options);
  meta_cf_ops.compaction_filter_factory =
      std::make_shared<SetsMetaFilterFactory>();
  member_cf_ops.compaction_filter_factory =
      std::make_shared<SetsMemberFilterFactory>(&db_, &handles_);

  // use the bloom filter policy to reduce disk reads
  rocksdb::BlockBasedTableOptions table_ops(bw_options.table_options);
  table_ops.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, true));
  rocksdb::BlockBasedTableOptions meta_cf_table_ops(table_ops);
  rocksdb::BlockBasedTableOptions member_cf_table_ops(table_ops);
  if (!bw_options.share_block_cache && bw_options.block_cache_size > 0) {
    meta_cf_table_ops.block_cache =
      rocksdb::NewLRUCache(bw_options.block_cache_size);
    member_cf_table_ops.block_cache =
      rocksdb::NewLRUCache(bw_options.block_cache_size);
  }
  meta_cf_ops.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(meta_cf_table_ops));
  member_cf_ops.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(member_cf_table_ops));

  std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
  // Meta CF
  column_families.push_back(rocksdb::ColumnFamilyDescriptor(
      rocksdb::kDefaultColumnFamilyName, meta_cf_ops));
  // Member CF
  column_families.push_back(rocksdb::ColumnFamilyDescriptor(
      "member_cf", member_cf_ops));
  return rocksdb::DB::Open(db_ops, db_path, column_families, &handles_, &db_);
}

Status RedisSets::CompactRange(const rocksdb::Slice* begin,
                               const rocksdb::Slice* end,
                               const ColumnFamilyType& type) {
  if (type == kMeta || type == kMetaAndData) {
    db_->CompactRange(default_compact_range_options_, handles_[0], begin, end);
  }
  if (type == kData || type == kMetaAndData) {
    db_->CompactRange(default_compact_range_options_, handles_[1], begin, end);
  }
  return Status::OK();
}

Status RedisSets::GetProperty(const std::string& property, uint64_t* out) {
  std::string value;
  db_->GetProperty(handles_[0], property, &value);
  *out = std::strtoull(value.c_str(), NULL, 10);
  db_->GetProperty(handles_[1], property, &value);
  *out += std::strtoull(value.c_str(), NULL, 10);
  return Status::OK();
}

Status RedisSets::ScanKeyNum(KeyInfo* key_info) {
  uint64_t keys = 0;
  uint64_t expires = 0;
  uint64_t ttl_sum = 0;
  uint64_t invaild_keys = 0;

  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  ScopeSnapshot ss(db_, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;

  int64_t curtime;
  rocksdb::Env::Default()->GetCurrentTime(&curtime);

  rocksdb::Iterator* iter = db_->NewIterator(iterator_options, handles_[0]);
  for (iter->SeekToFirst();
       iter->Valid();
       iter->Next()) {
    ParsedSetsMetaValue parsed_sets_meta_value(iter->value());
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      invaild_keys++;
    } else {
      keys++;
      if (!parsed_sets_meta_value.IsPermanentSurvival()) {
        expires++;
        ttl_sum += parsed_sets_meta_value.timestamp() - curtime;
      }
    }
  }
  delete iter;

  key_info->keys = keys;
  key_info->expires = expires;
  key_info->avg_ttl = (expires != 0) ? ttl_sum / expires : 0;
  key_info->invaild_keys = invaild_keys;
  return Status::OK();
}

Status RedisSets::ScanKeys(const std::string& pattern,
                             std::vector<std::string>* keys) {
  std::string key;
  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  ScopeSnapshot ss(db_, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;

  rocksdb::Iterator* iter = db_->NewIterator(iterator_options, handles_[0]);
  for (iter->SeekToFirst();
       iter->Valid();
       iter->Next()) {
    ParsedSetsMetaValue parsed_sets_meta_value(iter->value());
    if (!parsed_sets_meta_value.IsStale()
      && parsed_sets_meta_value.count() != 0) {
      key = iter->key().ToString();
      if (StringMatch(pattern.data(),
            pattern.size(), key.data(), key.size(), 0)) {
        keys->push_back(key);
      }
    }
  }
  delete iter;
  return Status::OK();
}

Status RedisSets::PKPatternMatchDel(const std::string& pattern,
                                    int32_t* ret) {
  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  ScopeSnapshot ss(db_, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;

  std::string key;
  std::string meta_value;
  int32_t total_delete = 0;
  Status s;
  rocksdb::WriteBatch batch;
  rocksdb::Iterator* iter = db_->NewIterator(iterator_options, handles_[0]);
  iter->SeekToFirst();
  while (iter->Valid()) {
    key = iter->key().ToString();
    meta_value = iter->value().ToString();
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (!parsed_sets_meta_value.IsStale()
      && parsed_sets_meta_value.count()
      && StringMatch(pattern.data(), pattern.size(), key.data(), key.size(), 0)) {
      parsed_sets_meta_value.InitialMetaValue();
      batch.Put(handles_[0], key, meta_value);
    }
    if (static_cast<size_t>(batch.Count()) >= BATCH_DELETE_LIMIT) {
      s = db_->Write(default_write_options_, &batch);
      if (s.ok()) {
        total_delete += batch.Count();
        batch.Clear();
      } else {
        *ret = total_delete;
        return s;
      }
    }
    iter->Next();
  }
  if (batch.Count()) {
    s = db_->Write(default_write_options_, &batch);
    if (s.ok()) {
      total_delete += batch.Count();
      batch.Clear();
    }
  }

  *ret = total_delete;
  return s;
}

Status RedisSets::SAdd(const Slice& key,
                       const std::vector<std::string>& members, int32_t* ret) {
  std::unordered_set<std::string> unique;
  std::vector<std::string> filtered_members;
  for (const auto& member : members) {
    if (unique.find(member) == unique.end()) {
      unique.insert(member);
      filtered_members.push_back(member);
    }
  }

  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  int32_t version = 0;
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      version = parsed_sets_meta_value.InitialMetaValue();
      parsed_sets_meta_value.set_count(filtered_members.size());
      batch.Put(handles_[0], key, meta_value);
      for (const auto& member : filtered_members) {
        SetsMemberKey sets_member_key(key, version, member);
        batch.Put(handles_[1], sets_member_key.Encode(), Slice());
      }
      *ret = filtered_members.size();
    } else {
      int32_t cnt = 0;
      std::string member_value;
      version = parsed_sets_meta_value.version();
      for (const auto& member : filtered_members) {
        SetsMemberKey sets_member_key(key, version, member);
        s = db_->Get(default_read_options_, handles_[1],
                     sets_member_key.Encode(), &member_value);
        if (s.ok()) {
        } else if (s.IsNotFound()) {
          cnt++;
          batch.Put(handles_[1], sets_member_key.Encode(), Slice());
        } else {
          return s;
        }
      }
      *ret = cnt;
      if (cnt == 0) {
        return Status::OK();
      } else {
        parsed_sets_meta_value.ModifyCount(cnt);
        batch.Put(handles_[0], key, meta_value);
      }
    }
  } else if (s.IsNotFound()) {
    char str[4];
    EncodeFixed32(str, filtered_members.size());
    SetsMetaValue sets_meta_value(Slice(str, sizeof(int32_t)));
    version = sets_meta_value.UpdateVersion();
    batch.Put(handles_[0], key, sets_meta_value.Encode());
    for (const auto& member : filtered_members) {
      SetsMemberKey sets_member_key(key, version, member);
      batch.Put(handles_[1], sets_member_key.Encode(), Slice());
    }
    *ret = filtered_members.size();
  } else {
    return s;
  }
  return db_->Write(default_write_options_, &batch);
}

Status RedisSets::SCard(const Slice& key, int32_t* ret) {
  *ret = 0;
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else {
      *ret = parsed_sets_meta_value.count();
      if (*ret == 0) {
        return Status::NotFound("Deleted");
      }
    }
  }
  return s;
}

Status RedisSets::SDiff(const std::vector<std::string>& keys,
                        std::vector<std::string>* members) {
  if (keys.size() <= 0) {
    return Status::Corruption("SDiff invalid parameter, no keys");
  }

  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  int32_t version = 0;
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  std::vector<KeyVersion> vaild_sets;
  Status s;

  for (uint32_t idx = 1; idx < keys.size(); ++idx) {
    s = db_->Get(read_options, handles_[0], keys[idx], &meta_value);
    if (s.ok()) {
      ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
      if (!parsed_sets_meta_value.IsStale()
        && parsed_sets_meta_value.count() != 0) {
        vaild_sets.push_back({keys[idx], parsed_sets_meta_value.version()});
      }
    } else if (!s.IsNotFound()) {
      return s;
    }
  }

  s = db_->Get(read_options, handles_[0], keys[0], &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (!parsed_sets_meta_value.IsStale()
      && parsed_sets_meta_value.count() != 0) {
      bool found;
      Slice prefix;
      std::string member_value;
      version = parsed_sets_meta_value.version();
      SetsMemberKey sets_member_key(keys[0], version, Slice());
      prefix = sets_member_key.Encode();
      auto iter = db_->NewIterator(read_options, handles_[1]);
      for (iter->Seek(prefix);
           iter->Valid() && iter->key().starts_with(prefix);
           iter->Next()) {
        ParsedSetsMemberKey parsed_sets_member_key(iter->key());
        Slice member = parsed_sets_member_key.member();

        found = false;
        for (const auto& key_version : vaild_sets) {
          SetsMemberKey sets_member_key(key_version.key,
                  key_version.version, member);
          s = db_->Get(read_options, handles_[1],
                  sets_member_key.Encode(), &member_value);
          if (s.ok()) {
            found = true;
            break;
          } else if (!s.IsNotFound()) {
            delete iter;
            return s;
          }
        }
        if (!found) {
          members->push_back(member.ToString());
        }
      }
      delete iter;
    }
  } else if (!s.IsNotFound()) {
    return s;
  }
  return Status::OK();
}

Status RedisSets::SDiffstore(const Slice& destination,
                             const std::vector<std::string>& keys,
                             int32_t* ret) {
  if (keys.size() <= 0) {
    return Status::Corruption("SDiffsotre invalid parameter, no keys");
  }

  rocksdb::WriteBatch batch;
  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  int32_t version = 0;
  ScopeRecordLock l(lock_mgr_, destination);
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  std::vector<KeyVersion> vaild_sets;
  Status s;

  for (uint32_t idx = 1; idx < keys.size(); ++idx) {
    s = db_->Get(read_options, handles_[0], keys[idx], &meta_value);
    if (s.ok()) {
      ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
      if (!parsed_sets_meta_value.IsStale()
        && parsed_sets_meta_value.count() != 0) {
        vaild_sets.push_back({keys[idx], parsed_sets_meta_value.version()});
      }
    } else if (!s.IsNotFound()) {
      return s;
    }
  }

  std::vector<std::string> members;
  s = db_->Get(read_options, handles_[0], keys[0], &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (!parsed_sets_meta_value.IsStale()
      && parsed_sets_meta_value.count() != 0) {
      bool found;
      std::string member_value;
      version = parsed_sets_meta_value.version();
      SetsMemberKey sets_member_key(keys[0], version, Slice());
      Slice prefix = sets_member_key.Encode();
      auto iter = db_->NewIterator(read_options, handles_[1]);
      for (iter->Seek(prefix);
           iter->Valid() && iter->key().starts_with(prefix);
           iter->Next()) {
        ParsedSetsMemberKey parsed_sets_member_key(iter->key());
        Slice member = parsed_sets_member_key.member();

        found = false;
        for (const auto& key_version : vaild_sets) {
          SetsMemberKey sets_member_key(key_version.key,
                  key_version.version, member);
          s = db_->Get(read_options, handles_[1],
                  sets_member_key.Encode(), &member_value);
          if (s.ok()) {
            found = true;
            break;
          } else if (!s.IsNotFound()) {
            delete iter;
            return s;
          }
        }
        if (!found) {
          members.push_back(member.ToString());
        }
      }
      delete iter;
    }
  } else if (!s.IsNotFound()) {
    return s;
  }

  uint32_t statistic = 0;
  s = db_->Get(read_options, handles_[0], destination, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    statistic = parsed_sets_meta_value.count();
    version = parsed_sets_meta_value.InitialMetaValue();
    parsed_sets_meta_value.set_count(members.size());
    batch.Put(handles_[0], destination, meta_value);
  } else if (s.IsNotFound()) {
    char str[4];
    EncodeFixed32(str, members.size());
    SetsMetaValue sets_meta_value(Slice(str, sizeof(int32_t)));
    version = sets_meta_value.UpdateVersion();
    batch.Put(handles_[0], destination, sets_meta_value.Encode());
  } else {
    return s;
  }
  for (const auto& member : members) {
    SetsMemberKey sets_member_key(destination, version, member);
    batch.Put(handles_[1], sets_member_key.Encode(), Slice());
  }
  *ret = members.size();
  s = db_->Write(default_write_options_, &batch);
  UpdateSpecificKeyStatistics(destination.ToString(), statistic);
  return s;
}

Status RedisSets::SInter(const std::vector<std::string>& keys,
                         std::vector<std::string>* members) {
  if (keys.size() <= 0) {
    return Status::Corruption("SInter invalid parameter, no keys");
  }

  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  int32_t version = 0;
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  std::vector<KeyVersion> vaild_sets;
  Status s;

  for (uint32_t idx = 1; idx < keys.size(); ++idx) {
    s = db_->Get(read_options, handles_[0], keys[idx], &meta_value);
    if (s.ok()) {
      ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
      if (parsed_sets_meta_value.IsStale()
        || parsed_sets_meta_value.count() == 0) {
        return Status::OK();
      } else {
        vaild_sets.push_back({keys[idx], parsed_sets_meta_value.version()});
      }
    } else if (s.IsNotFound()) {
      return Status::OK();
    } else {
      return s;
    }
  }

  s = db_->Get(read_options, handles_[0], keys[0], &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      return Status::OK();
    } else {
      bool reliable;
      std::string member_value;
      version = parsed_sets_meta_value.version();
      SetsMemberKey sets_member_key(keys[0], version, Slice());
      Slice prefix = sets_member_key.Encode();
      auto iter = db_->NewIterator(read_options, handles_[1]);
      for (iter->Seek(prefix);
           iter->Valid() && iter->key().starts_with(prefix);
           iter->Next()) {
        ParsedSetsMemberKey parsed_sets_member_key(iter->key());
        Slice member = parsed_sets_member_key.member();

        reliable = true;
        for (const auto& key_version : vaild_sets) {
          SetsMemberKey sets_member_key(key_version.key,
                  key_version.version, member);
          s = db_->Get(read_options, handles_[1],
                  sets_member_key.Encode(), &member_value);
          if (s.ok()) {
            continue;
          } else if (s.IsNotFound()) {
            reliable = false;
            break;
          } else {
            delete iter;
            return s;
          }
        }
        if (reliable) {
          members->push_back(member.ToString());
        }
      }
      delete iter;
    }
  } else if (s.IsNotFound()) {
    return Status::OK();
  } else {
    return s;
  }
  return Status::OK();
}

Status RedisSets::SInterstore(const Slice& destination,
                              const std::vector<std::string>& keys,
                              int32_t* ret) {
  if (keys.size() <= 0) {
    return Status::Corruption("SInterstore invalid parameter, no keys");
  }

  rocksdb::WriteBatch batch;
  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  int32_t version = 0;
  bool have_invalid_sets = false;
  ScopeRecordLock l(lock_mgr_, destination);
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  std::vector<KeyVersion> vaild_sets;
  Status s;

  for (uint32_t idx = 1; idx < keys.size(); ++idx) {
    s = db_->Get(read_options, handles_[0], keys[idx], &meta_value);
    if (s.ok()) {
      ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
      if (parsed_sets_meta_value.IsStale()
        || parsed_sets_meta_value.count() == 0) {
        have_invalid_sets = true;
        break;
      } else {
        vaild_sets.push_back({keys[idx], parsed_sets_meta_value.version()});
      }
    } else if (s.IsNotFound()) {
      have_invalid_sets = true;
      break;
    } else {
      return s;
    }
  }

  std::vector<std::string> members;
  if (!have_invalid_sets) {
    s = db_->Get(read_options, handles_[0], keys[0], &meta_value);
    if (s.ok()) {
      ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
      if (parsed_sets_meta_value.IsStale()
        || parsed_sets_meta_value.count() == 0) {
        have_invalid_sets = true;
      } else {
        bool reliable;
        std::string member_value;
        version = parsed_sets_meta_value.version();
        SetsMemberKey sets_member_key(keys[0], version, Slice());
        Slice prefix = sets_member_key.Encode();
        auto iter = db_->NewIterator(read_options, handles_[1]);
        for (iter->Seek(prefix);
             iter->Valid() && iter->key().starts_with(prefix);
             iter->Next()) {
          ParsedSetsMemberKey parsed_sets_member_key(iter->key());
          Slice member = parsed_sets_member_key.member();

          reliable = true;
          for (const auto& key_version : vaild_sets) {
            SetsMemberKey sets_member_key(key_version.key,
                    key_version.version, member);
            s = db_->Get(read_options, handles_[1],
                    sets_member_key.Encode(), &member_value);
            if (s.ok()) {
              continue;
            } else if (s.IsNotFound()) {
              reliable = false;
              break;
            } else {
              delete iter;
              return s;
            }
          }
          if (reliable) {
            members.push_back(member.ToString());
          }
        }
        delete iter;
      }
    } else if (s.IsNotFound()) {
    } else {
      return s;
    }
  }

  uint32_t statistic = 0;
  s = db_->Get(read_options, handles_[0], destination, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    statistic = parsed_sets_meta_value.count();
    version = parsed_sets_meta_value.InitialMetaValue();
    parsed_sets_meta_value.set_count(members.size());
    batch.Put(handles_[0], destination, meta_value);
  } else if (s.IsNotFound()) {
    char str[4];
    EncodeFixed32(str, members.size());
    SetsMetaValue sets_meta_value(Slice(str, sizeof(int32_t)));
    version = sets_meta_value.UpdateVersion();
    batch.Put(handles_[0], destination, sets_meta_value.Encode());
  } else {
    return s;
  }
  for (const auto& member : members) {
    SetsMemberKey sets_member_key(destination, version, member);
    batch.Put(handles_[1], sets_member_key.Encode(), Slice());
  }
  *ret = members.size();
  s = db_->Write(default_write_options_, &batch);
  UpdateSpecificKeyStatistics(destination.ToString(), statistic);
  return s;
}

Status RedisSets::SIsmember(const Slice& key, const Slice& member,
                            int32_t* ret) {
  *ret = 0;
  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  int32_t version = 0;
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  Status s = db_->Get(read_options, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      std::string member_value;
      version = parsed_sets_meta_value.version();
      SetsMemberKey sets_member_key(key, version, member);
      s = db_->Get(read_options, handles_[1],
              sets_member_key.Encode(), &member_value);
      *ret = s.ok() ? 1 : 0;
    }
  } else if (s.IsNotFound()) {
    *ret = 0;
  }
  return s;
}

Status RedisSets::SMembers(const Slice& key,
                           std::vector<std::string>* members) {
  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  int32_t version = 0;
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  Status s = db_->Get(read_options, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      version = parsed_sets_meta_value.version();
      SetsMemberKey sets_member_key(key, version, Slice());
      Slice prefix = sets_member_key.Encode();
      auto iter = db_->NewIterator(read_options, handles_[1]);
      for (iter->Seek(prefix);
           iter->Valid() && iter->key().starts_with(prefix);
           iter->Next()) {
        ParsedSetsMemberKey parsed_sets_member_key(iter->key());
        members->push_back(parsed_sets_member_key.member().ToString());
      }
      delete iter;
    }
  }
  return s;
}

Status RedisSets::SMove(const Slice& source, const Slice& destination,
                        const Slice& member, int32_t* ret) {
  *ret = 0;
  rocksdb::WriteBatch batch;
  rocksdb::ReadOptions read_options;

  int32_t version = 0;
  uint32_t statistic = 0;
  std::string meta_value;
  std::vector<std::string> keys {source.ToString(), destination.ToString()};
  MultiScopeRecordLock ml(lock_mgr_, keys);

  if (source == destination) {
    *ret = 1;
    return Status::OK();
  }

  Status s = db_->Get(default_read_options_, handles_[0], source, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      std::string member_value;
      version = parsed_sets_meta_value.version();
      SetsMemberKey sets_member_key(source, version, member);
      s = db_->Get(default_read_options_, handles_[1],
              sets_member_key.Encode(), &member_value);
      if (s.ok()) {
        *ret = 1;
        parsed_sets_meta_value.ModifyCount(-1);
        batch.Put(handles_[0], source, meta_value);
        batch.Delete(handles_[1], sets_member_key.Encode());
        statistic++;
      } else if (s.IsNotFound()) {
        *ret = 0;
        return Status::NotFound();
      } else {
        return s;
      }
    }
  } else if (s.IsNotFound()) {
    *ret = 0;
    return Status::NotFound();
  } else {
    return s;
  }

  s = db_->Get(default_read_options_, handles_[0], destination, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      version = parsed_sets_meta_value.InitialMetaValue();
      parsed_sets_meta_value.set_count(1);
      batch.Put(handles_[0], destination, meta_value);
      SetsMemberKey sets_member_key(destination, version, member);
      batch.Put(handles_[1], sets_member_key.Encode(), Slice());
    } else {
      std::string member_value;
      version = parsed_sets_meta_value.version();
      SetsMemberKey sets_member_key(destination, version, member);
      s = db_->Get(default_read_options_, handles_[1],
              sets_member_key.Encode(), &member_value);
      if (s.IsNotFound()) {
        parsed_sets_meta_value.ModifyCount(1);
        batch.Put(handles_[0], destination, meta_value);
        batch.Put(handles_[1], sets_member_key.Encode(), Slice());
      } else if (!s.ok()) {
        return s;
      }
    }
  } else if (s.IsNotFound()) {
    char str[4];
    EncodeFixed32(str, 1);
    SetsMetaValue sets_meta_value(Slice(str, sizeof(int32_t)));
    version = sets_meta_value.UpdateVersion();
    batch.Put(handles_[0], destination, sets_meta_value.Encode());
    SetsMemberKey sets_member_key(destination, version, member);
    batch.Put(handles_[1], sets_member_key.Encode(), Slice());
  } else {
    return s;
  }
  s = db_->Write(default_write_options_, &batch);
  UpdateSpecificKeyStatistics(source.ToString(), 1);
  return s;
}

Status RedisSets::SPop(const Slice& key,
                       std::string* member,
                       bool* need_compact) {
  std::default_random_engine engine;

  std::string meta_value;
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);

  uint64_t start_us = slash::NowMicros();
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      engine.seed(time(NULL));
      int32_t cur_index = 0;
      int32_t size = parsed_sets_meta_value.count();
      int32_t target_index = engine() % (size < 50 ? size : 50);
      int32_t version = parsed_sets_meta_value.version();

      SetsMemberKey sets_member_key(key, version, Slice());
      auto iter = db_->NewIterator(default_read_options_, handles_[1]);
      for (iter->Seek(sets_member_key.Encode());
           iter->Valid() && cur_index < size;
           iter->Next(), cur_index++) {
        if (cur_index == target_index) {
          batch.Delete(handles_[1], iter->key());
          ParsedSetsMemberKey parsed_sets_member_key(iter->key());
          *member = parsed_sets_member_key.member().ToString();

          parsed_sets_meta_value.ModifyCount(-1);
          batch.Put(handles_[0], key, meta_value);
          break;
        }
      }
      delete iter;
    }
  } else {
    return s;
  }
  uint64_t count = 0;
  uint64_t duration = slash::NowMicros() - start_us;
  AddAndGetSpopCount(key.ToString(), &count);
  if (duration >= SPOP_COMPACT_THRESHOLD_DURATION
    || count >= SPOP_COMPACT_THRESHOLD_COUNT) {
    *need_compact = true;
    ResetSpopCount(key.ToString());
  }
  return db_->Write(default_write_options_, &batch);
}

Status RedisSets::ResetSpopCount(const std::string& key) {
  return spop_counts_store_->Remove(key);
}

Status RedisSets::AddAndGetSpopCount(const std::string& key, uint64_t* count) {
  size_t old_count = 0;
  spop_counts_store_->Lookup(key, &old_count);
  spop_counts_store_->Insert(key, old_count + 1);
  *count = old_count + 1;
  return Status::OK();
}

Status RedisSets::SRandmember(const Slice& key, int32_t count,
                              std::vector<std::string>* members) {
  if (count == 0) {
    return Status::OK();
  }

  members->clear();
  int32_t last_seed = time(NULL);
  std::default_random_engine engine;

  std::string meta_value;
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);
  std::vector<int32_t> targets;
  std::unordered_set<int32_t> unique;

  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      int32_t size = parsed_sets_meta_value.count();
      int32_t version = parsed_sets_meta_value.version();
      if (count > 0) {
        count = count <= size ? count : size;
        while (targets.size() < static_cast<size_t>(count)) {
          engine.seed(last_seed);
          last_seed = engine();
          uint32_t pos = last_seed % size;
          if (unique.find(pos) == unique.end()) {
            unique.insert(pos);
            targets.push_back(pos);
          }
        }
      } else {
        count = -count;
        while (targets.size() < static_cast<size_t>(count)) {
          engine.seed(last_seed);
          last_seed = engine();
          targets.push_back(last_seed % size);
        }
      }
      std::sort(targets.begin(), targets.end());

      int32_t cur_index = 0, idx = 0;
      SetsMemberKey sets_member_key(key, version, Slice());
      auto iter = db_->NewIterator(default_read_options_, handles_[1]);
      for (iter->Seek(sets_member_key.Encode());
           iter->Valid() && cur_index < size;
           iter->Next(), cur_index++) {
        if (static_cast<size_t>(idx) >= targets.size()) {
          break;
        }
        ParsedSetsMemberKey parsed_sets_member_key(iter->key());
        while (static_cast<size_t>(idx) < targets.size()
          && cur_index == targets[idx]) {
          idx++;
          members->push_back(parsed_sets_member_key.member().ToString());
        }
      }
      random_shuffle(members->begin(), members->end());
      delete iter;
    }
  }
  return s;
}

Status RedisSets::SRem(const Slice& key,
                       const std::vector<std::string>& members,
                       int32_t* ret) {
  *ret = 0;
  rocksdb::WriteBatch batch;
  ScopeRecordLock l(lock_mgr_, key);

  int32_t version = 0;
  uint32_t statistic = 0;
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      int32_t cnt = 0;
      std::string member_value;
      version = parsed_sets_meta_value.version();
      for (const auto& member : members) {
        SetsMemberKey sets_member_key(key, version, member);
        s = db_->Get(default_read_options_, handles_[1],
                sets_member_key.Encode(), &member_value);
        if (s.ok()) {
          cnt++;
          statistic++;
          batch.Delete(handles_[1], sets_member_key.Encode());
        } else if (s.IsNotFound()) {
        } else {
          return s;
        }
      }
      *ret = cnt;
      parsed_sets_meta_value.ModifyCount(-cnt);
      batch.Put(handles_[0], key, meta_value);
    }
  } else if (s.IsNotFound()) {
    *ret = 0;
    return Status::NotFound();
  } else {
    return s;
  }
  s = db_->Write(default_write_options_, &batch);
  UpdateSpecificKeyStatistics(key.ToString(), statistic);
  return s;
}

Status RedisSets::SUnion(const std::vector<std::string>& keys,
                         std::vector<std::string>* members) {
  if (keys.size() <= 0) {
    return Status::Corruption("SUnion invalid parameter, no keys");
  }

  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  std::vector<KeyVersion> vaild_sets;
  Status s;

  for (uint32_t idx = 0; idx < keys.size(); ++idx) {
    s = db_->Get(read_options, handles_[0], keys[idx], &meta_value);
    if (s.ok()) {
      ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
      if (!parsed_sets_meta_value.IsStale() &&
        parsed_sets_meta_value.count() != 0) {
        vaild_sets.push_back({keys[idx], parsed_sets_meta_value.version()});
      }
    } else if (!s.IsNotFound()) {
      return s;
    }
  }

  Slice prefix;
  std::map<std::string, bool> result_flag;
  for (const auto& key_version : vaild_sets) {
    SetsMemberKey sets_member_key(key_version.key,
        key_version.version, Slice());
    prefix = sets_member_key.Encode();
    auto iter = db_->NewIterator(read_options, handles_[1]);
    for (iter->Seek(prefix);
         iter->Valid() && iter->key().starts_with(prefix);
         iter->Next()) {
      ParsedSetsMemberKey parsed_sets_member_key(iter->key());
      std::string member = parsed_sets_member_key.member().ToString();
      if (result_flag.find(member) == result_flag.end()) {
        members->push_back(member);
        result_flag[member] = true;
      }
    }
    delete iter;
  }
  return Status::OK();
}

Status RedisSets::SUnionstore(const Slice& destination,
                              const std::vector<std::string>& keys,
                              int32_t* ret) {
  if (keys.size() <= 0) {
    return Status::Corruption("SUnionstore invalid parameter, no keys");
  }

  rocksdb::WriteBatch batch;
  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  int32_t version = 0;
  ScopeRecordLock l(lock_mgr_, destination);
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  std::vector<KeyVersion> vaild_sets;
  Status s;

  for (uint32_t idx = 0; idx < keys.size(); ++idx) {
    s = db_->Get(read_options, handles_[0], keys[idx], &meta_value);
    if (s.ok()) {
      ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
      if (!parsed_sets_meta_value.IsStale() &&
        parsed_sets_meta_value.count() != 0) {
        vaild_sets.push_back({keys[idx], parsed_sets_meta_value.version()});
      }
    } else if (!s.IsNotFound()) {
      return s;
    }
  }

  Slice prefix;
  std::vector<std::string> members;
  std::map<std::string, bool> result_flag;
  for (const auto& key_version : vaild_sets) {
    SetsMemberKey sets_member_key(key_version.key,
        key_version.version, Slice());
    prefix = sets_member_key.Encode();
    auto iter = db_->NewIterator(read_options, handles_[1]);
    for (iter->Seek(prefix);
         iter->Valid() && iter->key().starts_with(prefix);
         iter->Next()) {
      ParsedSetsMemberKey parsed_sets_member_key(iter->key());
      std::string member = parsed_sets_member_key.member().ToString();
      if (result_flag.find(member) == result_flag.end()) {
        members.push_back(member);
        result_flag[member] = true;
      }
    }
    delete iter;
  }

  uint32_t statistic = 0;
  s = db_->Get(read_options, handles_[0], destination, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    statistic = parsed_sets_meta_value.count();
    version = parsed_sets_meta_value.InitialMetaValue();
    parsed_sets_meta_value.set_count(members.size());
    batch.Put(handles_[0], destination, meta_value);
  } else if (s.IsNotFound()) {
    char str[4];
    EncodeFixed32(str, members.size());
    SetsMetaValue sets_meta_value(Slice(str, sizeof(int32_t)));
    version = sets_meta_value.UpdateVersion();
    batch.Put(handles_[0], destination, sets_meta_value.Encode());
  } else {
    return s;
  }
  for (const auto& member : members) {
    SetsMemberKey sets_member_key(destination, version, member);
    batch.Put(handles_[1], sets_member_key.Encode(), Slice());
  }
  *ret = members.size();
  s = db_->Write(default_write_options_, &batch);
  UpdateSpecificKeyStatistics(destination.ToString(), statistic);
  return s;
}

Status RedisSets::SScan(const Slice& key,
                        int64_t cursor,
                        const std::string& pattern,
                        int64_t count,
                        std::vector<std::string>* members,
                        int64_t* next_cursor) {
  *next_cursor = 0;
  members->clear();
  if (cursor < 0) {
    *next_cursor = 0;
    return Status::OK();
  }

  int64_t rest = count;
  int64_t step_length = count;
  rocksdb::ReadOptions read_options;
  const rocksdb::Snapshot* snapshot;

  std::string meta_value;
  ScopeSnapshot ss(db_, &snapshot);
  read_options.snapshot = snapshot;
  Status s = db_->Get(read_options, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      *next_cursor = 0;
      return Status::NotFound();
    } else {
      std::string sub_member;
      std::string start_point;
      int32_t version = parsed_sets_meta_value.version();
      s = GetScanStartPoint(key, pattern, cursor, &start_point);
      if (s.IsNotFound()) {
        cursor = 0;
        if (isTailWildcard(pattern)) {
          start_point = pattern.substr(0, pattern.size() - 1);
        }
      }
      if (isTailWildcard(pattern)) {
        sub_member = pattern.substr(0, pattern.size() - 1);
      }

      SetsMemberKey sets_member_prefix(key, version, sub_member);
      SetsMemberKey sets_member_key(key, version, start_point);
      std::string prefix = sets_member_prefix.Encode().ToString();
      rocksdb::Iterator* iter = db_->NewIterator(read_options, handles_[1]);
      for (iter->Seek(sets_member_key.Encode());
           iter->Valid() && rest > 0 && iter->key().starts_with(prefix);
           iter->Next()) {
        ParsedSetsMemberKey parsed_sets_member_key(iter->key());
        std::string member = parsed_sets_member_key.member().ToString();
        if (StringMatch(pattern.data(),
              pattern.size(), member.data(), member.size(), 0)) {
          members->push_back(member);
        }
        rest--;
      }

      if (iter->Valid()
        && (iter->key().compare(prefix) <= 0
          || iter->key().starts_with(prefix))) {
        *next_cursor = cursor + step_length;
        ParsedSetsMemberKey parsed_sets_member_key(iter->key());
        std::string next_member = parsed_sets_member_key.member().ToString();
        StoreScanNextPoint(key, pattern, *next_cursor, next_member);
      } else {
        *next_cursor = 0;
      }
      delete iter;
    }
  } else {
    *next_cursor = 0;
    return s;
  }
  return Status::OK();
}

Status RedisSets::PKScanRange(const Slice& key_start,
                              const Slice& key_end,
                              const Slice& pattern,
                              int32_t limit,
                              std::vector<std::string>* keys,
                              std::string* next_key) {
  next_key->clear();

  std::string key;
  int32_t remain = limit;
  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  ScopeSnapshot ss(db_, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;

  bool start_no_limit = !key_start.compare("");
  bool end_no_limit = !key_end.compare("");

  if (!start_no_limit
    && !end_no_limit
    && (key_start.compare(key_end) > 0)) {
    return Status::InvalidArgument("error in given range");
  }

  rocksdb::Iterator* it = db_->NewIterator(iterator_options, handles_[0]);
  if (start_no_limit) {
    it->SeekToFirst();
  } else {
    it->Seek(key_start);
  }

  while (it->Valid() && remain > 0
    && (end_no_limit || it->key().compare(key_end) <= 0)) {
    ParsedSetsMetaValue parsed_meta_value(it->value());
    if (parsed_meta_value.IsStale()
      || parsed_meta_value.count() == 0) {
      it->Next();
    } else {
      key = it->key().ToString();
      if (StringMatch(pattern.data(), pattern.size(),
                         key.data(), key.size(), 0)) {
        keys->push_back(key);
      }
      remain--;
      it->Next();
    }
  }

  while (it->Valid()
    && (end_no_limit || it->key().compare(key_end) <= 0)) {
    ParsedSetsMetaValue parsed_sets_meta_value(it->value());
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      it->Next();
    } else {
      *next_key = it->key().ToString();
      break;
    }
  }
  delete it;
  return Status::OK();
}

Status RedisSets::PKRScanRange(const Slice& key_start,
                               const Slice& key_end,
                               const Slice& pattern,
                               int32_t limit,
                               std::vector<std::string>* keys,
                               std::string* next_key) {
  next_key->clear();

  std::string key;
  int32_t remain = limit;
  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  ScopeSnapshot ss(db_, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;

  bool start_no_limit = !key_start.compare("");
  bool end_no_limit = !key_end.compare("");

  if (!start_no_limit
    && !end_no_limit
    && (key_start.compare(key_end) < 0)) {
    return Status::InvalidArgument("error in given range");
  }

  rocksdb::Iterator* it = db_->NewIterator(iterator_options, handles_[0]);
  if (start_no_limit) {
    it->SeekToLast();
  } else {
    it->SeekForPrev(key_start);
  }

  while (it->Valid() && remain > 0
    && (end_no_limit || it->key().compare(key_end) >= 0)) {
    ParsedSetsMetaValue parsed_sets_meta_value(it->value());
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      it->Prev();
    } else {
      key = it->key().ToString();
      if (StringMatch(pattern.data(), pattern.size(),
                         key.data(), key.size(), 0)) {
        keys->push_back(key);
      }
      remain--;
      it->Prev();
    }
  }

  while (it->Valid()
    && (end_no_limit || it->key().compare(key_end) >= 0)) {
    ParsedSetsMetaValue parsed_sets_meta_value(it->value());
    if (parsed_sets_meta_value.IsStale()
      || parsed_sets_meta_value.count() == 0) {
      it->Prev();
    } else {
      *next_key = it->key().ToString();
      break;
    }
  }
  delete it;
  return Status::OK();
}

Status RedisSets::Expire(const Slice& key, int32_t ttl) {
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    }

    if (ttl > 0) {
      parsed_sets_meta_value.SetRelativeTimestamp(ttl);
      s = db_->Put(default_write_options_, handles_[0], key, meta_value);
    } else {
      parsed_sets_meta_value.InitialMetaValue();
      s = db_->Put(default_write_options_, handles_[0], key, meta_value);
    }
  }
  return s;
}

Status RedisSets::Del(const Slice& key) {
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      uint32_t statistic = parsed_sets_meta_value.count();
      parsed_sets_meta_value.InitialMetaValue();
      s = db_->Put(default_write_options_, handles_[0], key, meta_value);
      UpdateSpecificKeyStatistics(key.ToString(), statistic);
    }
  }
  return s;
}

bool RedisSets::Scan(const std::string& start_key,
                     const std::string& pattern,
                     std::vector<std::string>* keys,
                     int64_t* count,
                     std::string* next_key) {
  std::string meta_key;
  bool is_finish = true;
  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  ScopeSnapshot ss(db_, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;

  rocksdb::Iterator* it = db_->NewIterator(iterator_options, handles_[0]);

  it->Seek(start_key);
  while (it->Valid() && (*count) > 0) {
    ParsedSetsMetaValue parsed_meta_value(it->value());
    if (parsed_meta_value.IsStale()
      || parsed_meta_value.count() == 0) {
      it->Next();
      continue;
    } else {
      meta_key = it->key().ToString();
      if (StringMatch(pattern.data(), pattern.size(),
                         meta_key.data(), meta_key.size(), 0)) {
        keys->push_back(meta_key);
      }
      (*count)--;
      it->Next();
    }
  }

  std::string prefix = isTailWildcard(pattern) ?
    pattern.substr(0, pattern.size() - 1) : "";
  if (it->Valid()
    && (it->key().compare(prefix) <= 0 || it->key().starts_with(prefix))) {
    *next_key = it->key().ToString();
    is_finish = false;
  } else {
    *next_key = "";
  }
  delete it;
  return is_finish;
}

Status RedisSets::Expireat(const Slice& key, int32_t timestamp) {
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      if (timestamp > 0) {
        parsed_sets_meta_value.set_timestamp(timestamp);
      } else {
        parsed_sets_meta_value.InitialMetaValue();
      }
      return db_->Put(default_write_options_, handles_[0], key, meta_value);
    }
  }
  return s;
}

Status RedisSets::Persist(const Slice& key) {
  std::string meta_value;
  ScopeRecordLock l(lock_mgr_, key);
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_sets_meta_value(&meta_value);
    if (parsed_sets_meta_value.IsStale()) {
      return Status::NotFound("Stale");
    } else if (parsed_sets_meta_value.count() == 0) {
      return Status::NotFound();
    } else {
      int32_t timestamp = parsed_sets_meta_value.timestamp();
      if (timestamp == 0) {
        return Status::NotFound("Not have an associated timeout");
      } else {
        parsed_sets_meta_value.set_timestamp(0);
        return db_->Put(default_write_options_, handles_[0], key, meta_value);
      }
    }
  }
  return s;
}

Status RedisSets::TTL(const Slice& key, int64_t* timestamp) {
  std::string meta_value;
  Status s = db_->Get(default_read_options_, handles_[0], key, &meta_value);
  if (s.ok()) {
    ParsedSetsMetaValue parsed_setes_meta_value(&meta_value);
    if (parsed_setes_meta_value.IsStale()) {
      *timestamp = -2;
      return Status::NotFound("Stale");
    } else if (parsed_setes_meta_value.count() == 0) {
      *timestamp = -2;
      return Status::NotFound();
    } else {
      *timestamp = parsed_setes_meta_value.timestamp();
      if (*timestamp == 0) {
        *timestamp = -1;
      } else {
        int64_t curtime;
        rocksdb::Env::Default()->GetCurrentTime(&curtime);
        *timestamp = *timestamp - curtime >= 0 ? *timestamp - curtime : -2;
      }
    }
  } else if (s.IsNotFound()) {
    *timestamp = -2;
  }
  return s;
}

void RedisSets::ScanDatabase() {
  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  ScopeSnapshot ss(db_, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;
  int32_t current_time = time(NULL);

  printf("\n***************Sets Meta Data***************\n");
  auto meta_iter = db_->NewIterator(iterator_options, handles_[0]);
  for (meta_iter->SeekToFirst();
       meta_iter->Valid();
       meta_iter->Next()) {
    ParsedSetsMetaValue parsed_sets_meta_value(meta_iter->value());
    int32_t survival_time = 0;
    if (parsed_sets_meta_value.timestamp() != 0) {
      survival_time = parsed_sets_meta_value.timestamp() - current_time > 0 ?
        parsed_sets_meta_value.timestamp() - current_time : -1;
    }

    printf("[key : %-30s] [count : %-10d] [timestamp : %-10d] [version : %d] [survival_time : %d]\n",
           meta_iter->key().ToString().c_str(),
           parsed_sets_meta_value.count(),
           parsed_sets_meta_value.timestamp(),
           parsed_sets_meta_value.version(),
           survival_time);
  }
  delete meta_iter;

  printf("\n***************Sets Member Data***************\n");
  auto member_iter = db_->NewIterator(iterator_options, handles_[1]);
  for (member_iter->SeekToFirst();
       member_iter->Valid();
       member_iter->Next()) {
    ParsedSetsMemberKey parsed_sets_member_key(member_iter->key());
    printf("[key : %-30s] [member : %-20s] [version : %d]\n",
           parsed_sets_member_key.key().ToString().c_str(),
           parsed_sets_member_key.member().ToString().c_str(),
           parsed_sets_member_key.version());
  }
  delete member_iter;
}

}  //  namespace blackwidow
