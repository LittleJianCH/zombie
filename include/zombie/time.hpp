#pragma once

#include <chrono>
#include <utility>
#include <vector>
#include <functional>
#include <iostream>
#include <cassert>

#include "common.hpp"

using ns = std::chrono::nanoseconds;

// A slight wrapper above standard clock,
//   providing fast forwarding ability.
// Useful for testing, and for including additional time uncaptured in bindZombie,
//   e.g. Zombie's use in gegl.
// TODO: the right way is to add configurability to Zombie, not via such an adhoc patch.
struct ZombieRawClock {
  using time_t = decltype(std::chrono::steady_clock::now());

  time_t begin_time = std::chrono::steady_clock::now();
  // Note that the clock will overflow after only 585 years.

  ns forwarded = ns(0);

  ns time() const {
    return ns(std::chrono::steady_clock::now() - begin_time) + forwarded;
  }

  void fast_forward(ns n) {
    forwarded += n;
  }

  static ZombieRawClock& singleton() {
    static ZombieRawClock zc;
    return zc;
  }
};

// In zombie, we want the time or bindZombie to not include that of recursive bindZombie.
// This class take care of that.
struct ZombieClock {
  struct Node {
    ns constructed_time = ZombieRawClock::singleton().time();
    ns skipping_time = ns(0);
  };

  std::vector<Node> stack;

  void fast_forward(ns n) {
    ZombieRawClock::singleton().fast_forward(n);
  }

  template<typename F>
  std::pair<decltype(std::declval<F>()()), ns> timed(const F& f) {
    ns taken_time;
    auto t = bracket([&](){ stack.push_back(Node()); },
                     f,
                     [&](){
                       assert(!stack.empty());
                       ns current_time = ZombieRawClock::singleton().time();
                       ns skipping_time = stack.back().skipping_time;
                       ns minus_time = stack.back().constructed_time + skipping_time;
                       assert(current_time >= minus_time);
                       taken_time = current_time - minus_time;
                       stack.pop_back();
                       if (!stack.empty()) {
                         stack.back().skipping_time += taken_time + skipping_time;
                       }
                     });
    return {t, taken_time};
  }
};
