#pragma once

#include <memory>

#include "base.hpp"
#include "tock/tock.hpp"
#include "meter.hpp"
#include "config.hpp"
#include "zombie_types.hpp"
#include "heap/gd_heap.hpp"

namespace ZombieInternal {

template<typename T>
struct UF {
  std::shared_ptr<UF> parent;
  T t; // only meaningful when parent.get() == nullptr
};

template<const ZombieConfig& cfg>
Tock tick();

template<const ZombieConfig& cfg>
struct RecordNode {
  Tock t;
  std::vector<std::shared_ptr<EZombieNode<cfg>>> ez;
  size_t space_taken = 0;

  ~RecordNode() { }
  RecordNode() : t(tick<cfg>()) { }
  explicit RecordNode(Tock t) : t(t) { }

  void suspend(const std::shared_ptr<RecordNode<cfg>>& rec);
  void complete();
  void replay_finished();
  virtual void suspended() = 0;
  virtual void completed() = 0;
  virtual void resumed() = 0;
  virtual bool is_tailcall() { return false; }
  virtual Trampoline::Output<EZombie<cfg>> tailcall(std::function<Trampoline::Output<EZombie<cfg>>(const std::vector<const void*>& in)>&& f,
                                                    std::vector<EZombie<cfg>>&& in) = 0;
  virtual Trampoline::Output<EZombie<cfg>> play() = 0;
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
  Trampoline::Output<EZombie<cfg>> tailcall(std::function<Trampoline::Output<EZombie<cfg>>(const std::vector<const void*>& in)>&& f,
                                            std::vector<EZombie<cfg>>&& in) override { assert(false); }
  Trampoline::Output<EZombie<cfg>> play() override { assert(false); }
};

template<const ZombieConfig& cfg>
struct HeadRecordNode : RecordNode<cfg> {
  // we dont really use the Tock return type, but this allow one less boxing.
  std::function<Trampoline::Output<EZombie<cfg>>(const std::vector<const void*>& in)> f;
  std::vector<EZombie<cfg>> inputs;

  Time start_time;

  HeadRecordNode(std::function<Trampoline::Output<EZombie<cfg>>(const std::vector<const void*>& in)>&& f, std::vector<EZombie<cfg>>&& inputs);

  void suspended() { assert(false); }
  void completed() override;
  void resumed() override { assert(false); }
  bool is_tailcall() override { return true; }
  Trampoline::Output<EZombie<cfg>> tailcall(std::function<Trampoline::Output<EZombie<cfg>>(const std::vector<const void*>& in)>&& f,
                                            std::vector<EZombie<cfg>>&& in) override;
  Trampoline::Output<EZombie<cfg>> play() override;
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
};

template<const ZombieConfig& cfg>
struct FullContextNode : ContextNode<cfg> {
  std::function<Trampoline::Output<EZombie<cfg>>(const std::vector<const void*>& in)> f;
  std::vector<EZombie<cfg>> inputs;
  Tock t;

  Time time_taken;
  mutable Time last_accessed;

  mutable ptrdiff_t pool_index = -1;

  explicit FullContextNode(std::vector<std::shared_ptr<EZombieNode<cfg>>>&& ez,
                           const size_t& sp,
                           const Time& time_taken,
                           std::function<Trampoline::Output<EZombie<cfg>>(const std::vector<const void*>& in)>&& f,
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
  std::function<void()> each_tc = [](){};

public:
  Trailokya() { }
  ~Trailokya() { }

  static Trailokya& get_trailokya() {
    static Trailokya t;
    return t;
  }

  // return the closest MicroWave holding [t]
  std::shared_ptr<MicroWave<cfg>> get_microwave(const Tock& t) {/*
    TockTreeElem elem = akasha.get_node(t).value;
    if (elem.index() == TockTreeElemKind::MicroWave) {
      return std::get<TockTreeElemKind::MicroWave>(elem);
    } else {
      auto parent = akasha.get_parent(t);
      if (! parent || parent->value.index() == TockTreeElemKind::Nothing) {
        return nullptr;
      } else {
        assert (parent->value.index() == TockTreeElemKind::MicroWave);
        return std::get<TockTreeElemKind::MicroWave>(parent->value);
      }
      }*/
    assert(false);
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
