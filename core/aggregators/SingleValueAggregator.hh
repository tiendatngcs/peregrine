#ifndef SINGLE_VALUE_AGGREGATOR_HH
#define SINGLE_VALUE_AGGREGATOR_HH

#include "../Options.hh"
#include "../Barrier.hh"
#include "../OutputManager.hh"

namespace Peregrine
{

  template <typename AggValueT>
  struct SVAggItem
  {
    AggValueT *v;
    bool fresh;
  };
  
  template <typename AggValueT, OnTheFlyOption onthefly, StoppableOption stoppable, typename ViewFunc, OutputOption Output = NONE>
  struct SVAggHandle;
  
  template <typename AggValueT, OnTheFlyOption onthefly, StoppableOption stoppable, typename ViewFunc, OutputOption Output = NONE>
  struct SVAggregator
  {
    using ViewType = decltype(std::declval<ViewFunc>()(std::declval<AggValueT>()));
    using AggHandle = SVAggHandle<AggValueT, onthefly, stoppable, ViewFunc, Output>;
  
    AggValueT global;
    std::vector<std::atomic<SVAggItem<AggValueT>>> values;
    std::vector<AggHandle *> handles;
    std::atomic<flag_t> flag;
    ViewFunc viewer;
    std::atomic<ViewType> latest_result;

    SVAggregator(uint32_t nworkers, ViewFunc &vf)
      : values(nworkers),
        handles(nworkers),
        flag({false, false}),
        viewer(vf),
        latest_result(ViewType())
    {}


    SVAggregator(SVAggregator &) = delete;
    ~SVAggregator() { for (auto handle : handles) delete handle; }
  
    bool stale(uint32_t id) const
    {
      assert(!values.empty());
      return !values[id].load().fresh;
    }
  
    void set_fresh(uint32_t id)
    {
      values[id].store({values[id].load().v, true});
    }
  
    void update()
    {
      flag_t expected = ON();
      if (flag.compare_exchange_strong(expected, WORKING()))
      {
        update_unchecked();
        flag_t e = WORKING();
        flag.compare_exchange_strong(e, ON());
      }
    }
  
    void update_unchecked()
    {
      assert(!values.empty());
      for (auto &val : values)
      {
        if (val.load().fresh)
        {
          auto i = val.load().v;
          assert(i != nullptr);
          global += *i;
          val.store({i, false});
        }
      }
  
      latest_result.store(viewer(global));
    }
  
    void get_result()
    {
      // wait for aggregator thread to stop
      flag_t expected = ON();
      while (!flag.compare_exchange_weak(expected, OFF()));
  
      update_unchecked();
      for (auto &handle : handles)
      {
        handle->submit();
      }
      update_unchecked();
    }
  
    void reset()
    {
      global.reset();
      latest_result = ViewType();
  
      // cmp_exchg isn't really necessary but
      flag_t expected = OFF();
      flag.compare_exchange_strong(expected, ON());
    }
  
    void register_handle(uint32_t id, AggHandle *ah)
    {
      values[id] = {&ah->other, false};
      handles[id] = ah;
    }
  };
  
  template <typename AggValueT, OnTheFlyOption onthefly, StoppableOption stoppable, typename ViewFunc, OutputOption Output>
  struct SVAggHandle
  {
    using ViewType = decltype(std::declval<ViewFunc>()(std::declval<AggValueT>()));
    using Aggregator = SVAggregator<AggValueT, onthefly, stoppable, ViewFunc, Output>;
    AggValueT curr;
    AggValueT other;
  
    uint32_t id;
    Aggregator *agg;
    Barrier &barrier;

    OutputManager<Output> bm;

    SVAggHandle(uint32_t tid, Aggregator *a, Barrier &b) : id(tid), agg(a), barrier(b) {}
  
    void map(const std::vector<uint32_t> &, const auto &v)
    {
      curr += v;
    }
  
    void reset()
    {
      curr.reset();
      other.reset();
      if constexpr (Output != NONE)
      {
        bm.reset(id);
      }
    }
  
    ViewType read_value(const std::vector<uint32_t> &)
    {
      return agg->latest_result.load();
    }
  

    void stop() requires (stoppable == STOPPABLE)
    {
      barrier.stopAll();
    }
  
    void submit()
    {
      // if other has been read
      if (agg && agg->stale(id))
      {
        // swap curr and other
        std::swap(curr, other);
        curr.reset();
  
        // set freshness
        agg->set_fresh(id);
      }

      if constexpr (Output != NONE)
      {
        // flush output buffer
        bm.flush();
      }
    }

    template <OutputFormat fmt> requires (Output != NONE)
    void output(const std::vector<uint32_t> &vertices)
    {
      bm.template output<fmt>(vertices);
    }
  };
}

#endif
