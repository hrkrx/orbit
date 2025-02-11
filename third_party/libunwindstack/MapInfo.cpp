/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <string>

#include <android-base/stringprintf.h>

#include <unwindstack/Elf.h>
#include <unwindstack/MapInfo.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Object.h>

#include "MemoryFileAtOffset.h"
#include "MemoryRange.h"

namespace unwindstack {

bool MapInfo::InitFileMemoryFromPreviousReadOnlyMap(MemoryFileAtOffset* memory) {
  // One last attempt, see if the previous map is read-only with the
  // same name and stretches across this map.
  if (prev_real_map() == nullptr || prev_real_map()->flags() != PROT_READ) {
    return false;
  }

  uint64_t map_size = end() - prev_real_map()->end();
  if (!memory->Init(name(), prev_real_map()->offset(), map_size)) {
    return false;
  }

  uint64_t max_size;
  if (!Elf::GetInfo(memory, &max_size) || max_size < map_size) {
    return false;
  }

  if (!memory->Init(name(), prev_real_map()->offset(), max_size)) {
    return false;
  }

  set_object_offset(offset() - prev_real_map()->offset());
  set_object_start_offset(prev_real_map()->offset());
  return true;
}

Memory* MapInfo::GetFileMemory() {
  std::unique_ptr<MemoryFileAtOffset> memory(new MemoryFileAtOffset);
  if (offset() == 0) {
    if (memory->Init(name(), 0)) {
      return memory.release();
    }
    return nullptr;
  }

  // These are the possibilities when the offset is non-zero.
  // - There is an elf file embedded in a file, and the offset is the
  //   the start of the elf in the file.
  // - There is an elf file embedded in a file, and the offset is the
  //   the start of the executable part of the file. The actual start
  //   of the elf is in the read-only segment preceeding this map.
  // - The whole file is an elf file, and the offset needs to be saved.
  //
  // Map in just the part of the file for the map. If this is not
  // a valid elf, then reinit as if the whole file is an elf file.
  // If the offset is a valid elf, then determine the size of the map
  // and reinit to that size. This is needed because the dynamic linker
  // only maps in a portion of the original elf, and never the symbol
  // file data.
  uint64_t map_size = end() - start();
  if (!memory->Init(name(), offset(), map_size)) {
    return nullptr;
  }

  // Check if the start of this map is an embedded elf.
  uint64_t max_size = 0;
  if (Elf::GetInfo(memory.get(), &max_size)) {
    set_object_start_offset(offset());
    if (max_size > map_size) {
      if (memory->Init(name(), offset(), max_size)) {
        return memory.release();
      }
      // Try to reinit using the default map_size.
      if (memory->Init(name(), offset(), map_size)) {
        return memory.release();
      }
      set_object_start_offset(0);
      return nullptr;
    }
    return memory.release();
  }

  // No elf at offset, try to init as if the whole file is an elf.
  if (memory->Init(name(), 0) && Elf::IsValidElf(memory.get())) {
    set_object_offset(offset());
    // Need to check how to set the elf start offset. If this map is not
    // the r-x map of a r-- map, then use the real offset value. Otherwise,
    // use 0.
    if (prev_real_map() == nullptr || prev_real_map()->offset() != 0 ||
        prev_real_map()->flags() != PROT_READ || prev_real_map()->name() != name()) {
      set_object_start_offset(offset());
    }
    return memory.release();
  }

  // See if the map previous to this one contains a read-only map
  // that represents the real start of the elf data.
  if (InitFileMemoryFromPreviousReadOnlyMap(memory.get())) {
    return memory.release();
  }

  // Failed to find elf at start of file or at read-only map, return
  // file object from the current map.
  if (memory->Init(name(), offset(), map_size)) {
    return memory.release();
  }
  return nullptr;
}

Memory* MapInfo::CreateMemory(const std::shared_ptr<Memory>& process_memory) {
  if (end() <= start()) {
    return nullptr;
  }

  set_object_offset(0);

  // Fail on device maps.
  if (flags() & MAPS_FLAGS_DEVICE_MAP) {
    return nullptr;
  }

  // First try and use the file associated with the info.
  if (!name().empty()) {
    Memory* memory = GetFileMemory();
    if (memory != nullptr) {
      return memory;
    }
  }

  if (process_memory == nullptr) {
    return nullptr;
  }

  set_memory_backed_object(true);

  // Need to verify that this elf is valid. It's possible that
  // only part of the elf file to be mapped into memory is in the executable
  // map. In this case, there will be another read-only map that includes the
  // first part of the elf file. This is done if the linker rosegment
  // option is used.
  std::unique_ptr<MemoryRange> memory(new MemoryRange(process_memory, start(), end() - start(), 0));
  if (Elf::IsValidElf(memory.get())) {
    set_object_start_offset(offset());

    // Might need to peek at the next map to create a memory object that
    // includes that map too.
    if (offset() != 0 || name().empty() || next_real_map() == nullptr ||
        offset() >= next_real_map()->offset() || next_real_map()->name() != name()) {
      return memory.release();
    }

    // There is a possibility that the elf object has already been created
    // in the next map. Since this should be a very uncommon path, just
    // redo the work. If this happens, the elf for this map will eventually
    // be discarded.
    MemoryRanges* ranges = new MemoryRanges;
    ranges->Insert(new MemoryRange(process_memory, start(), end() - start(), 0));
    ranges->Insert(new MemoryRange(process_memory, next_real_map()->start(),
                                   next_real_map()->end() - next_real_map()->start(),
                                   next_real_map()->offset() - offset()));

    return ranges;
  }

  // Find the read-only map by looking at the previous map. The linker
  // doesn't guarantee that this invariant will always be true. However,
  // if that changes, there is likely something else that will change and
  // break something.
  if (offset() == 0 || name().empty() || prev_real_map() == nullptr ||
      prev_real_map()->name() != name() || prev_real_map()->offset() >= offset()) {
    set_memory_backed_object(false);
    return nullptr;
  }

  // Make sure that relative pc values are corrected properly.
  set_object_offset(offset() - prev_real_map()->offset());
  // Use this as the elf start offset, otherwise, you always get offsets into
  // the r-x section, which is not quite the right information.
  set_object_start_offset(prev_real_map()->offset());

  MemoryRanges* ranges = new MemoryRanges;
  ranges->Insert(new MemoryRange(process_memory, prev_real_map()->start(),
                                 prev_real_map()->end() - prev_real_map()->start(), 0));
  ranges->Insert(new MemoryRange(process_memory, start(), end() - start(), object_offset()));

  return ranges;
}

Object* MapInfo::GetObject(const std::shared_ptr<Memory>& process_memory, ArchEnum expected_arch) {
  {
    // Make sure no other thread is trying to add the object to this map.
    std::lock_guard<std::mutex> guard(object_mutex());

    if (object().get() != nullptr) {
      return object().get();
    }

    bool locked = false;
    if (Object::CachingEnabled() && !name().empty()) {
      Object::CacheLock();
      locked = true;
      if (Object::CacheGet(this)) {
        Object::CacheUnlock();
        return object().get();
      }
    }

    Memory* memory = CreateMemory(process_memory);
    if (locked) {
      if (Object::CacheAfterCreateMemory(this)) {
        delete memory;
        Object::CacheUnlock();
        return object().get();
      }
    }
    // TODO: Distinguish between the type of object file here (Coff vs. Elf).
    object().reset(new Elf(memory));

    // If the init fails, keep the object around as an invalid object so we
    // don't try to reinit the object.
    object()->Init();
    if (object()->valid() && expected_arch != object()->arch()) {
      // Make the object invalid, mismatch between arch and expected arch.
      object()->Invalidate();
    }

    if (locked) {
      Object::CacheAdd(this);
      Object::CacheUnlock();
    }
  }

  if (!object()->valid()) {
    set_object_start_offset(offset());
  } else if (prev_real_map() != nullptr && object_start_offset() != offset() &&
             prev_real_map()->offset() == object_start_offset() &&
             prev_real_map()->name() == name()) {
    // If there is a read-only map then a read-execute map that represents the
    // same object, make sure the previous map is using the same elf
    // object if it hasn't already been set.
    std::lock_guard<std::mutex> guard(prev_real_map()->object_mutex());
    if (prev_real_map()->object().get() == nullptr) {
      prev_real_map()->set_object(object());
      prev_real_map()->set_memory_backed_object(memory_backed_object());
    } else {
      // Discard this object, and use the object from the previous map instead.
      set_object(prev_real_map()->object());
    }
  }
  return object().get();
}

bool MapInfo::GetFunctionName(uint64_t addr, SharedString* name, uint64_t* func_offset) {
  {
    // Make sure no other thread is trying to update this elf object.
    std::lock_guard<std::mutex> guard(object_mutex());
    if (object() == nullptr) {
      return false;
    }
  }
  // No longer need the lock, once the elf object is created, it is not deleted
  // until this object is deleted.
  return object()->GetFunctionName(addr, name, func_offset);
}

uint64_t MapInfo::GetLoadBias(const std::shared_ptr<Memory>& process_memory) {
  int64_t cur_load_bias = load_bias().load();
  if (cur_load_bias != INT64_MAX) {
    return cur_load_bias;
  }

  {
    // Make sure no other thread is trying to add the elf to this map.
    std::lock_guard<std::mutex> guard(object_mutex());
    if (object() != nullptr) {
      if (object()->valid()) {
        cur_load_bias = object()->GetLoadBias();
        set_load_bias(cur_load_bias);
        return cur_load_bias;
      } else {
        set_load_bias(0);
        return 0;
      }
    }
  }

  if (IsElf()) {
    // Call lightweight static function that will only read enough of the
    // object data to get the load bias.
    std::unique_ptr<Memory> memory(CreateMemory(process_memory));
    cur_load_bias = Elf::GetLoadBias(memory.get());
    set_load_bias(cur_load_bias);
    return cur_load_bias;
  } else {
    // TODO: Handle other object file types.
    return cur_load_bias;
  }
}

MapInfo::~MapInfo() {
  ObjectFields* object_fields = object_fields_.load();
  if (object_fields != nullptr) {
    delete object_fields->build_id_.load();
    delete object_fields;
  }
}

SharedString MapInfo::GetBuildID() {
  SharedString* id = build_id().load();
  if (id != nullptr) {
    return *id;
  }

  // No need to lock, at worst if multiple threads do this at the same
  // time it should be detected and only one thread should win and
  // save the data.

  // Now need to see if the elf object exists.
  // Make sure no other thread is trying to add the object to this map.
  object_mutex().lock();
  Object* obj = object().get();
  object_mutex().unlock();
  std::string result;
  if (obj != nullptr) {
    result = obj->GetBuildID();
  } else {
    // This will only work if we can get the file associated with this memory.
    // If this is only available in memory, then the section name information
    // is not present and we will not be able to find the build id info.
    std::unique_ptr<Memory> memory(GetFileMemory());
    if (memory != nullptr) {
      if (IsElf()) {
        result = Elf::GetBuildID(memory.get());
      } else {
        // TODO: Handle other object file types.
        result = "";
      }
    }
  }
  return SetBuildID(std::move(result));
}

SharedString MapInfo::SetBuildID(std::string&& new_build_id) {
  std::unique_ptr<SharedString> new_build_id_ptr(new SharedString(std::move(new_build_id)));
  SharedString* expected_id = nullptr;
  // Strong version since we need to reliably return the stored pointer.
  if (build_id().compare_exchange_strong(expected_id, new_build_id_ptr.get())) {
    // Value saved, so make sure the memory is not freed.
    return *new_build_id_ptr.release();
  } else {
    // The expected value is set to the stored value on failure.
    return *expected_id;
  }
}

MapInfo::ObjectFields& MapInfo::GetObjectFields() {
  ObjectFields* object_fields = object_fields_.load(std::memory_order_acquire);
  if (object_fields != nullptr) {
    return *object_fields;
  }
  // Allocate and initialize the field in thread-safe way.
  std::unique_ptr<ObjectFields> desired(new ObjectFields());
  ObjectFields* expected = nullptr;
  // Strong version is reliable. Weak version might randomly return false.
  if (object_fields_.compare_exchange_strong(expected, desired.get())) {
    return *desired.release();  // Success: we transferred the pointer ownership to the field.
  } else {
    return *expected;  // Failure: 'expected' is updated to the value set by the other thread.
  }
}

std::string MapInfo::GetPrintableBuildID() {
  std::string raw_build_id = GetBuildID();
  if (raw_build_id.empty()) {
    return "";
  }
  std::string printable_build_id;
  for (const char& c : raw_build_id) {
    // Use %hhx to avoid sign extension on abis that have signed chars.
    printable_build_id += android::base::StringPrintf("%02hhx", c);
  }
  return printable_build_id;
}

}  // namespace unwindstack
