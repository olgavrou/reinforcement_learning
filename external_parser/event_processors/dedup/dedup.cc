#include "dedup.h"
#include "parse_example_json.h"

dedup::dedup(vw *vw) : _vw(vw) {}

dedup::~dedup() {
  // cleanup examples
  _dedup_cache.clear(return_example_f, this);
  for (auto *ex : _example_pool) {
    VW::dealloc_examples(ex, 1);
  }
}

example *dedup::get_or_create_example() {
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

void dedup::return_example(example *ex) { _example_pool.push_back(ex); }

example &dedup::get_or_create_example_f(void *vw) {
  return *(((dedup *)vw)->get_or_create_example());
}

void dedup::return_example_f(void *vw, example *ex) {
  ((dedup *)vw)->return_example(ex);
}

bool dedup::process(const v2::DedupInfo &dedup_info) {
  if (dedup_info.ids()->size() != dedup_info.values()->size()) {
    VW::io::logger::log_error(
        "Can not process dedup payload, id and value sizes do not match");
    return false;
  }

  auto examples = v_init<example *>();

  for (size_t i = 0; i < dedup_info.ids()->size(); i++) {
    auto dedup_id = dedup_info.ids()->Get(i);
    if (!_dedup_cache.exists(dedup_id)) {

      examples.push_back(get_or_create_example());

      try {
        if (_vw->audit || _vw->hash_inv) {
          VW::template read_line_json<true>(
              *_vw, examples,
              const_cast<char *>(dedup_info.values()->Get(i)->c_str()),
              get_or_create_example_f, this);
        } else {
          VW::template read_line_json<false>(
              *_vw, examples,
              const_cast<char *>(dedup_info.values()->Get(i)->c_str()),
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

  if (dedup_info.ids()->size() > 0) {
    // location of first item in dedup payload will be the "last" item in the
    // cache that we care about keeping
    _dedup_cache.clear_after(dedup_info.ids()->Get(0), return_example_f, this);
  }
  return true;
}

std::unordered_map<uint64_t, example *> &dedup::get_deduped_examples() {
  return _dedup_cache.dedup_examples;
}