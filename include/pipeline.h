// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GCL_PIPELINE_
#define GCL_PIPELINE_

#include <iostream>
#include <vector>

#include <exception>
#include <stdexcept>
#if defined(__GXX_EXPERIMENTAL_CXX0X__)
#include <functional>
#else
#include <tr1/functional>
#endif

#include <atomic.h>
#include <barrier.h>
#include <buffer_queue.h>
#include <latch.h>
#include <queue_base.h>
#include <simple_thread_pool.h>

namespace gcl {

#if defined(__GXX_EXPERIMENTAL_CXX0X__)
using std::function;
using std::bind;
using std::placeholders::_1;
#else
using std::tr1::function;
using std::tr1::bind;
using std::tr1::placeholders::_1;
#endif

using std::vector;

// BEGIN UTILITIES
//TODO(aberkan): Common naming scheme
//TODO(aberkan): Better commenting
//TODO(aberkan): Move some of this out of this file
//TODO(aberkan): Simplify filter and segment stuff
//TODO(aberkan): Move excecution strategy to template parameter
//TODO(aberkan): Reduce Cloning

class PipelineTerm { };

template <typename T>
PipelineTerm ignore(T t) { return PipelineTerm(); };

template<typename T>
PipelineTerm do_consume(function<void (T)> f, T t) {
  f(t);
  return PipelineTerm();
}

template <typename IN_TYPE,
          typename INTERMEDIATE,
          typename OUT_TYPE>
static OUT_TYPE chain(function<INTERMEDIATE (IN_TYPE in)> intermediate_fn,
                      function<OUT_TYPE (INTERMEDIATE in)> out_fn,
                      IN_TYPE in) {
  return out_fn(intermediate_fn(in));
}


template <typename IN,
          typename OUT>
class filter {
 public:
  virtual ~filter() {};
  virtual OUT Apply(IN in) = 0;
  virtual bool Run(function<PipelineTerm (OUT)> r) = 0;
  virtual bool Run() = 0;
  virtual void Close() = 0;
  virtual filter<IN, OUT>* clone() const = 0;
};

template <typename IN, typename OUT>
class FullPipelinePlan;

typedef FullPipelinePlan<PipelineTerm, PipelineTerm> PipelinePlan;

class PipelineExecution {
 public:
  PipelineExecution(const PipelinePlan& pp, simple_thread_pool* pool);

  ~PipelineExecution();

  bool is_done() {
    return done_;
  }
  void wait() {
    end_.wait();
  }
  void cancel();

  void execute(filter<PipelineTerm, PipelineTerm> *f);

  void threads_done() {
    done_ = true;
    end_.count_down();
  }

  PipelinePlan* pp_;
  simple_thread_pool* pool_;
  latch start_;
  barrier* thread_end_;
  latch end_;
  int num_threads_;
  bool done_;
};


template <typename IN,
          typename MID,
          typename OUT>
class filter_chain : public filter<IN, OUT> {
 public:
  filter_chain(class filter<IN, MID> *first,
               class filter<MID, OUT> *second)
      : first_(first),
        second_(second) {assert(first && second);};
  virtual ~filter_chain() {
    delete first_;
    delete second_;
  };
  virtual OUT Apply(IN in) {
    return second_->Apply(first_->Apply(in));
  };
  virtual filter<IN, OUT>* clone() const {
    return new filter_chain<IN, MID, OUT>(first_->clone(), second_->clone());
  };
  //TODO(aberkan): Make calling run on non-runnable a compile time error.
  virtual bool Run(function<PipelineTerm (OUT)> r);
  virtual bool Run();
  virtual void Close() {
    first_->Close();
    second_->Close();
  }
  class filter<IN, MID>* first_;
  class filter<MID, OUT>* second_;
};

template <typename IN,
          typename MID,
          typename OUT>
bool filter_chain<IN, MID, OUT>::Run(function<PipelineTerm (OUT)> r) {
  function<OUT (MID)> m = bind(&filter<MID, OUT>::Apply, second_, _1);
  function<PipelineTerm (MID)> p
    = bind(chain<MID, OUT, PipelineTerm>, m, r, _1);
  return first_->Run(p);
};

template <typename IN,
          typename MID,
          typename OUT>
bool filter_chain<IN, MID, OUT>::Run() {
  function<OUT (MID)> m = bind(&filter<MID, OUT>::Apply, second_, _1);
  function<PipelineTerm (OUT)> r = ignore<OUT>;
  function<PipelineTerm (MID)> p =
    bind(chain<MID, OUT, PipelineTerm>, m, r, _1);
  return first_->Run(p);
};

void Nothing() {};

template <typename IN,
          typename OUT>
class filter_function : public filter<IN, OUT> {
 public:
  filter_function(function<OUT (IN)> f)
      : f_(f), close_(Nothing) { };
  filter_function(function<OUT (IN)> f, function<void ()> close)
      : f_(f), close_(close) { };
  virtual ~filter_function() { };
  virtual OUT Apply(IN in) {
    return f_(in);
  }
  virtual bool Run(function<PipelineTerm (OUT)> r) {
    throw;
  }
  virtual bool Run() {
    throw;
  }
  virtual void Close() {
    close_();
  }
  virtual class filter<IN, OUT>* clone() const {
    return new filter_function<IN, OUT>(f_, close_);
  }
  function<OUT (IN)> f_;
  function<void ()> close_;
};

template <typename OUT>
class filter_thread_point : public filter<PipelineTerm, OUT> {
 public:
  filter_thread_point(queue_back<OUT> *qb) : qb_(qb) { };
  ~filter_thread_point() { };
  virtual OUT Apply(PipelineTerm IN) {
    throw;
  }
  virtual bool Run(function<PipelineTerm (OUT)> r) {
    OUT out;
    // TODO(aberkan): What if this throws?
    queue_op_status status = qb_->wait_pop(out);
    if (status != CXX0X_ENUM_QUAL(queue_op_status)success) {
      return false;
    }
    r(out);
    return true;
  }
  virtual bool Run() {
    throw;
  }

  virtual void Close() { };

  virtual filter<PipelineTerm, OUT>* clone() const {
    return new filter_thread_point<OUT>(qb_);
  }
  queue_back<OUT> *qb_;
};

template <typename OUT>
class pipeline_segment {
 public:
  pipeline_segment(filter<PipelineTerm, OUT> *f,
                   pipeline_segment<PipelineTerm> *next)
      : f_(f), next_(next) { };
  virtual ~pipeline_segment() {
    delete f_;
    delete next_;
  }
  void Run(PipelineExecution* pex);
  pipeline_segment<OUT> *clone() const {
    return new pipeline_segment<OUT>(f_->clone(),
                                     next_ ? next_->clone() : NULL);
  }
  pipeline_segment<OUT>* chain(pipeline_segment<PipelineTerm>* p) {
    assert(!next_);
    next_ = p;
    return this;
  }

  filter<PipelineTerm, OUT> *f_;
  pipeline_segment<PipelineTerm> *next_;

};

template <typename OUT>
void pipeline_segment<OUT>::Run(PipelineExecution* pex) {
  throw;
}

template <>
void pipeline_segment<PipelineTerm>::Run(PipelineExecution* pex) {
  pex->execute(f_);
  if(next_) {
    next_->Run(pex);
  }
}

template<typename MID,
         typename OUT>
pipeline_segment<OUT>* chain(const pipeline_segment<MID>* p,
                             const filter<MID, OUT>* f) {
  pipeline_segment<OUT> *ps = new pipeline_segment<OUT>(
      new filter_chain<PipelineTerm, MID, OUT>(p->f_->clone(),
                                               f->clone()),
      NULL);
  return ps;
}

// END UTILITIES

// BEGIN CLASSES

template<typename IN,
         typename OUT>
class FullPipelinePlan {
 public:
  FullPipelinePlan(filter<IN, PipelineTerm> *leading,
                   pipeline_segment<PipelineTerm> *chain,
                   pipeline_segment<OUT> *trailing)
      : leading_(leading),
        chain_(chain),
        trailing_(trailing) {
  };

  ~FullPipelinePlan() {
    delete leading_;
    delete chain_;
    delete trailing_;
  };
  void run(PipelineExecution* pex);
  filter<IN, PipelineTerm> *leading_clone() const {
    return leading_ ? leading_->clone() : NULL;
  }
  pipeline_segment<PipelineTerm> *chain_clone() const {
    return chain_ ? chain_->clone() : NULL;
  }
  pipeline_segment<OUT> *trailing_clone() const {
    return trailing_ ? trailing_->clone() : NULL;
  }

  FullPipelinePlan<IN, OUT>* clone() const {
    return new FullPipelinePlan<IN, OUT>(leading_clone(),
                                         chain_clone(),
                                         trailing_clone());
  }

  filter<IN, PipelineTerm> *leading_;
  pipeline_segment<PipelineTerm> *chain_;
  pipeline_segment<OUT> *trailing_;
};

template<typename IN,
         typename OUT>
class SimplePipelinePlan {
 public:
  SimplePipelinePlan(function<OUT (IN in)> f)
      : f_(new filter_function<IN, OUT>(f)) { };
  SimplePipelinePlan(function<OUT (IN in)> f,
                     function<void ()> close)
      : f_(new filter_function<IN, OUT>(f, close)) { };
  SimplePipelinePlan(filter<IN, OUT> *f)
      : f_(f) { };

  OUT Apply(IN in) {
    return f_->Apply(in);
  }

  filter<IN, OUT> *f_;
};

// END CLASSES

// BEGIN CONSTRUCTORS

template<typename OUT>
FullPipelinePlan<PipelineTerm, OUT> Source(queue_back<OUT> *b) {
  pipeline_segment<OUT>* p =
      new pipeline_segment<OUT>(new filter_thread_point<OUT>(b), NULL);
  return FullPipelinePlan<PipelineTerm, OUT>(NULL, NULL, p);
}

template<typename IN,
         typename OUT>
SimplePipelinePlan<IN, OUT> Filter(OUT f(IN)) {
  return SimplePipelinePlan<IN, OUT>(f);
}

template<typename IN,
         typename OUT>
SimplePipelinePlan<IN, OUT> Filter(function<OUT (IN)> f) {
  return SimplePipelinePlan<IN, OUT>(f);
}

template<typename IN>
SimplePipelinePlan<IN, PipelineTerm> Consume(void consumer(IN)) {
  return function<PipelineTerm (IN)>(bind(do_consume<IN>, consumer, _1));
}

template<typename IN>
SimplePipelinePlan<IN, PipelineTerm> Consume(function<void (IN)> consumer,
                                             function<void ()> close) {
  return SimplePipelinePlan<IN, PipelineTerm>(
      function<PipelineTerm (IN)>(bind(do_consume<IN>, consumer, _1)),
      close);
}

template<typename IN>
SimplePipelinePlan<IN, PipelineTerm> Consume(function<void (IN)> consumer) {
  return function<PipelineTerm (IN)>(bind(do_consume<IN>, consumer, _1));
}

template<typename IN>
SimplePipelinePlan<IN, PipelineTerm> Sink(queue_front<IN> *front) {
  function<void (IN)> f1 = bind(&queue_front<IN>::push, front, _1);
  return Consume(f1);
}

template<typename IN>
SimplePipelinePlan<IN, PipelineTerm> SinkAndClose(queue_front<IN> *front) {
  function<void (IN)> f1 = bind(&queue_front<IN>::push, front, _1);
  function<void ()> f2 = bind(&queue_common<IN>::close, front);
  return Consume(f1, f2);
}

template<typename IN,
         typename OUT>
FullPipelinePlan<IN, OUT> Parallel(SimplePipelinePlan<IN, OUT> p) {
  // TODO: ref counting queue, no limit
  buffer_queue<IN> *q = new buffer_queue<IN>(10);
  return SinkAndClose(q) | Source(q) | p;
}

// END CONSTRUCTORS

// BEGIN PIPES

template<typename IN,
         typename MID,
         typename OUT>
SimplePipelinePlan<IN, OUT> operator|(const SimplePipelinePlan<IN, MID>& p1,
                                      const SimplePipelinePlan<MID, OUT>& p2) {
  return SimplePipelinePlan<IN, OUT>(
      new filter_chain<IN, MID, OUT>(p1.f_->clone(),
                                     p2.f_->clone()));
}

template<typename IN,
         typename MID,
         typename OUT>
FullPipelinePlan<IN, OUT> operator|(const FullPipelinePlan<IN, MID>& p1,
                                    const SimplePipelinePlan<MID, OUT>& p2) {
  return FullPipelinePlan<IN, OUT>(p1.leading_clone(),
                                   p1.chain_clone(),
                                   chain(p1.trailing_, p2.f_));
}

template<typename IN,
         typename MID,
         typename OUT>
FullPipelinePlan<IN, OUT> operator|(const SimplePipelinePlan<IN, MID>& p1,
                                    const FullPipelinePlan<MID, OUT>& p2) {
  filter<IN, PipelineTerm> *leading =
      p2.leading_ ?
      new filter_chain<IN, MID, PipelineTerm>(p1.f_, p2.leading_) :
      p1.f_->clone();
  return FullPipelinePlan<IN, OUT>(leading,
                                   p2.chain_clone(),
                                   p2.trailing_clone());
}

template<typename IN,
         typename MID,
         typename OUT>
FullPipelinePlan<IN, OUT> operator|(const FullPipelinePlan<IN, MID>& p1,
                                    const FullPipelinePlan<MID, OUT>& p2) {
  pipeline_segment<PipelineTerm> *p =
      chain(p1.trailing_, p2.leading_)->chain(p2.chain_clone());
  if (p1.chain_) {
    p = p1.chain_clone()->chain(p);
  }
  return FullPipelinePlan<IN, OUT>(p1.leading_clone(),
                               p,
                               p2.trailing_clone());
}

// END PIPES

// START PIPELINE IMPLEMENTATION

template<>
void FullPipelinePlan<PipelineTerm, PipelineTerm>::run(
    PipelineExecution* pex) {
  if(chain_) {
    chain_->Run(pex);
  }
  trailing_->Run(pex);
}

// END PIPELINE IMPLEMENTATION

// START PIPELINE_EXECUTION IMPLEMENTATION

void RunFilter(PipelineExecution* pex,
               filter<PipelineTerm, PipelineTerm> *f) {
  pex->start_.wait();
  bool b = true;
  while (b) {
    b = f->Run();
  }
  f->Close();

  pex->thread_end_->count_down_and_wait();
}

PipelineExecution::PipelineExecution(const PipelinePlan& pp,
                                     simple_thread_pool* pool)
      : pp_(pp.clone()), pool_(pool),
        start_(1), thread_end_(NULL), end_(1),
        num_threads_(0), done_(false) {
  pp_->run(this);
  thread_end_ = new barrier(num_threads_,
                            bind(&PipelineExecution::threads_done, this));
  start_.count_down();  // Start the threads
}

PipelineExecution::~PipelineExecution() {
  wait();
  delete pp_;
  delete thread_end_;
}

void PipelineExecution::execute(filter<PipelineTerm, PipelineTerm> *f) {
  num_threads_++;
  pool_->try_get_unused_thread()->execute(bind(RunFilter, this, f));
}


// END PIPELINE_EXECUTION IMPLEMENTATION



} // namespace
#endif  // GCL_PIPELINE_