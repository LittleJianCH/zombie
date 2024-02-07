#pragma once

#include <memory>

#include "base.hpp"
#include "tock/tock.hpp"
#include "meter.hpp"
#include "config.hpp"
#include "zombie_types.hpp"
#include "heap/gd_heap.hpp"
#include "uf.hpp"

namespace ZombieInternal {

template<const ZombieConfig& cfg>
Tock tick();

template<const ZombieConfig& cfg>
using replay_func = std::function<void(const std::vector<const void*>& in)>;

template<const ZombieConfig& cfg>
struct RecordNode {
  Tock t;
  std::vector<std::shared_ptr<EZombieNode<cfg>>> ez;
  size_t space_taken = 0;

  ~RecordNode() { }
  RecordNode() : t(tick<cfg>()) { }
  explicit RecordNode(Tock t) : t(t) { }

  // a record's function can only be called via records.back()->xxx(...).
  void suspend(const std::shared_ptr<RecordNode<cfg>>& rec);
  virtual void suspended() = 0;
  virtual void resumed() = 0;
  virtual void completed() = 0;
  virtual bool is_tailcall() { return false; }

  // can only be called once, deleting this in the process
  void complete();
  void replay_finished();
  void complete_to(const std::shared_ptr<RecordNode<cfg>>& rec);
  virtual void tailcall(std::shared_ptr<replay_func<cfg>>&& f,
                        std::vector<EZombie<cfg>>&& in) { assert(false); }
  virtual void play() { assert(false); }

  virtual bool is_value() { return false; }
  virtual ExternalEZombie<cfg> get_value() { assert(false); }
};

template<const ZombieConfig& cfg>
using Record = std::shared_ptr<RecordNode<cfg>>;

template<const ZombieConfig& cfg>
struct RootRecordNode : RecordNode<cfg> {
  explicit RootRecordNode(const Tock& t) : RecordNode<cfg>(t) { }
  RootRecordNode() { }
  void suspended() override;
  void completed() override { assert(false); }
  void resumed() override;
};

template<const ZombieConfig& cfg>
struct ValueRecordNode : RecordNode<cfg> {
  ExternalEZombie<cfg> eez;

  ValueRecordNode(ExternalEZombie<cfg>&& eez) : eez(std::move(eez)) { }

  void suspended() override { assert(false); }
  void completed() override { }
  void resumed() override { assert(false); }
  bool is_value() override { return true; }
  ExternalEZombie<cfg> get_value() override { return eez; }
};

template<const ZombieConfig& cfg>
struct HeadRecordNode : RecordNode<cfg> {
  // we dont really use the Tock return type, but this allow one less boxing.
  std::shared_ptr<replay_func<cfg>> f;
  std::vector<EZombie<cfg>> inputs;
  bool played = false;

  Time start_time;

  HeadRecordNode(std::shared_ptr<replay_func<cfg>>&& f, std::vector<EZombie<cfg>>&& inputs);
  HeadRecordNode(const std::shared_ptr<replay_func<cfg>>& f, const std::vector<EZombie<cfg>>& inputs);

  void suspended() { assert(false); }
  void completed() override;
  void resumed() override { assert(false); }
  bool is_tailcall() override { return true; }
  void tailcall(std::shared_ptr<replay_func<cfg>>&& f,
                std::vector<EZombie<cfg>>&& in) override;
  void play() override;
};

template<const ZombieConfig& cfg>
struct SpineRecordNode : RecordNode<cfg> {
  Tock head_t;
  std::vector<Tock> inputs;

  Time start_time;
};

template<const ZombieConfig& cfg>
struct ContextNode : Object {
  std::vector<std::shared_ptr<EZombieNode<cfg>>> ez;
  size_t space_taken;

  explicit ContextNode(std::vector<std::shared_ptr<EZombieNode<cfg>>>&& ez, const size_t& sp) : ez(std::move(ez)), space_taken(sp) { }
  virtual void accessed() = 0;
  virtual bool evictable() = 0;
  virtual void evict() = 0;
  virtual void evict_individual(const Tock& t) = 0;
  virtual void replay() = 0;
  virtual bool is_tailcall() { return false; }
  virtual void dependency_evicted(UF<Time>& t) = 0;
  virtual UF<Time> get_evicted_dependencies() = 0;
};

template<const ZombieConfig& cfg>
using Context = std::shared_ptr<ContextNode<cfg>>;

template<const ZombieConfig& cfg>
struct RootContextNode : ContextNode<cfg> {
  explicit RootContextNode(std::vector<std::shared_ptr<EZombieNode<cfg>>>&& ez, const size_t& sp) : ContextNode<cfg>(std::move(ez), sp) { }
  void accessed() override { }
  bool evictable() override { return false; }
  void evict() override { assert(false); }
  void evict_individual(const Tock& t) override { assert(false); }
  void replay() override { assert(false); }
  void dependency_evicted(UF<Time>& t) override { }
  UF<Time> get_evicted_dependencies() override { return UF<Time>(Time(ns(0))); }
};

template<const ZombieConfig& cfg>
struct FullContextNode : ContextNode<cfg> {
  std::shared_ptr<replay_func<cfg>> f, next_f;
  std::vector<EZombie<cfg>> inputs, next_inputs;
  Tock start_t;
  Tock end_t; // open-close

  Time time_taken;
  mutable Time last_accessed;

  mutable ptrdiff_t pool_index = -1;

  // if X is evicted, and recomputing depend on this node,
  // X should merge the UF representation of itself with this UF.
  // note that as FullContextNode can be evicted, when that happens
  // the UF rep have to be merged with the FullContextNode's evicted_dependencies before it.
  UF<Time> evicted_dependencies = UF<Time>(time_taken);

  explicit FullContextNode(std::vector<std::shared_ptr<EZombieNode<cfg>>>&& ez,
                           const size_t& sp,
                           const Time& time_taken,
                           std::shared_ptr<replay_func<cfg>>&& f,
                           std::vector<EZombie<cfg>>&& inputs,
                           const Tock& t);
  ~FullContextNode();

  void accessed() override;
  bool evictable() override { return true; }
  void evict() override;
  void evict_individual(const Tock& t) override;
  void replay() override;
  cost_t cost();
  bool is_tailcall() override { return true; }
  void dependency_evicted(UF<Time>& t) override { evicted_dependencies.merge(t); }
  UF<Time> get_evicted_dependencies() override { return evicted_dependencies; }
};

// RecomputeLater holds a weak pointer to a MicroWave,
// and is stored in Trailokya::book for eviction.
template<const ZombieConfig& cfg>
struct RecomputeLater : Phantom {
  std::weak_ptr<FullContextNode<cfg>> weak_ptr;

  RecomputeLater(const std::shared_ptr<FullContextNode<cfg>>& ptr) : weak_ptr(ptr) { }
  cost_t cost() const override;
  void evict() override;
  void notify_index_changed(size_t idx) override {
    if (auto ptr = weak_ptr.lock()) {
      ptr->pool_index = idx;
    }
  }
};

template<const ZombieConfig& cfg>
struct SpineContextNode : ContextNode<cfg> { };

template<const ZombieConfig& cfg>
struct Replay {
  Tock forward_at = std::numeric_limits<Tock>::max();
  std::shared_ptr<EZombieNode<cfg>>* forward_to = nullptr;
};

template<const ZombieConfig& cfg>
struct Trailokya {
public:
  struct NotifyIndexChanged {
    void operator()(const std::unique_ptr<Phantom>& p, size_t idx) {
      p->notify_index_changed(idx);
    };
  };

  struct NotifyElementRemoved {
    void operator()(const std::unique_ptr<Phantom>& p) { }
  };

  struct Reaper;
public:
  Tock current_tock = 1;
  SplayList<Tock, Context<cfg>> akasha;
  GDHeap<cfg, std::unique_ptr<Phantom>, NotifyIndexChanged, NotifyElementRemoved> book;
  std::vector<Record<cfg>> records = {std::make_shared<RootRecordNode<cfg>>(Tock(0))};
  Replay<cfg> replay;
  ZombieMeter meter;
  Reaper reaper = Reaper(*this);
  std::function<void()> each_step = [](){};

public:
  Trailokya() { }
  ~Trailokya() { }

  static Trailokya& get_trailokya() {
    static Trailokya t;
    return t;
  }

public:
  struct Reaper {
    Trailokya& t;

    Reaper(Trailokya& t) : t(t) { }

    bool have_soul() {
      return t.book.empty();
    }

    void murder() {
      assert (t.book.size() > 0);
      t.book.adjust_pop([](const std::unique_ptr<Phantom>& p) { return p->cost(); })->evict();
    }

    uint64_t score() {
      return t.book.score();
    }
  };
};

} // end of namespace ZombieInternal
