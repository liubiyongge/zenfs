// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "zbd_zenfs.h"

#include <assert.h>
#include <thread>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/io_status.h"
#include "snapshot.h"
#include "zbdlib_zenfs.h"
#include "zonefs_zenfs.h"

#define KB (1024)
#define MB (1024 * KB)

/* Number of reserved zones for metadata
 * Two non-offline meta zones are needed to be able
 * to roll the metadata log safely. One extra
 * is allocated to cover for one zone going offline.
 */
#define ZENFS_META_ZONES (3)

/* Minimum of number of zones that makes sense */
#define ZENFS_MIN_ZONES (32)

namespace ROCKSDB_NAMESPACE {

Zone::Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
           std::unique_ptr<ZoneList> &zones, unsigned int idx)
    : zbd_(zbd),
      zbd_be_(zbd_be),
      busy_(false),
      start_(zbd_be->ZoneStart(zones, idx)),
      max_capacity_(zbd_be->ZoneMaxCapacity(zones, idx)),
      wp_(zbd_be->ZoneWp(zones, idx)) {
  lifetime_ = Env::WLTH_NOT_SET;
  used_capacity_ = 0;
  capacity_ = 0;
  if (zbd_be->ZoneIsWritable(zones, idx))
    capacity_ = max_capacity_ - (wp_ - start_);
}

bool Zone::IsUsed() { return (used_capacity_ > 0); }
uint64_t Zone::GetCapacityLeft() { return capacity_; }
bool Zone::IsFull() { return (capacity_ == 0); }
bool Zone::IsEmpty() { return (wp_ == start_); }
uint64_t Zone::GetZoneNr() { return start_ / zbd_->GetZoneSize(); }

void Zone::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"capacity\":" << capacity_ << ",";
  json_stream << "\"max_capacity\":" << max_capacity_ << ",";
  json_stream << "\"wp\":" << wp_ << ",";
  json_stream << "\"lifetime\":" << lifetime_ << ",";
  json_stream << "\"used_capacity\":" << used_capacity_;
  json_stream << "}";
}

IOStatus Zone::Reset() {
  bool offline;
  uint64_t max_capacity;

  assert(!IsUsed());
  assert(IsBusy());

  IOStatus ios = zbd_be_->Reset(start_, &offline, &max_capacity);
  if (ios != IOStatus::OK()) return ios;

  if (offline)
    capacity_ = 0;
  else
    max_capacity_ = capacity_ = max_capacity;

  wp_ = start_;
  lifetime_ = Env::WLTH_NOT_SET;

  return IOStatus::OK();
}

IOStatus Zone::Finish() {
  assert(IsBusy());

  IOStatus ios = zbd_be_->Finish(start_);
  if (ios != IOStatus::OK()) {
    return ios;
  }
  capacity_ = 0;
  wp_ = start_ + zbd_->GetZoneSize();

  return IOStatus::OK();
}

IOStatus Zone::Close() {
  assert(IsBusy());

  if (!(IsEmpty() || IsFull())) {
    IOStatus ios = zbd_be_->Close(start_);
    if (ios != IOStatus::OK()) return ios;
  }

  return IOStatus::OK();
}

IOStatus Zone::Append(char *data, uint32_t size) {
  ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_ZONE_WRITE_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportThroughput(ZENFS_ZONE_WRITE_THROUGHPUT, size);
  char *ptr = data;
  uint32_t left = size;
  int ret;

  if (capacity_ < size)
    return IOStatus::NoSpace("Not enough capacity for append");

  assert((size % zbd_->GetBlockSize()) == 0);

  while (left) {
    ret = zbd_be_->Write(ptr, left, wp_);
    if (ret < 0) {
      return IOStatus::IOError(strerror(errno));
    }

    ptr += ret;
    wp_ += ret;
    capacity_ -= ret;
    left -= ret;
    zbd_->AddBytesWritten(ret);
  }

  return IOStatus::OK();
}

inline IOStatus Zone::CheckRelease() {
  if (!Release()) {
    assert(false);
    return IOStatus::Corruption("Failed to unset busy flag of zone " +
                                std::to_string(GetZoneNr()));
  }

  return IOStatus::OK();
}

Zone *ZonedBlockDevice::GetIOZone(uint64_t offset) {
  for (const auto z : io_zones)
    if (z->start_ <= offset && offset < (z->start_ + zbd_be_->GetZoneSize()))
      return z;
  return nullptr;
}

void ZonedBlockDevice::InitialLevelZones(){
  //加锁
  std::unique_lock<std::mutex> lk(level_zones_mtx_);
  IOStatus s = IOStatus::OK();
  Zone *allocated = nullptr;
  for(uint32_t i = 0; i < diff_level_num_; i++){
    open_io_zones_++;
    active_io_zones_++;
    s = AllocateEmptyZone(&allocated);
    if(!s.ok()){
      exit(1);
    }
    allocated->lifetime_ = (Env::WriteLifeTimeHint)(i + lifetime_begin_);
    level_zones[i].insert(allocated);

    level_active_io_zones_[i]++;
    Debug(logger_, "lby allocate zone %lu to lifetime %d", allocated->GetZoneNr(), (int)allocated->lifetime_);

  }
}

//true replace old zone with new zone
//false throw old zone
bool ZonedBlockDevice::EmitLevelZone(Zone* emit_zone){
  std::unique_lock<std::mutex> lk(level_zones_mtx_);
  level_zones[emit_zone->lifetime_-lifetime_begin_].erase(emit_zone);
  emit_zone->useinlevelzone_ = false;
  emit_zone->Release();
  Debug(logger_, "lby remove zone %lu from lifetime %d", emit_zone->GetZoneNr(), (int)emit_zone->lifetime_);
  if(level_zones[emit_zone->lifetime_-lifetime_begin_].empty()){
    Zone *allocated = nullptr;
    int wait_count = 0;
    while(!allocated){
      wait_count++;
      IOStatus s = AllocateEmptyZone(&allocated);
      if(!s.ok()){
        exit(1);
      }
      if(!allocated){
        usleep(std::rand() %(std::min(4000 * wait_count, 1000000)));
      }
    }
    allocated->lifetime_ = emit_zone->lifetime_;
    level_zones[emit_zone->lifetime_-lifetime_begin_].insert(allocated);
    Debug(logger_, "lby allocate zone %lu to lifetime %d", allocated->GetZoneNr(), (int)allocated->lifetime_);
    return true;
  }
  active_io_zones_--;
  open_io_zones_--;
  level_zone_resources_.notify_all();
  return false;
}

  void ZonedBlockDevice::ReleaseLevelZone(Zone* release_zone, uint64_t file_id){
    std::unique_lock<std::mutex> lk(level_zones_mtx_);
    level_active_io_zones_[release_zone->lifetime_-lifetime_begin_]++;
    release_zone->useinlevelzone_ = false;
    Debug(logger_, "lby release zone %lu from file %lu", release_zone->GetZoneNr(), file_id);
    level_zone_resources_.notify_all();
  }

ZonedBlockDevice::ZonedBlockDevice(std::string path, ZbdBackendType backend,
                                   std::shared_ptr<Logger> logger,
                                   std::shared_ptr<ZenFSMetrics> metrics)
    : logger_(logger), gc_bytes_written_(11, 0), level_zones(diff_level_num_), level_active_io_zones_(diff_level_num_), metrics_(metrics) {
  if (backend == ZbdBackendType::kBlockDev) {
    zbd_be_ = std::unique_ptr<ZbdlibBackend>(new ZbdlibBackend(path));
    Info(logger_, "New Zoned Block Device: %s", zbd_be_->GetFilename().c_str());
  } else if (backend == ZbdBackendType::kZoneFS) {
    zbd_be_ = std::unique_ptr<ZoneFsBackend>(new ZoneFsBackend(path));
    Info(logger_, "New zonefs backing: %s", zbd_be_->GetFilename().c_str());
  }
}

IOStatus ZonedBlockDevice::Open(bool readonly, bool exclusive) {
  std::unique_ptr<ZoneList> zone_rep;
  unsigned int max_nr_active_zones;
  unsigned int max_nr_open_zones;
  Status s;
  uint64_t i = 0;
  uint64_t m = 0;
  // Reserve one zone for metadata and another one for extent migration
  int reserved_zones = 2;

  if (!readonly && !exclusive)
    return IOStatus::InvalidArgument("Write opens must be exclusive");

  IOStatus ios = zbd_be_->Open(readonly, exclusive, &max_nr_active_zones,
                               &max_nr_open_zones);
  if (ios != IOStatus::OK()) return ios;

  if (zbd_be_->GetNrZones() < ZENFS_MIN_ZONES) {
    return IOStatus::NotSupported("To few zones on zoned backend (" +
                                  std::to_string(ZENFS_MIN_ZONES) +
                                  " required)");
  }

  if (max_nr_active_zones == 0)
    max_nr_active_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_active_io_zones_ = max_nr_active_zones - reserved_zones;

  if (max_nr_open_zones == 0)
    max_nr_open_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_open_io_zones_ = max_nr_open_zones - reserved_zones;

  Info(logger_, "Zone block device nr zones: %u max active: %u max open: %u \n",
       zbd_be_->GetNrZones(), max_nr_active_zones, max_nr_open_zones);

  zone_rep = zbd_be_->ListZones();
  if (zone_rep == nullptr || zone_rep->ZoneCount() != zbd_be_->GetNrZones()) {
    Error(logger_, "Failed to list zones");
    return IOStatus::IOError("Failed to list zones");
  }

  while (m < ZENFS_META_ZONES && i < zone_rep->ZoneCount()) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        meta_zones.push_back(new Zone(this, zbd_be_.get(), zone_rep, i));
      }
      m++;
    }
    i++;
  }

  active_io_zones_ = 0;
  open_io_zones_ = 0;

  for (; i < zone_rep->ZoneCount(); i++) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        Zone *newZone = new Zone(this, zbd_be_.get(), zone_rep, i);
        if (!newZone->Acquire()) {
          assert(false);
          return IOStatus::Corruption("Failed to set busy flag of zone " +
                                      std::to_string(newZone->GetZoneNr()));
        }
        io_zones.push_back(newZone);
        if (zbd_be_->ZoneIsActive(zone_rep, i)) {
          active_io_zones_++;
          if (zbd_be_->ZoneIsOpen(zone_rep, i)) {
            if (!readonly) {
              newZone->Close();
            }
          }
        }
        IOStatus status = newZone->CheckRelease();
        if (!status.ok()) {
          return status;
        }
      }
    }
  }

  start_time_ = time(NULL);

  return IOStatus::OK();
}

uint64_t ZonedBlockDevice::GetFreeSpace() {
  uint64_t free = 0;
  for (const auto z : io_zones) {
    free += z->capacity_;
  }
  return free;
}

uint64_t ZonedBlockDevice::GetUsedSpace() {
  uint64_t used = 0;
  for (const auto z : io_zones) {
    used += z->used_capacity_;
  }
  return used;
}

uint64_t ZonedBlockDevice::GetReclaimableSpace() {
  uint64_t reclaimable = 0;
  for (const auto z : io_zones) {
    if (z->IsFull()) reclaimable += (z->max_capacity_ - z->used_capacity_);
  }
  return reclaimable;
}

void ZonedBlockDevice::LogZoneStats() {
  uint64_t used_capacity = 0;
  uint64_t reclaimable_capacity = 0;
  uint64_t reclaimables_max_capacity = 0;
  uint64_t active = 0;

  for (const auto z : io_zones) {
    used_capacity += z->used_capacity_;

    if (z->used_capacity_) {
      reclaimable_capacity += z->max_capacity_ - z->used_capacity_;
      reclaimables_max_capacity += z->max_capacity_;
    }

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }

  if (reclaimables_max_capacity == 0) reclaimables_max_capacity = 1;

  Info(logger_,
       "[Zonestats:time(s),used_cap(MB),reclaimable_cap(MB), "
       "avg_reclaimable(%%), active(#), active_zones(#), open_zones(#)] %ld "
       "%lu %lu %lu %lu %ld %ld\n",
       time(NULL) - start_time_, used_capacity / MB, reclaimable_capacity / MB,
       100 * reclaimable_capacity / reclaimables_max_capacity, active,
       active_io_zones_.load(), open_io_zones_.load());
}

void ZonedBlockDevice::LogZoneUsage() {
  for (const auto z : io_zones) {
    int64_t used = z->used_capacity_;

    if (used > 0) {
      Debug(logger_, "Zone 0x%lX used capacity: %ld bytes (%ld MB)\n",
            z->start_, used, used / MB);
    }
  }
}

void ZonedBlockDevice::LogGarbageInfo() {
  // Log zone garbage stats vector.
  //
  // The values in the vector represents how many zones with target garbage
  // percent. Garbage percent of each index: [0%, <10%, < 20%, ... <100%, 100%]
  // For example `[100, 1, 2, 3....]` means 100 zones are empty, 1 zone has less
  // than 10% garbage, 2 zones have  10% ~ 20% garbage ect.
  //
  // We don't need to lock io_zones since we only read data and we don't need
  // the result to be precise.
  int zone_gc_stat[12] = {0};
  for (auto z : io_zones) {
    if (!z->Acquire()) {
      continue;
    }

    if (z->IsEmpty()) {
      zone_gc_stat[0]++;
      z->Release();
      continue;
    }

    double garbage_rate = 0;
    if (z->IsFull()) {
      garbage_rate =
          double(z->max_capacity_ - z->used_capacity_) / z->max_capacity_;
    } else {
      garbage_rate =
          double(z->wp_ - z->start_ - z->used_capacity_) / z->max_capacity_;
    }
    assert(garbage_rate > 0);
    int idx = int((garbage_rate + 0.1) * 10);
    zone_gc_stat[idx]++;

    z->Release();
  }

  std::stringstream ss;
  ss << "Zone Garbage Stats: [";
  for (int i = 0; i < 12; i++) {
    ss << zone_gc_stat[i] << " ";
  }
  ss << "]";
  Info(logger_, "%s", ss.str().data());
}

void ZonedBlockDevice::PrintDataMovementSize(){
    uint64_t sumGC = 0;
    int len = gc_bytes_written_.size();
    for(int i = 0; i < len; i++){
      printf("Lifetime %d Data Movement in Garbage Collecting %lu MB\n", i, gc_bytes_written_[i] / (1024 * 1024));
      sumGC += gc_bytes_written_[i];
    }
    printf("Data Movement in Garbage Collecting %lu MB\n", sumGC / (1024 * 1024));
  }
ZonedBlockDevice::~ZonedBlockDevice() {
  PrintDataMovementSize();
  for (const auto z : meta_zones) {
    delete z;
  }

#define LIFETIME_DIFF_NOT_GOOD (100)
#define LIFETIME_DIFF_COULD_BE_WORSE (50)

unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime,
                             Env::WriteLifeTimeHint file_lifetime) {
  assert(file_lifetime <= Env::WLTH_EXTREME);

  if ((file_lifetime == Env::WLTH_NOT_SET) ||
      (file_lifetime == Env::WLTH_NONE)) {
    if (file_lifetime == zone_lifetime) {
      return 0;
    } else {
      return LIFETIME_DIFF_NOT_GOOD;
    }
  }

  if (zone_lifetime > file_lifetime) return zone_lifetime - file_lifetime;
  if (zone_lifetime == file_lifetime) return 0;

  return LIFETIME_DIFF_NOT_GOOD;
}

IOStatus ZonedBlockDevice::AllocateMetaZone(Zone **out_meta_zone) {
  assert(out_meta_zone);
  *out_meta_zone = nullptr;
  ZenFSMetricsLatencyGuard guard(metrics_, ZENFS_META_ALLOC_LATENCY,
                                 Env::Default());
  metrics_->ReportQPS(ZENFS_META_ALLOC_QPS, 1);

  for (const auto z : meta_zones) {
    /* If the zone is not used, reset and use it */
    if (z->Acquire()) {
      if (!z->IsUsed()) {
        if (!z->IsEmpty() && !z->Reset().ok()) {
          Warn(logger_, "Failed resetting zone!");
          IOStatus status = z->CheckRelease();
          if (!status.ok()) return status;
          continue;
        }
        *out_meta_zone = z;
        return IOStatus::OK();
      }
    }
  }
  assert(true);
  Error(logger_, "Out of metadata zones, we should go to read only now.");
  return IOStatus::NoSpace("Out of metadata zones");
}

IOStatus ZonedBlockDevice::ResetUnusedIOZones() {
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (!z->IsEmpty() && !z->IsUsed()) {//已经被用过且zone内全部为无效数据。
        bool full = z->IsFull();
        Debug(logger_, "Reset Zone %lu", z->GetZoneNr());
        IOStatus reset_status = z->Reset();
        IOStatus release_status = z->CheckRelease();
        if (!reset_status.ok()) return reset_status;
        if (!release_status.ok()) return release_status;
        //不是full说明其是正在打开的Zone
        if (!full){
          if(IsLevelZone(z)){
            EmitLevelZone(z);
          }else{
            PutActiveIOZoneToken();
          }
        } 
        
      } else {
        IOStatus release_status = z->CheckRelease();
        if (!release_status.ok()) return release_status;
      }
    }
  }
  return IOStatus::OK();
}

void ZonedBlockDevice::WaitForOpenIOZoneToken(bool prioritized) {
  long allocator_open_limit;

  /* Avoid non-priortized allocators from starving prioritized ones */
  if (prioritized) {
    allocator_open_limit = max_nr_open_io_zones_;
  } else {
    allocator_open_limit = max_nr_open_io_zones_ - 1;
  }

  /* Wait for an open IO Zone token - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutOpenIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(level_zones_mtx_);
  level_zone_resources_.wait(lk, [this, allocator_open_limit] {
    if (open_io_zones_.load() < allocator_open_limit) {
      open_io_zones_++;
      return true;
    } else {
      return false;
    }
  });
}

bool ZonedBlockDevice::GetActiveIOZoneTokenIfAvailable() {
  /* Grap an active IO Zone token if available - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutActiveIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(level_zones_mtx_);
  if (active_io_zones_.load() < max_nr_active_io_zones_) {
    active_io_zones_++;
    return true;
  }
  return false;
}

void ZonedBlockDevice::PutOpenIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(level_zones_mtx_);
    open_io_zones_--;
  }
  level_zone_resources_.notify_all();
}

void ZonedBlockDevice::PutActiveIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(level_zones_mtx_);
    active_io_zones_--;
  }
  level_zone_resources_.notify_all();
}

IOStatus ZonedBlockDevice::ApplyFinishThreshold() {
  IOStatus s;

  if (finish_threshold_ == 0) return IOStatus::OK();

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      bool within_finish_threshold =
          z->capacity_ < (z->max_capacity_ * finish_threshold_ / 100);
      if (!(z->IsEmpty() || z->IsFull()) && within_finish_threshold) {
        /* If there is less than finish_threshold_% remaining capacity in a
         * non-open-zone, finish the zone */
        s = z->Finish();
        Debug(logger_, "Finish Zone %lu", z->GetZoneNr());
        if (!s.ok()) {
          z->Release();
          Debug(logger_, "Failed finishing zone");
          return s;
        }
        s = z->CheckRelease();
        if (!s.ok()) return s;
        PutActiveIOZoneToken();
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::FinishCheapestIOZone() {
  IOStatus s;
  Zone *finish_victim = nullptr;

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty() || z->IsFull()) {
        s = z->CheckRelease();
        if (!s.ok()) return s;
        continue;
      }
      if (finish_victim == nullptr) {
        finish_victim = z;
        continue;
      }
      if (finish_victim->capacity_ > z->capacity_) {
        s = finish_victim->CheckRelease();
        if (!s.ok()) return s;
        finish_victim = z;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  // If all non-busy zones are empty or full, we should return success.
  if (finish_victim == nullptr) {
    //Info(logger_, "All non-busy zones are empty or full, skip.");
    return IOStatus::OK();
  }
  Debug(logger_, "Finish Zone %lu left %lu", finish_victim->GetZoneNr(), finish_victim->GetCapacityLeft());
  s = finish_victim->Finish();
  
  IOStatus release_status = finish_victim->CheckRelease();
  if (s.ok()) {
    PutActiveIOZoneToken();
  }else{
    std::ostringstream oss;
    oss << std::this_thread::get_id() << std::endl;

    Debug(logger_, "Zone finish error %ld in thread %s]n", finish_victim->GetZoneNr(), oss.str().c_str());
    exit(1);
  }

  if (!release_status.ok()) {
    return release_status;
  }

  return s;
}

// IOStatus ZonedBlockDevice::GetBestOpenZoneMatch(
//     Env::WriteLifeTimeHint file_lifetime, unsigned int *best_diff_out,
//     Zone **zone_out, uint32_t min_capacity) {
//   unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
//   Zone *allocated_zone = nullptr;
//   IOStatus s;

//   for (const auto z : io_zones) {
//     if (z->Acquire()) {
//       if ((z->used_capacity_ > 0) && !z->IsFull() &&
//           z->capacity_ >= min_capacity) {
//         unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
//         if (diff <= 1) {
//           allocated_zone = z;
//           best_diff = diff;
//           break;
//         } else {
//           s = z->CheckRelease();
//           if (!s.ok()) return s;
//         }
//       } else {
//         s = z->CheckRelease();
//         if (!s.ok()) return s;
//       }
//     }
//   }

//   *best_diff_out = best_diff;
//   *zone_out = allocated_zone;

//   return IOStatus::OK();
// }

IOStatus ZonedBlockDevice::GetBestOpenZoneMatch(
    Env::WriteLifeTimeHint file_lifetime, unsigned int *best_diff_out,
    Zone **zone_out, uint32_t min_capacity) {
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if ((z->used_capacity_ > 0) && !z->IsFull() &&
          z->capacity_ >= min_capacity) {
        unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
        if (diff <= best_diff) {
          if (allocated_zone != nullptr) {
            s = allocated_zone->CheckRelease();
            if (!s.ok()) {
              IOStatus s_ = z->CheckRelease();
              if (!s_.ok()) return s_;
              return s;
            }
          }
          allocated_zone = z;
          best_diff = diff;
        } else {
          s = z->CheckRelease();
          if (!s.ok()) return s;
        }
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZone(Zone **zone_out) {
  IOStatus s;
  Zone *allocated_zone = nullptr;
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty()) {
        allocated_zone = z;
        break;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }
  *zone_out = allocated_zone;
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZoneForGC(bool is_aux) {
  bool get_token = false;
  IOStatus s = IOStatus::OK();
  Zone *allocated = nullptr;
  if(!is_aux){
    WaitForOpenIOZoneToken(false);
    while (!get_token) get_token = GetActiveIOZoneTokenIfAvailable();
  }


  s = AllocateEmptyZone(&allocated);
  if(!is_aux){
    if (!s.ok()) {
      PutOpenIOZoneToken();
      PutActiveIOZoneToken();
      return s;
    }
  }
  allocated->lifetime_ = (Env::WriteLifeTimeHint)(3 + 2);
  if (!is_aux) SetGCZone(allocated);
  else SetGCAuxZone(allocated);

  return s;
}

IOStatus ZonedBlockDevice::InvalidateCache(uint64_t pos, uint64_t size) {
  int ret = zbd_be_->InvalidateCache(pos, size);

  if (ret) {
    return IOStatus::IOError("Failed to invalidate cache");
  }
  return IOStatus::OK();
}

int ZonedBlockDevice::Read(char *buf, uint64_t offset, int n, bool direct) {
  int ret = 0;
  int left = n;
  int r = -1;

  while (left) {
    r = zbd_be_->Read(buf, left, offset, direct);
    if (r <= 0) {
      if (r == -1 && errno == EINTR) {
        continue;
      }
      break;
    }
    ret += r;
    buf += r;
    left -= r;
    offset += r;
  }

  if (r < 0) return r;
  return ret;
}

IOStatus ZonedBlockDevice::ReleaseMigrateZone(Zone *zone) {
  IOStatus s = IOStatus::OK();
  {
    // std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
    // migrating_ = false;
    if (zone != nullptr && zone != GetGCZone()) {
      s = zone->CheckRelease();
      Info(logger_, "ReleaseMigrateZone: %lu", zone->GetZoneNr());
    }
  }
  // migrate_resource_.notify_one();
  return s;
}

IOStatus ZonedBlockDevice::TakeMigrateZone(Zone **out_zone,
                                           Env::WriteLifeTimeHint file_lifetime,
                                           uint32_t min_capacity) {
  std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
  // migrate_resource_.wait(lock, [this] { return !migrating_; });

  // migrating_ = true;

  // unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  // auto s =
  //     GetBestOpenZoneMatch(file_lifetime, &best_diff, out_zone, min_capacity);
    /* Avoid compiler error report. */
  if (file_lifetime) {}
  if (min_capacity) {}
  Zone *gczone = GetGCZone();
  IOStatus s;
  if (gczone->GetCapacityLeft() < min_capacity) {
     s = gczone->Finish();
     Debug(logger_, "Finish GC Zone %lu", gczone->GetZoneNr());
    if (!s.ok()) {
      printf("***[Err] GCZone %lu Finish Failed.\n", gczone->GetZoneNr());
      // migrating_ = false;
      return s;
    }
    s = gczone->CheckRelease();
    if (!s.ok()) {
      printf("***[Err] GCZone %lu CheckRelease Failed.\n", gczone->GetZoneNr());
      // migrating_ = false;
      return s;
    }
    SetGCZone(GetGCAuxZone());
    SetGCAuxZone(nullptr);
  }
  *out_zone = GetGCZone();
  if (s.ok() && (*out_zone) != nullptr) {
    Info(logger_, "TakeMigrateZone: %lu", (*out_zone)->GetZoneNr());
  } else {
    // migrating_ = false;
    Info(logger_, "GC Zone have used out\n");
  }

  return s;
}

IOStatus ZonedBlockDevice::AllocateIOZone(Env::WriteLifeTimeHint file_lifetime,
                                          IOType io_type, Zone **out_zone, uint64_t file_id) {
  Zone *allocated_zone = nullptr;

  int new_zone = 0;
  IOStatus s;

  auto tag = ZENFS_WAL_IO_ALLOC_LATENCY;
  if (io_type != IOType::kWAL) {
    // L0 flushes have lifetime MEDIUM
    if (file_lifetime == Env::WLTH_MEDIUM) {
      tag = ZENFS_L0_IO_ALLOC_LATENCY;
    } else {
      tag = ZENFS_NON_WAL_IO_ALLOC_LATENCY;
    }
  }

  ZenFSMetricsLatencyGuard guard(metrics_, tag, Env::Default());
  metrics_->ReportQPS(ZENFS_IO_ALLOC_QPS, 1);

  // Check if a deferred IO error was set
  s = GetZoneDeferredStatus();
  if (!s.ok()) {
    return s;
  }

  if (io_type != IOType::kWAL) {
    s = ApplyFinishThreshold();
    if (!s.ok()) {
      return s;
    }
    s = ResetUnusedIOZones();
    if(!s.ok()){
      return s;
    }
  }

  long allocator_open_limit = max_nr_open_io_zones_;
  if(file_lifetime < Env::WLTH_SHORT){
    if(file_id == 5){
      file_lifetime = (Env::WriteLifeTimeHint)lifetime_begin_;
    }else{
      file_lifetime = (Env::WriteLifeTimeHint)8;//highest level
    }
  }
  int level = file_lifetime - lifetime_begin_;

  
  std::unique_lock<std::mutex> lk(level_zones_mtx_);
  level_zone_resources_.wait(lk, [this, allocator_open_limit, level] {
    if (level_active_io_zones_[level].load() > 0 || open_io_zones_.load() < allocator_open_limit){
      return true;
    } else {
      return false;
    }
  });

  if(level_active_io_zones_[level].load() > 0){
    level_active_io_zones_[level]--;
    for(const auto z: level_zones[level]){
        if(!z->useinlevelzone_){
          allocated_zone = z;
          allocated_zone->useinlevelzone_ = true;
          break;
        }
    }
    Debug(logger_, "lby allocate zone %lu to file %lu", allocated_zone->GetZoneNr(), file_id);
  }else{
    open_io_zones_++;
    active_io_zones_++;
    int wait_count = 0;
    while(!allocated_zone){
      wait_count++;
      s = AllocateEmptyZone(&allocated_zone);
      if (!s.ok()) {//空间不足
          active_io_zones_--;
          open_io_zones_--;
          level_zone_resources_.notify_all();
          return s;
      }
      if(!allocated_zone){
        usleep(std::rand() %(std::min(4000 * wait_count, 1000000)));
      }
    }

    
    new_zone = 1;
    allocated_zone->lifetime_ = file_lifetime;
    level_zones[level].insert(allocated_zone);
    allocated_zone->useinlevelzone_ = true;
    Debug(logger_, "lby allocate zone %lu to lifetime %d", allocated_zone->GetZoneNr(), (int)file_lifetime);
    Debug(logger_, "lby allocate zone %lu to file %lu", allocated_zone->GetZoneNr(), file_id);
  }
  //allocated_zone->lifetime_ = file_lifetime;
  // WaitForOpenIOZoneToken(io_type == IOType::kWAL);

  /* Try to fill an already open zone(with the best life time diff) */
  // s = GetBestOpenZoneMatch(file_lifetime, &best_diff, &allocated_zone);
  //best_diff == 0, find one; best_diff = LIFETIME_DIFF_COULD_BE_WORSE, No Find
  // if (!s.ok()) {
  //   PutOpenIOZoneToken();
  //   return s;
  // }

  // Holding allocated_zone if != nullptr

  // if (best_diff >= LIFETIME_DIFF_COULD_BE_WORSE) {
  //   bool got_token = GetActiveIOZoneTokenIfAvailable();

  //   /* If we did not get a token, try to use the best match, even if the life
  //    * time diff not good but a better choice than to finish an existing zone
  //    * and open a new one
  //    */
  //   if (allocated_zone != nullptr) {
  //     if (!got_token && best_diff == LIFETIME_DIFF_COULD_BE_WORSE) {
  //       Debug(logger_,
  //             "Allocator: avoided a finish by relaxing lifetime diff "
  //             "requirement\n");
  //     } else {
  //       s = allocated_zone->CheckRelease();
  //       if (!s.ok()) {
  //         PutOpenIOZoneToken();
  //         if (got_token) PutActiveIOZoneToken();
  //         return s;
  //       }
  //       allocated_zone = nullptr;
  //     }
  //   }

  //   /* If we haven't found an open zone to fill, open a new zone */
  //   if (allocated_zone == nullptr) {
  //     /* We have to make sure we can open an empty zone */
  //     while (!got_token) {
  //       //会多个线程请求Zone导致多个Zone被finish
  //       s = FinishCheapestIOZone();
  //       if (!s.ok()) {
  //           PutOpenIOZoneToken();
  //           return s;
  //       }
  //       got_token = GetActiveIOZoneTokenIfAvailable();
  //       if(!got_token){
  //         usleep(1000);
  //       }

  //     }

  //     s = AllocateEmptyZone(&allocated_zone);
  //     if (!s.ok()) {
  //       PutActiveIOZoneToken();
  //       PutOpenIOZoneToken();
  //       return s;
  //     }

  //     if (allocated_zone != nullptr) {
  //       assert(allocated_zone->IsBusy());
  //       allocated_zone->lifetime_ = file_lifetime;
  //       new_zone = true;
  //     } else {
  //       PutActiveIOZoneToken();
  //     }
  //   }
  // }

  if (allocated_zone) {
    assert(allocated_zone->IsBusy());
    Debug(logger_,
          "Allocating zone(new=%d) nr: %lu start: 0x%lx wp: 0x%lx lt: %d file lt: %d file_id: %lu\n",
          new_zone, allocated_zone->GetZoneNr(), allocated_zone->start_, allocated_zone->wp_,
          allocated_zone->lifetime_, file_lifetime, file_id);
  }
  // } else {
  //   PutOpenIOZoneToken();
  // }

  if (io_type != IOType::kWAL) {
    LogZoneStats();
  }

  *out_zone = allocated_zone;

  metrics_->ReportGeneral(ZENFS_OPEN_ZONES_COUNT, open_io_zones_);
  metrics_->ReportGeneral(ZENFS_ACTIVE_ZONES_COUNT, active_io_zones_);

  return IOStatus::OK();
}

std::string ZonedBlockDevice::GetFilename() { return zbd_be_->GetFilename(); }

uint32_t ZonedBlockDevice::GetBlockSize() { return zbd_be_->GetBlockSize(); }

uint64_t ZonedBlockDevice::GetZoneSize() { return zbd_be_->GetZoneSize(); }

uint32_t ZonedBlockDevice::GetNrZones() { return zbd_be_->GetNrZones(); }

void ZonedBlockDevice::EncodeJsonZone(std::ostream &json_stream,
                                      const std::vector<Zone *> zones) {
  bool first_element = true;
  json_stream << "[";
  for (Zone *zone : zones) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    zone->EncodeJson(json_stream);
  }

  json_stream << "]";
}

void ZonedBlockDevice::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"meta\":";
  EncodeJsonZone(json_stream, meta_zones);
  json_stream << ",\"io\":";
  EncodeJsonZone(json_stream, io_zones);
  json_stream << "}";
}

IOStatus ZonedBlockDevice::GetZoneDeferredStatus() {
  std::lock_guard<std::mutex> lock(zone_deferred_status_mutex_);
  return zone_deferred_status_;
}

void ZonedBlockDevice::SetZoneDeferredStatus(IOStatus status) {
  std::lock_guard<std::mutex> lk(zone_deferred_status_mutex_);
  if (!zone_deferred_status_.ok()) {
    zone_deferred_status_ = status;
  }
}

void ZonedBlockDevice::GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot) {
  for (auto *zone : io_zones) {
    snapshot.emplace_back(*zone);
  }
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
