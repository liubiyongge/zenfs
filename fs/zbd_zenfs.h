// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>
#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include <errno.h>
#include <libzbd/zbd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>
#include <spdlog/spdlog.h>

#include "metrics.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/io_status.h"

namespace ROCKSDB_NAMESPACE {

class ZonedBlockDevice;
class ZonedBlockDeviceBackend;
class ZoneSnapshot;
class ZenFSSnapshotOptions;

class ZoneList {
 private:
  void *data_;
  unsigned int zone_count_;

 public:
  ZoneList(void *data, unsigned int zone_count)
      : data_(data), zone_count_(zone_count){};
  void *GetData() { return data_; };
  unsigned int ZoneCount() { return zone_count_; };
  ~ZoneList() { free(data_); };
};

class Zone {
  ZonedBlockDevice *zbd_;
  ZonedBlockDeviceBackend *zbd_be_;
  std::atomic_bool busy_;

 public:
  explicit Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
                std::unique_ptr<ZoneList> &zones, unsigned int idx);

  uint64_t start_;
  uint64_t capacity_; /* remaining capacity */
  uint64_t max_capacity_;
  uint64_t wp_;
  Env::WriteLifeTimeHint lifetime_;
  std::atomic<uint64_t> used_capacity_;
  bool useinlevelzone_ = false;

  IOStatus Reset();
  IOStatus Finish();
  IOStatus Close();

  IOStatus Append(char *data, uint32_t size);
  bool IsUsed();
  bool IsFull();
  bool IsEmpty();
  uint64_t GetZoneNr();
  uint64_t GetCapacityLeft();
  bool IsBusy() { return this->busy_.load(std::memory_order_relaxed); }
  bool Acquire() {
    bool expected = false;
    return this->busy_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel);
  }
  bool Release() {
    bool expected = true;
    return this->busy_.compare_exchange_strong(expected, false,
                                               std::memory_order_acq_rel);
  }

  void EncodeJson(std::ostream &json_stream);

  inline IOStatus CheckRelease();
};

class ZonedBlockDeviceBackend {
 public:
  uint32_t block_sz_ = 0;
  uint64_t zone_sz_ = 0;
  uint32_t nr_zones_ = 0;

 public:
  virtual IOStatus Open(bool readonly, bool exclusive,
                        unsigned int *max_active_zones,
                        unsigned int *max_open_zones) = 0;

  virtual std::unique_ptr<ZoneList> ListZones() = 0;
  virtual IOStatus Reset(uint64_t start, bool *offline,
                         uint64_t *max_capacity) = 0;
  virtual IOStatus Finish(uint64_t start) = 0;
  virtual IOStatus Close(uint64_t start) = 0;
  virtual int Read(char *buf, int size, uint64_t pos, bool direct) = 0;
  virtual int Write(char *data, uint32_t size, uint64_t pos) = 0;
  virtual int InvalidateCache(uint64_t pos, uint64_t size) = 0;
  virtual bool ZoneIsSwr(std::unique_ptr<ZoneList> &zones,
                         unsigned int idx) = 0;
  virtual bool ZoneIsOffline(std::unique_ptr<ZoneList> &zones,
                             unsigned int idx) = 0;
  virtual bool ZoneIsWritable(std::unique_ptr<ZoneList> &zones,
                              unsigned int idx) = 0;
  virtual bool ZoneIsActive(std::unique_ptr<ZoneList> &zones,
                            unsigned int idx) = 0;
  virtual bool ZoneIsOpen(std::unique_ptr<ZoneList> &zones,
                          unsigned int idx) = 0;
  virtual uint64_t ZoneStart(std::unique_ptr<ZoneList> &zones,
                             unsigned int idx) = 0;
  virtual uint64_t ZoneMaxCapacity(std::unique_ptr<ZoneList> &zones,
                                   unsigned int idx) = 0;
  virtual uint64_t ZoneWp(std::unique_ptr<ZoneList> &zones,
                          unsigned int idx) = 0;
  virtual std::string GetFilename() = 0;
  uint32_t GetBlockSize() { return block_sz_; };
  uint64_t GetZoneSize() { return zone_sz_; };
  uint32_t GetNrZones() { return nr_zones_; };
  virtual ~ZonedBlockDeviceBackend(){};
};

enum class ZbdBackendType {
  kBlockDev,
  kZoneFS,
};

class ZonedBlockDevice {
 public:
  std::shared_ptr<Logger> logger_;
 private:
  std::unique_ptr<ZonedBlockDeviceBackend> zbd_be_;
  std::vector<Zone *> io_zones;
  std::vector<Zone *> meta_zones;
  time_t start_time_;
  uint32_t finish_threshold_ = 0;
  std::atomic<uint64_t> bytes_written_{0};
  // std::mutex gclk; 单线程GC不需要锁
  std::vector<uint64_t> gc_bytes_written_;

  std::atomic<long> active_io_zones_;
  std::atomic<long> open_io_zones_;
  /* Protects zone_resuorces_  condition variable, used
     for notifying changes in open_io_zones_ */
  //std::mutex zone_resources_mtx_;
  //std::condition_variable zone_resources_;
  std::mutex zone_deferred_status_mutex_;
  IOStatus zone_deferred_status_;

  std::condition_variable migrate_resource_;
  std::mutex migrate_zone_mtx_;
  std::atomic<bool> migrating_{false};
  //Preserve tow zones for gC
  Zone *gc_zone_{nullptr};
  Zone *gc_aux_zone_{nullptr};
  unsigned int max_nr_active_io_zones_;
  unsigned int max_nr_open_io_zones_;
  //level zone
  const uint32_t diff_level_num_ = 7;
  const uint32_t lifetime_begin_ = 2;
  std::vector<std::unordered_set<Zone *>> level_zones;//里面的每个zone已经获取了open_io_token active_io_token
  std::mutex level_zones_mtx_;
  std::condition_variable level_zone_resources_;
  std::vector<std::atomic<long>> level_active_io_zones_;//正数就是有，0就是没了

  //wal 0/1  2 3 4 5 6 
  std::shared_ptr<ZenFSMetrics> metrics_;

  void EncodeJsonZone(std::ostream &json_stream,
                      const std::vector<Zone *> zones);

 public:
  explicit ZonedBlockDevice(std::string path, ZbdBackendType backend,
                            std::shared_ptr<Logger> logger,
                            std::shared_ptr<ZenFSMetrics> metrics =
                                std::make_shared<NoZenFSMetrics>());
  virtual ~ZonedBlockDevice();

  IOStatus Open(bool readonly, bool exclusive);

  //initial level zones
  void InitialLevelZones();
  bool EmitLevelZone(Zone* emit_zone);
  void ReleaseLevelZone(Zone* release_zone, uint64_t file_id);
  bool IsLevelZone(Zone * z){
    return z->lifetime_ >= lifetime_begin_;
  }
  
  Zone *GetIOZone(uint64_t offset);
  //Get and set GC tow zones
  Zone *GetGCZone() {return gc_zone_; }
  void SetGCZone(Zone *zone) { gc_zone_ = zone; }
  Zone *GetGCAuxZone() {return gc_aux_zone_; }
  void SetGCAuxZone(Zone *zone) { gc_aux_zone_ = zone; }  
  IOStatus AllocateIOZone(Env::WriteLifeTimeHint file_lifetime, IOType io_type,
                          Zone **out_zone, uint64_t file_id);
  IOStatus AllocateMetaZone(Zone **out_meta_zone);
  IOStatus AllocateEmptyZoneForGC(bool is_aux);
  uint64_t GetFreeSpace();
  uint64_t GetUsedSpace();
  uint64_t GetReclaimableSpace();

  std::string GetFilename();
  uint32_t GetBlockSize();

  IOStatus ResetUnusedIOZones();
  void LogZoneStats();
  void LogZoneUsage();
  void LogGarbageInfo();

  uint64_t GetZoneSize();
  uint32_t GetNrZones();
  std::vector<Zone *> GetMetaZones() { return meta_zones; }

  void SetFinishTreshold(uint32_t threshold) { finish_threshold_ = threshold; }

  void PutOpenIOZoneToken();
  void PutActiveIOZoneToken();


  void EncodeJson(std::ostream &json_stream);

  void SetZoneDeferredStatus(IOStatus status);

  std::shared_ptr<ZenFSMetrics> GetMetrics() { return metrics_; }

  void GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot);

  int Read(char *buf, uint64_t offset, int n, bool direct);
  IOStatus InvalidateCache(uint64_t pos, uint64_t size);

  IOStatus ReleaseMigrateZone(Zone *zone);

  IOStatus TakeMigrateZone(Zone **out_zone, Env::WriteLifeTimeHint lifetime,
                           uint32_t min_capacity);

  void AddBytesWritten(uint64_t written) { bytes_written_ += written; };
  void AddGCBytesWritten(uint64_t written, Env::WriteLifeTimeHint file_lifetime) { 
    
    gc_bytes_written_[file_lifetime] += written; 
    
  };
  uint64_t GetUserBytesWritten() {
    return bytes_written_.load() -  std::accumulate(gc_bytes_written_.begin(), gc_bytes_written_.end(), 0);
  };
  uint64_t GetTotalBytesWritten() { return bytes_written_.load(); };
  void PrintDataMovementSize();
 private:
  IOStatus GetZoneDeferredStatus();
  bool GetActiveIOZoneTokenIfAvailable();
  void WaitForOpenIOZoneToken(bool prioritized);
  IOStatus ApplyFinishThreshold();
  IOStatus FinishCheapestIOZone();
  IOStatus GetBestOpenZoneMatch(Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  IOStatus AllocateEmptyZone(Zone **zone_out);
};

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
