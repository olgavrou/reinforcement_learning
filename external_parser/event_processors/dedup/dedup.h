#pragma once

#include "generated/v2/DedupInfo_generated.h"
#include "lru_dedup_cache.h"

// VW headers
#include "example.h"
#include "io/logger.h"
#include "json_utils.h"

#include <vector>

namespace v2 = reinforcement_learning::messages::flatbuff::v2;

class dedup {
public:
  dedup(vw *vw);
  ~dedup();

  bool process(const v2::DedupInfo &dedup_info);
  std::unordered_map<uint64_t, example *> &get_deduped_examples();

private:
  example *get_or_create_example();

  static example &get_or_create_example_f(void *vw);

  void return_example(example *ex);

  static void return_example_f(void *vw, example *ex);
  lru_dedup_cache _dedup_cache;
  vw *_vw;
  std::vector<example *> _example_pool;
};
