#include "joiners/example_joiner.h"
#include "log_converter.h"

#include "event_processors/typed_events.h"
#include "generated/v2/DedupInfo_generated.h"
#include "generated/v2/Event_generated.h"
#include "generated/v2/OutcomeEvent_generated.h"
#include "zstd.h"

#include <limits.h>
#include <time.h>

// VW headers
#include "parse_example_json.h"
#include "parser.h"

example_joiner::example_joiner(vw *vw)
    : _vw(vw), _reward_calculation(&reward::earliest), _binary_to_json(false) {}

example_joiner::example_joiner(vw *vw, bool binary_to_json,
                               std::string outfile_name)
    : _vw(vw), _reward_calculation(&reward::earliest),
      _binary_to_json(binary_to_json) {
  _outfile.open(outfile_name, std::ofstream::out);
}

example_joiner::~example_joiner() {
  // cleanup examples
  _dedup_cache.clear(return_example_f, this);
  for (auto *ex : _example_pool) {
    VW::dealloc_examples(ex, 1);
  }
  if (_binary_to_json) {
    _outfile.close();
  }
}

example *example_joiner::get_or_create_example() {
  // alloc new element if we don't have any left
  if (_example_pool.size() == 0) {
    auto ex = VW::alloc_examples(1);
    _vw->example_parser->lbl_parser.default_label(&ex->l);

    return ex;
  }

  // get last element
  example *ex = _example_pool.back();
  _example_pool.pop_back();

  _vw->example_parser->lbl_parser.default_label(&ex->l);
  VW::empty_example(*_vw, *ex);

  return ex;
}

void example_joiner::return_example(example *ex) {
  _example_pool.push_back(ex);
}

example &example_joiner::get_or_create_example_f(void *vw) {
  return *(((example_joiner *)vw)->get_or_create_example());
}

void example_joiner::return_example_f(void *vw, example *ex) {
  ((example_joiner *)vw)->return_example(ex);
}

bool example_joiner::process_event(const v2::JoinedEvent &joined_event) {
  if (joined_event.event() == nullptr || joined_event.timestamp() == nullptr) {
    VW::io::logger::log_error(
        "JoinedEvent is malformed, can not process JoinedEvent");
    return false;
  }

  auto event = flatbuffers::GetRoot<v2::Event>(joined_event.event()->data());

  if (event->meta() == nullptr || event->meta()->id() == nullptr) {
    VW::io::logger::log_error(
        "Event of JoinedEvent is malformed, can not process JoinedEvent");
    return false;
  }

  std::string id = event->meta()->id()->str();

  if (event->meta()->payload_type() == v2::PayloadType_DedupInfo) {
    if (!process_dedup(*event, *event->meta())) {
      // clean everything this batch is ruined without the dedup info
      // dedup cache will clear itself up when next dedup payload arrives
      clear_batch_info();
      return false;
    }
    return true;
  }
  if (_batch_grouped_events.find(id) != _batch_grouped_events.end()) {
    _batch_grouped_events[id].push_back(&joined_event);
  } else {
    _batch_grouped_events.insert({id, {&joined_event}});
    _batch_event_order.emplace(id);
  }
  return true;
}

void example_joiner::set_default_reward(float default_reward, bool sticky) {
  _loop_info.default_reward.set(default_reward, sticky);
}

void example_joiner::set_learning_mode_config(
    v2::LearningModeType learning_mode, bool sticky) {
  _loop_info.learning_mode_config.set(learning_mode, sticky);
}

void example_joiner::set_problem_type_config(v2::ProblemType problem_type,
                                             bool sticky) {
  _loop_info.problem_type_config.set(problem_type, sticky);
}

bool example_joiner::joiner_ready() {
  return _loop_info.is_configured() && _reward_calculation.is_valid();
}

void example_joiner::set_reward_function(const v2::RewardFunctionType type,
                                         bool sticky) {

  reward::RewardFunctionType reward_calculation = nullptr;
  switch (type) {
  case v2::RewardFunctionType_Earliest:
    reward_calculation = &reward::earliest;
    break;
  case v2::RewardFunctionType_Average:
    reward_calculation = &reward::average;
    break;

  case v2::RewardFunctionType_Sum:
    reward_calculation = &reward::sum;
    break;

  case v2::RewardFunctionType_Min:
    reward_calculation = &reward::min;
    break;

  case v2::RewardFunctionType_Max:
    reward_calculation = &reward::max;
    break;

  case v2::RewardFunctionType_Median:
    reward_calculation = &reward::median;
    break;

  default:
    break;
  }

  if (reward_calculation) {
    _reward_calculation.set(reward_calculation, sticky);
  }
}

void example_joiner::clear_batch_info() {
  _batch_grouped_events.clear();
  _batch_grouped_examples.clear();
  while (!_batch_event_order.empty()) {
    _batch_event_order.pop();
  }
}

void example_joiner::clear_vw_examples(v_array<example *> &examples) {
  // cleanup examples since something might have gone wrong and left the
  // existing example's in a bad state
  VW::return_multiple_example(*_vw, examples);
  // add one new example since all parsers expect to be called with one unused
  // example
  examples.push_back(&VW::get_unused_example(_vw));
}

void example_joiner::clear_event_id_batch_info(const std::string &id) {
  _batch_grouped_events.erase(id);
  if (!_batch_event_order.empty() && _batch_event_order.front() == id) {
    _batch_event_order.pop();
  }
  _batch_grouped_examples.erase(id);
}

void example_joiner::invalidate_joined_event(const std::string &id) {
  if (_batch_grouped_examples.find(id) != _batch_grouped_examples.end()) {
    _batch_grouped_examples[id].ok = false;
  }
}

bool example_joiner::process_interaction(const v2::Event &event,
                                         const v2::Metadata &metadata,
                                         const TimePoint &enqueued_time_utc,
                                         v_array<example *> &examples) {

  if (EnumNamePayloadType(metadata.payload_type()) !=
      EnumNameProblemType(_loop_info.problem_type_config)) {
    VW::io::logger::log_warn(
        "Online Trainer mode [{}] "
        "and Interaction event type [{}] "
        "don't match. Skipping interaction from processing. "
        "EventId: [{}]",
        EnumNameProblemType(_loop_info.problem_type_config),
        EnumNamePayloadType(metadata.payload_type()), metadata.id()->c_str());
    return false;
  }

  joined_event::joined_event je;

  if (metadata.payload_type() == v2::PayloadType_CB) {

    const v2::CbEvent *cb = nullptr;
    if (!typed_event::process_compression<v2::CbEvent>(
            event.payload()->data(), event.payload()->size(), metadata, cb,
            _detached_buffer) ||
        cb == nullptr) {
      return false;
    }

    if (!typed_event::event_processor<v2::CbEvent>::is_valid(*cb, _loop_info)) {
      VW::io::logger::log_warn("CB payload with event id [{}] is malformed. "
                               "Skipping interaction from processing.",
                               metadata.id()->c_str());
      return false;
    }

    je = std::move(
        typed_event::event_processor<v2::CbEvent>::fill_in_joined_event(
            *cb, metadata, enqueued_time_utc,
            typed_event::event_processor<v2::CbEvent>::get_context(*cb)));
  } else if (metadata.payload_type() == v2::PayloadType_CCB) {
    const v2::MultiSlotEvent *ccb = nullptr;
    if (!typed_event::process_compression<v2::MultiSlotEvent>(
            event.payload()->data(), event.payload()->size(), metadata, ccb,
            _detached_buffer) ||
        ccb == nullptr) {
      return false;
    }

    if (!typed_event::event_processor<v2::MultiSlotEvent>::is_valid(
            *ccb, _loop_info)) {
      VW::io::logger::log_warn("CCB payload with event id [{}] is malformed. "
                               "Skipping interaction from processing.",
                               metadata.id()->c_str());
      return false;
    }
    je = std::move(
        typed_event::event_processor<v2::MultiSlotEvent>::fill_in_joined_event(
            *ccb, metadata, enqueued_time_utc,
            typed_event::event_processor<v2::MultiSlotEvent>::get_context(
                *ccb)));
  }

  else {
    // for now only CB is supported so log and return false
    VW::io::logger::log_error("Interaction event learning mode [{}] not "
                              "currently supported, skipping interaction "
                              "EventId: [{}]",
                              metadata.payload_type(), metadata.id()->c_str());
    return false;
  }

  std::string context(je.context);

  try {
    if (_vw->audit || _vw->hash_inv) {
      VW::template read_line_json<true>(
          *_vw, examples, const_cast<char *>(context.c_str()),
          reinterpret_cast<VW::example_factory_t>(&VW::get_unused_example), _vw,
          &_dedup_cache.dedup_examples);
    } else {
      VW::template read_line_json<false>(
          *_vw, examples, const_cast<char *>(context.c_str()),
          reinterpret_cast<VW::example_factory_t>(&VW::get_unused_example), _vw,
          &_dedup_cache.dedup_examples);
    }
  } catch (VW::vw_exception &e) {
    VW::io::logger::log_warn(
        "JSON parsing during interaction processing failed "
        "with error: [{}] for event with id: [{}]",
        e.what(), metadata.id()->c_str());
    return false;
  }

  _batch_grouped_examples.emplace(
      std::make_pair<std::string, joined_event::joined_event>(
          metadata.id()->str(), std::move(je)));
  return true;
}

bool example_joiner::process_outcome(const v2::Event &event,
                                     const v2::Metadata &metadata,
                                     const TimePoint &enqueued_time_utc) {
  reward::outcome_event o_event;
  o_event.metadata = {metadata.client_time_utc()
                          ? timestamp_to_chrono(*metadata.client_time_utc())
                          : TimePoint(),
                      metadata.app_id() ? metadata.app_id()->str() : "",
                      metadata.payload_type(),
                      metadata.pass_probability(),
                      metadata.encoding(),
                      metadata.id()->str()};
  o_event.enqueued_time_utc = enqueued_time_utc;

  const v2::OutcomeEvent *outcome = nullptr;
  if (!typed_event::process_compression<v2::OutcomeEvent>(
          event.payload()->data(), event.payload()->size(), metadata, outcome,
          _detached_buffer) ||
      outcome == nullptr) {
    // invalidate joined_event so that we don't learn from it
    invalidate_joined_event(metadata.id()->str());
    return false;
  }

  if (outcome->value_type() == v2::OutcomeValue_literal) {
    o_event.s_value = outcome->value_as_literal()->c_str();
  } else if (outcome->value_type() == v2::OutcomeValue_numeric) {
    o_event.value = outcome->value_as_numeric()->value();
  }

  if (outcome->index_type() == v2::IndexValue_literal) {
    o_event.s_index = outcome->index_as_literal()->c_str();
  } else if (outcome->index_type() == v2::IndexValue_numeric) {
    o_event.index = outcome->index_as_numeric()->index();
  }

  o_event.action_taken = outcome->action_taken();

  if (_batch_grouped_examples.find(metadata.id()->str()) !=
      _batch_grouped_examples.end()) {
    auto &joined_event = _batch_grouped_examples[metadata.id()->str()];
    joined_event.outcome_events.push_back(o_event);
  }

  return true;
}

bool example_joiner::process_dedup(const v2::Event &event,
                                   const v2::Metadata &metadata) {

  const v2::DedupInfo *dedup = nullptr;
  if (!typed_event::process_compression<v2::DedupInfo>(
          event.payload()->data(), event.payload()->size(), metadata, dedup,
          _detached_buffer) ||
      dedup == nullptr) {
    return false;
  }

  if (dedup->ids()->size() != dedup->values()->size()) {
    VW::io::logger::log_error(
        "Can not process dedup payload, id and value sizes do not match");
    return false;
  }

  auto examples = v_init<example *>();

  for (size_t i = 0; i < dedup->ids()->size(); i++) {
    auto dedup_id = dedup->ids()->Get(i);
    if (!_dedup_cache.exists(dedup_id)) {

      examples.push_back(get_or_create_example());

      try {
        if (_vw->audit || _vw->hash_inv) {
          VW::template read_line_json<true>(
              *_vw, examples,
              const_cast<char *>(dedup->values()->Get(i)->c_str()),
              get_or_create_example_f, this);
        } else {
          VW::template read_line_json<false>(
              *_vw, examples,
              const_cast<char *>(dedup->values()->Get(i)->c_str()),
              get_or_create_example_f, this);
        }
      } catch (VW::vw_exception &e) {
        VW::io::logger::log_error("JSON parsing during dedup processing failed "
                                  "with error: [{}]",
                                  e.what());
        return false;
      }

      _dedup_cache.add(dedup_id, examples[0]);
      examples.clear();
    } else {
      _dedup_cache.update(dedup_id);
    }
  }

  if (dedup->ids()->size() > 0) {
    // location of first item in dedup payload will be the "last" item in the
    // cache that we care about keeping
    _dedup_cache.clear_after(dedup->ids()->Get(0), return_example_f, this);
  }

  return true;
}

bool example_joiner::process_joined(v_array<example *> &examples) {
  if (_batch_event_order.empty()) {
    return true;
  }

  auto id = _batch_event_order.front();
  bool multiline = false;

  for (auto &joined_event : _batch_grouped_events[id]) {
    auto event = flatbuffers::GetRoot<v2::Event>(joined_event->event()->data());
    auto metadata = event->meta();
    auto enqueued_time_utc = timestamp_to_chrono(*joined_event->timestamp());
    const auto &payload_type = metadata->payload_type();

    if (payload_type == v2::PayloadType_Outcome) {
      process_outcome(*event, *metadata, enqueued_time_utc);
    } else {
      multiline = (payload_type != v2::PayloadType_CA);
      if (!process_interaction(*event, *metadata, enqueued_time_utc,
                               examples)) {
        continue;
      }
    }
  }

  if (_batch_grouped_examples.find(id) == _batch_grouped_examples.end()) {
    // can't learn from this interaction
    VW::io::logger::log_warn("Events with event id [{}] were processed but "
                             "no valid interaction found. Skipping..",
                             id);
    clear_event_id_batch_info(id);
    clear_vw_examples(examples);
    return false;
  }

  auto &je = _batch_grouped_examples[id];
  if (!je.ok) {
    // don't learn from this interaction
    VW::io::logger::log_warn(
        "Interaction with event id [{}] has been invalidated due to malformed "
        "observation. Skipping...",
        id);
    clear_event_id_batch_info(id);
    clear_vw_examples(examples);
    return false;
  }

  je.fill_in_label(examples);

  if (je.interaction_metadata.payload_type == v2::PayloadType_CB) {
    je.calc_and_set_reward(examples, _loop_info.default_reward,
                           _reward_calculation.value());

    if (_binary_to_json) {
      log_converter::build_cb_json(_outfile, je);

      clear_event_id_batch_info(id);
      clear_vw_examples(examples);
      return true;
    }

    if (!je.is_joined_event_learnable()) {
      _joiner_metrics.number_of_skipped_events++;
      clear_event_id_batch_info(id);
      clear_vw_examples(examples);
      return true;
    }
  } else if (je.interaction_metadata.payload_type == v2::PayloadType_CCB) {
    je.calc_and_set_reward(examples, _loop_info.default_reward,
                           _reward_calculation.value());
  }

  _joiner_metrics.number_of_learned_events++;

  if (multiline) {
    // add an empty example to signal end-of-multiline
    examples.push_back(&VW::get_unused_example(_vw));
    _vw->example_parser->lbl_parser.default_label(&examples.back()->l);
    examples.back()->is_newline = true;
  }

  clear_event_id_batch_info(id);
  return true;
}

bool example_joiner::processing_batch() { return !_batch_event_order.empty(); }
void example_joiner::on_new_batch() {}
void example_joiner::on_batch_read() {}

metrics::joiner_metrics example_joiner::get_metrics() {
  return _joiner_metrics;
}
