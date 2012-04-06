// Copyright 2010 Google Inc. All Rights Reserved.

// Unit tests for countdown_latch
#include <test_mutex.h>
#include "countdown_latch.h"

#include "gmock/gmock.h"
#include <tr1/functional>

#include <thread.h>

using namespace std;
using testing::_;

class CountdownLatchTest : public testing::Test {
};

void WaitForLatch(countdown_latch& latch) {
  std::cerr << "WaitForLatch " << this_thread::get_id() << "\n";
  latch.wait();
  std::cerr << "WaitForLatch waited " << this_thread::get_id() << "\n";
  EXPECT_EQ(latch.get_count(), 0);
}

void WaitForLatchAndDecrement(countdown_latch& to_wait,
                              countdown_latch& decrement) {
  to_wait.wait();
  decrement.count_down();
  EXPECT_EQ(to_wait.get_count(), 0);
  EXPECT_EQ(decrement.get_count(), 0);
}

void DecrementAndWaitForLatch(countdown_latch& decrement,
                              countdown_latch& to_wait) {
  decrement.count_down();
  to_wait.wait();
  EXPECT_EQ(to_wait.get_count(), 0);
  EXPECT_EQ(decrement.get_count(), 0);
}

// Tests two threads waiting on a single countdown_latch
TEST_F(CountdownLatchTest, TwoThreads) {
  countdown_latch latch(2);
  thread t1(tr1::bind(WaitForLatch, tr1::ref(latch)));
  thread t2(tr1::bind(WaitForLatch, tr1::ref(latch)));
  std::cerr << "Counting down " << this_thread::get_id() << "\n";
  latch.count_down();
  std::cerr << "Counting down " << this_thread::get_id() << "\n";
  latch.count_down();
  t1.join();
  t2.join();
}

// Tests two threads waiting on a countdown_latch that has already
// been decremented.
TEST_F(CountdownLatchTest, TwoThreadsPreDecremented) {
  countdown_latch latch(2);
  latch.count_down();
  latch.count_down();
  thread t1(tr1::bind(WaitForLatch, tr1::ref(latch)));
  thread t2(tr1::bind(WaitForLatch, tr1::ref(latch)));
  t1.join();
  t2.join();
}

// Tests two threads waiting and decrementing two latches
TEST_F(CountdownLatchTest, TwoThreadsTwoLatches) {
  countdown_latch first(1);
  countdown_latch second(1);
  thread t1(tr1::bind(
      WaitForLatchAndDecrement, tr1::ref(first), tr1::ref(second)));
  thread t2(tr1::bind(
      DecrementAndWaitForLatch, tr1::ref(first), tr1::ref(second)));
  t1.join();
  t2.join();
}