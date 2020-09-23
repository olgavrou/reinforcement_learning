#pragma once

#include <vector>
#include <memory>
#include "vw.h"
#include "model_mgmt.h"

namespace reinforcement_learning {

  class safe_vw {
    // we need to keep a reference to the master around, so it's still valid even if the factory is deleted
    std::shared_ptr<safe_vw> _master;
    vw* _vw;
    std::vector<example*> _example_pool;

    example* get_or_create_example();
    static example& get_or_create_example_f(void* vw);

  public:
    safe_vw(const std::shared_ptr<safe_vw>& master);
    safe_vw(const char* model_data, size_t len);
    safe_vw(std::string vw_parameters);

    ~safe_vw();

    void parse_context_with_pdf(const char* context, std::vector<int>& actions, std::vector<float>& scores);
    void rank(const char* context, std::vector<int>& actions, std::vector<float>& scores);
    void choose_continuous_action(const char* context, float& action, float& pdf_value);
    // Used for CCB
    void rank_decisions(const std::vector<const char*>& event_ids, const char* context, std::vector<std::vector<uint32_t>>& actions, std::vector<std::vector<float>>& scores);
    // Used for slates
    void rank_slates_decisions(const char* event_id, uint32_t slot_count, const char* context, std::vector<std::vector<uint32_t>>& actions, std::vector<std::vector<float>>& scores);

    const char* id() const;

    bool is_compatible(const std::string& args) const;

    friend class safe_vw_factory;
  };

  class safe_vw_factory {
    model_management::model_data _master_data;
    std::string _command_line;

  public:
    // model_data is copied and stored in the factory object.
    safe_vw_factory(const std::string& command_line);
    safe_vw_factory(const model_management::model_data& master_data);
    safe_vw_factory(const model_management::model_data&& master_data);

    safe_vw* operator()();
  };
}
