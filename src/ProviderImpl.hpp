/*
 * (C) 2023 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#ifndef __WARABI_PROVIDER_IMPL_H
#define __WARABI_PROVIDER_IMPL_H

#include "config.h"
#include <valijson/adapters/nlohmann_json_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>

#include "warabi/Provider.hpp"
#include "warabi/Backend.hpp"
#include "warabi/UUID.hpp"
#include "warabi/TransferManager.hpp"
#include "warabi/MigrationOptions.hpp"
#include "BufferWrapper.hpp"
#include "Defer.hpp"

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/pair.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <tuple>

#ifdef WARABI_HAS_REMI
#include <remi/remi-client.h>
#include <remi/remi-server.h>
#endif

#define FIND_TARGET(__var__) \
        std::shared_ptr<TargetEntry> __var__;\
        do {\
            std::lock_guard<tl::mutex> lock(m_targets_mtx);\
            auto it = m_targets.find(target_id);\
            if(it == m_targets.end()) {\
                result.success() = false;\
                result.error() = "Target with UUID "s + target_id.to_string() + " not found";\
                error(result.error());\
                return;\
            }\
            __var__ = it->second;\
        } while(0)

namespace warabi {

using namespace std::string_literals;
namespace tl = thallium;

static constexpr const char* configSchema = R"(
{
  "type": "object",
  "properties": {
    "targets": {
      "type": "array",
        "items": {
          "type": "object",
          "properties": {
            "type": {"type": "string"},
            "config": {
              "type": "object",
              "properties": {
                "transfer_manager": {"type": "string"}
              }
            }
          },
          "required": ["type"]
        }
      },
      "transfer_managers": {
        "type": "object",
        "patternProperties": {
        ".*": {
          "type": "object",
          "properties": {
            "type": {"type": "string"},
            "config": {"type": "object"}
          },
          "required": ["type"]
        }
      }
    }
  }
}
)";

/**
 * @brief This class automatically deregisters a tl::remote_procedure
 * when the ProviderImpl's destructor is called.
 */
struct AutoDeregistering : public tl::remote_procedure {

    AutoDeregistering(tl::remote_procedure rpc)
    : tl::remote_procedure(std::move(rpc)) {}

    ~AutoDeregistering() {
        deregister();
    }
};

/**
 * @brief This class automatically calls req.respond(resp)
 * when its constructor is called, helping developers not
 * forget to respond in all code branches.
 */
template<typename ResponseType>
struct AutoResponse {

    AutoResponse(const tl::request& req, ResponseType& resp)
    : m_req(req)
    , m_resp(resp) {}

    AutoResponse(const AutoResponse&) = delete;
    AutoResponse(AutoResponse&&) = delete;

    ~AutoResponse() {
        m_req.respond(m_resp);
    }

    const tl::request& m_req;
    ResponseType&      m_resp;
};

struct TargetEntry {

    std::shared_ptr<Backend>         m_target;
    std::shared_ptr<TransferManager> m_transfer_manager;
    std::string                      m_transfer_manager_name;

    TargetEntry(std::shared_ptr<Backend> target,
                std::shared_ptr<TransferManager> tm,
                std::string tm_name)
    : m_target(std::move(target))
    , m_transfer_manager(std::move(tm))
    , m_transfer_manager_name(std::move(tm_name)) {}

    Backend* operator->() { return m_target.get(); }
    Backend& operator*() { return *m_target; }
    const Backend* operator->() const { return m_target.get(); }
    const Backend& operator*() const { return *m_target; }
};

class ProviderImpl : public tl::provider<ProviderImpl> {

    auto id() const { return get_provider_id(); } // for convenience

    using json = nlohmann::json;

    #define DEF_LOGGING_FUNCTION(__name__)                         \
    template<typename ... Args>                                    \
    void __name__(Args&&... args) {                                \
        auto msg = fmt::format(std::forward<Args>(args)...);       \
        spdlog::__name__("[warabi:{}] {}", get_provider_id(), msg); \
    }

    DEF_LOGGING_FUNCTION(trace)
    DEF_LOGGING_FUNCTION(debug)
    DEF_LOGGING_FUNCTION(info)
    DEF_LOGGING_FUNCTION(warn)
    DEF_LOGGING_FUNCTION(error)
    DEF_LOGGING_FUNCTION(critical)

    #undef DEF_LOGGING_FUNCTION

    public:

    tl::engine           m_engine;
    tl::pool             m_pool;
    remi_client_t        m_remi_client;
    remi_provider_t      m_remi_provider;
    // Admin RPC
    AutoDeregistering m_add_target;
    AutoDeregistering m_remove_target;
    AutoDeregistering m_destroy_target;
    AutoDeregistering m_add_transfer_manager;
    AutoDeregistering m_migrate_target;
    // Client RPC
    AutoDeregistering m_check_target;
    AutoDeregistering m_create;
    AutoDeregistering m_write;
    AutoDeregistering m_write_eager;
    AutoDeregistering m_persist;
    AutoDeregistering m_create_write;
    AutoDeregistering m_create_write_eager;
    AutoDeregistering m_read;
    AutoDeregistering m_read_eager;
    AutoDeregistering m_erase;

    // Backends
    std::unordered_map<UUID, std::shared_ptr<TargetEntry>> m_targets;
    mutable tl::mutex                                      m_targets_mtx;
    std::unordered_map<std::string, std::shared_ptr<TransferManager>> m_transfer_managers;
    mutable tl::mutex                                                 m_transfer_managers_mtx;

    ProviderImpl(
            const tl::engine& engine,
            uint16_t provider_id,
            const std::string& config,
            const tl::pool& pool,
            remi_client_t remi_cl,
            remi_provider_t remi_pr)
    : tl::provider<ProviderImpl>(engine, provider_id)
    , m_engine(engine)
    , m_pool(pool)
    , m_remi_client(remi_cl)
    , m_remi_provider(remi_pr)
    , m_add_target(define("warabi_add_target", &ProviderImpl::addTargetRPC, pool))
    , m_remove_target(define("warabi_remove_target", &ProviderImpl::removeTargetRPC, pool))
    , m_destroy_target(define("warabi_destroy_target", &ProviderImpl::destroyTargetRPC, pool))
    , m_add_transfer_manager(define("warabi_add_transfer_manager", &ProviderImpl::addTransferManagerRPC, pool))
    , m_migrate_target(define("warabi_migrate_target", &ProviderImpl::migrateTargetRPC, pool))
    , m_check_target(define("warabi_check_target", &ProviderImpl::checkTargetRPC, pool))
    , m_create(define("warabi_create",  &ProviderImpl::createRPC, pool))
    , m_write(define("warabi_write",  &ProviderImpl::writeRPC, pool))
    , m_write_eager(define("warabi_write_eager",  &ProviderImpl::writeEagerRPC, pool))
    , m_persist(define("warabi_persist",  &ProviderImpl::persistRPC, pool))
    , m_create_write(define("warabi_create_write",  &ProviderImpl::createWriteRPC, pool))
    , m_create_write_eager(define("warabi_create_write_eager",  &ProviderImpl::createWriteEagerRPC, pool))
    , m_read(define("warabi_read",  &ProviderImpl::readRPC, pool))
    , m_read_eager(define("warabi_read_eager",  &ProviderImpl::readEagerRPC, pool))
    , m_erase(define("warabi_erase",  &ProviderImpl::eraseRPC, pool))
    {
        trace("Registered provider with id {}", get_provider_id());
        json json_config;
        try {
            if(!config.empty())
                json_config = json::parse(config);
            else
                json_config = json::object();
        } catch(json::parse_error& e) {
            auto err = fmt::format("Could not parse warabi provider configuration: {}", e.what());
            error("{}", err);
            throw Exception(err);
        }

#ifdef WARABI_HAS_REMI
        if(m_remi_client && !m_remi_provider) {
            warn("Warabi provider initialized with only a REMI client"
                 " will only be able to *send* targets to other providers");
        } else if(!m_remi_client && m_remi_provider) {
            warn("Warabi provider initialized with only a REMI provider"
                 " will only be able to *receive* targets from other providers");
        }
        if(m_remi_provider) {
            std::string remi_class = fmt::format("warabi/{}", provider_id);
            int rret = remi_provider_register_migration_class(
                    m_remi_provider, remi_class.c_str(),
                    beforeMigrationCallback,
                    afterMigrationCallback, nullptr, this);
            if(rret != REMI_SUCCESS) {
                throw Exception(fmt::format(
                    "Failed to register migration class in REMI:"
                    " remi_provider_register_migration_class returned {}",
                    rret));
            }
        }
#else
        if(m_remi_client || m_remi_provider) {
            error("Provided REMI client or provider will be ignored because"
                  " Warabi wasn't built with REMI support");
        }
#endif
        static json schemaDocument = json::parse(configSchema);

        valijson::Schema schemaValidator;
        valijson::SchemaParser schemaParser;
        valijson::adapters::NlohmannJsonAdapter schemaAdapter(schemaDocument);
        schemaParser.populateSchema(schemaAdapter, schemaValidator);

        valijson::Validator validator;
        valijson::adapters::NlohmannJsonAdapter jsonAdapter(json_config);

        valijson::ValidationResults validationResults;
        validator.validate(schemaValidator, jsonAdapter, &validationResults);

        if(validationResults.numErrors() != 0) {
            error("Error(s) while validating JSON config for warabi provider:");
            for(auto& err : validationResults) {
                error("\t{}", err.description);
            }
            throw Exception("Invalid JSON configuration (see error logs for information)");
        }
        auto transfer_managers = json_config.value("transfer_managers", json::object());
        for(auto& [key, val] : transfer_managers.items()) {
            auto& tm_type = val["type"].get_ref<const std::string&>();
            auto tm_config = val.value("config", json::object());
            auto valid = validateTransferManagerConfig(tm_type, tm_config);
            if(!valid.success()) throw Exception(valid.error());
        }
        for(auto& [key, val] : transfer_managers.items()) {
            const std::string& tm_type = val["type"].get_ref<const std::string&>();
            auto tm_config = val.value("config", json::object());
            addTransferManager(key, tm_type, tm_config.dump());
        }
        if(!m_transfer_managers.count("__default__")) {
            addTransferManager("__default__", "__default__", json::object());
        }

        if(!json_config.contains("targets")) return;
        auto& targets = json_config["targets"];
        for(auto& target : targets) {
            auto& target_type = target["type"].get_ref<const std::string&>();
            auto target_config = target.contains("config") ? target["config"] : json::object();
            auto valid = validateTargetConfig(target_type, target_config);
            if(!valid.success()) throw Exception(valid.error());
        }
        for(auto& target : targets) {
            const std::string& target_type = target["type"].get_ref<const std::string&>();
            auto target_config = target.contains("config") ? target["config"] : json::object();
            addTarget(target_type, target_config.dump());
        }
    }

    ~ProviderImpl() {
        trace("Deregistering provider");
    }

    std::string getConfig() const {
        auto l1 = std::unique_lock<tl::mutex>{m_targets_mtx};
        auto l2 = std::unique_lock<tl::mutex>{m_transfer_managers_mtx};
        auto config = json::object();
        config["targets"] = json::array();
        for(auto& pair : m_targets) {
            auto target_config = json::object();
            const auto& target_entry = *pair.second;
            target_config["__id__"] = pair.first.to_string();
            target_config["type"] = target_entry->name();
            target_config["config"] = json::parse(target_entry->getConfig());
            target_config["config"]["transfer_manager"] = target_entry.m_transfer_manager_name;
            config["targets"].push_back(target_config);
        }
        config["transfer_managers"] = json::object();
        auto& tms = config["transfer_managers"];
        for(auto& pair : m_transfer_managers) {
            tms[pair.first] = json::object();
            tms[pair.first]["type"] = pair.second->name();
            tms[pair.first]["config"] = json::parse(pair.second->getConfig());
        }
        return config.dump();
    }

    Result<bool> validateTargetConfig(const std::string& target_type,
                                      const json& target_config) {
        return TargetFactory::validateConfig(target_type, target_config);
    }

    Result<UUID> addTarget(const std::string& target_type,
                           const json& json_config) {

        auto target_id = UUID::generate();
        Result<UUID> result;

        auto target = TargetFactory::createTarget(target_type, get_engine(), json_config);

        if(not target.success()) {
            result.success() = false;
            result.error() = target.error();
            return result;
        } else {
            std::lock_guard<tl::mutex> lock(m_targets_mtx);
            std::lock_guard<tl::mutex> lock2(m_transfer_managers_mtx);

            auto tm_name = json_config.value("transfer_manager", "__default__");
            if(m_transfer_managers.count(tm_name) == 0) {
                result.success() = false;
                result.error() = fmt::format("Could not find transfer manager named {}", tm_name);
                return result;
            }
            auto tm = m_transfer_managers[tm_name];

            m_targets[target_id] = std::make_shared<TargetEntry>(
                std::shared_ptr<Backend>(std::move(target.value())),
                tm, tm_name);
            result.value() = target_id;
        }

        trace("Successfully added target {} of type {}",
              target_id.to_string(), target_type);
        return result;
    }

    Result<UUID> addTarget(const std::string& target_type,
                           const std::string& target_config) {

        Result<UUID> result;

        json json_config;
        try {
            json_config = json::parse(target_config);
        } catch(json::parse_error& ex) {
            result.error() = ex.what();
            result.success() = false;
            error("Could not parse target configuration for target");
            return result;
        }

        auto valid = validateTargetConfig(target_type, json_config);
        if(!valid.success()) {
            result.error() = valid.error();
            result.success() = false;
            return result;
        }

        result = addTarget(target_type, json_config);
        return result;
    }

    void addTargetRPC(const tl::request& req,
                      const std::string& target_type,
                      const std::string& target_config) {

        trace("Received addTarget request");
        trace(" => type = {}", target_type);
        trace(" => config = {}", target_config);

        Result<UUID> result;
        AutoResponse<decltype(result)> response{req, result};

        result = addTarget(target_type, target_config);
    }

    void removeTargetRPC(const tl::request& req,
                         const UUID& target_id) {
        trace("Received removeTarget request for target {}",
              target_id.to_string());

        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};

        {
            std::lock_guard<tl::mutex> lock(m_targets_mtx);

            if(m_targets.count(target_id) == 0) {
                result.success() = false;
                result.error() = "Target "s + target_id.to_string() + " not found";
                error(result.error());
                return;
            }

            m_targets.erase(target_id);
        }

        trace("Target {} successfully removed", target_id.to_string());
    }

    void destroyTargetRPC(const tl::request& req,
                          const UUID& target_id) {
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
        trace("Received destroyTarget request for target {}", target_id.to_string());

        {
            std::lock_guard<tl::mutex> lock(m_targets_mtx);

            if(m_targets.count(target_id) == 0) {
                result.success() = false;
                result.error() = "Target "s + target_id.to_string() + " not found";
                error(result.error());
                return;
            }
            auto& target_entry = *m_targets[target_id];
            result = target_entry->destroy();
            m_targets.erase(target_id);
        }

        trace("Target {} successfully destroyed", target_id.to_string());
    }

    Result<bool> validateTransferManagerConfig(
            const std::string& type,
            const json& config) {
        return TransferManagerFactory::validateConfig(type, config);
    }

    Result<bool> addTransferManager(const std::string& name,
                                    const std::string& type,
                                    const json& config) {

        std::lock_guard<tl::mutex> lock(m_transfer_managers_mtx);
        Result<bool> result;
        if(m_transfer_managers.count(name) != 0) {
            result.success() = false;
            result.error() = fmt::format("A TransferManager with name \"{}\" already exists", name);
            return result;
        }
        auto tm = TransferManagerFactory::createTransferManager(type, m_engine, config);
        if(not tm.success()) {
            result.success() = false;
            result.error() = tm.error();
            return result;
        } else {
            m_transfer_managers[name] = std::move(tm.value());
        }
        trace("Successfully added transfer manager {} of type {}", name, type);
        return result;
    }

    Result<bool> addTransferManager(const std::string& name,
                                    const std::string& type,
                                    const std::string& config) {

        Result<bool> result;

        json json_config;
        try {
            json_config = json::parse(config);
        } catch(json::parse_error& ex) {
            result.error() = ex.what();
            result.success() = false;
            error("Could not parse target configuration for target");
            return result;
        }

        auto valid = validateTransferManagerConfig(type, json_config);
        if(!valid.success()) {
            result.error() = valid.error();
            result.success() = false;
            return result;
        }

        result = addTransferManager(name, type, json_config);
        return result;
    }

    void addTransferManagerRPC(const tl::request& req,
                               const std::string& name,
                               const std::string& type,
                               const std::string& config) {

        trace("Received addTransferManager request");
        trace(" => name = {}", name);
        trace(" => type = {}", type);
        trace(" => config = {}", config);

        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};

        result = addTransferManager(name, type, config);
    }

    void migrateTargetRPC(const tl::request& req,
                          const UUID& target_id,
                          const std::string& dest_address,
                          uint16_t dest_provider_id,
                          const MigrationOptions& options) {
        trace("Received migrateTarget request for target {}", target_id.to_string());
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
#ifndef WARABI_HAS_REMI
        result.error() = "Warabi was not compiled with REMI support";
        result.success() = false;
#else

        #define HANDLE_REMI_ERROR(func, rret, ...) do {                          \
            if(rret != REMI_SUCCESS) {                                           \
                result.success() = false;                                        \
                result.error() = fmt::format(__VA_ARGS__);                       \
                result.error() += fmt::format(" ({} returned {})", #func, rret); \
                return;                                                          \
            }                                                                    \
        } while(0)

        // lookup destination address
        tl::endpoint dest_endpoint;
        try {
            dest_endpoint = m_engine.lookup(dest_address);
        } catch(const std::exception& ex) {
            result.success() = false;
            result.error() = fmt::format("Failed to lookup destination address: {}", ex.what());
            return;
        }
        // create remi provider handle
        remi_provider_handle_t remi_ph = NULL;
        int rret = remi_provider_handle_create(
                m_remi_client, dest_endpoint.get_addr(), dest_provider_id, &remi_ph);
        HANDLE_REMI_ERROR(remi_provider_handle_create, rret, "Failed to create REMI provider handle");
        DEFER(remi_provider_handle_release(remi_ph));
        // find target
        FIND_TARGET(target);
        // get a MigrationHandle
        auto& tg = target->m_target;
        auto startMigration = tg->startMigration(options.removeSource);
        if(!startMigration.success()) {
            result.error() = startMigration.error();
            result.success() = false;
            return;
        }
        auto& mh = startMigration.value();
        // create REMI fileset
        remi_fileset_t fileset = REMI_FILESET_NULL;
        std::string remi_class = fmt::format("warabi/{}", dest_provider_id);
        rret = remi_fileset_create(remi_class.c_str(), mh->getRoot().c_str(), &fileset);
        HANDLE_REMI_ERROR(remi_fileset_create, rret, "Failed to create REMI fileset");
        DEFER(remi_fileset_free(fileset));
        // fill REMI fileset
        for(const auto& file : mh->getFiles()) {
            if(!file.empty() && file.back() == '/') {
                rret = remi_fileset_register_directory(fileset, file.c_str());
                HANDLE_REMI_ERROR(remi_fileset_register_directory, rret,
                        "Failed to register directory {} in REMI fileset", file);
            } else {
                rret = remi_fileset_register_file(fileset, file.c_str());
                HANDLE_REMI_ERROR(remi_fileset_register_file, rret,
                        "Failed to register file {} in REMI fileset", file);
            }
        }
        // register REMI metadata
        rret = remi_fileset_register_metadata(fileset, "uuid", target_id.to_string().c_str());
        HANDLE_REMI_ERROR(remi_fileset_register_metadata, rret, "Failed to register metadata in REMI fileset");
        rret = remi_fileset_register_metadata(fileset, "config", tg->getConfig().c_str());
        HANDLE_REMI_ERROR(remi_fileset_register_metadata, rret, "Failed to register metadata in REMI fileset");
        rret = remi_fileset_register_metadata(fileset, "type", tg->name().c_str());
        HANDLE_REMI_ERROR(remi_fileset_register_metadata, rret, "Failed to register metadata in REMI fileset");
        rret = remi_fileset_register_metadata(fileset, "migration_config", options.extraConfig.c_str());
        HANDLE_REMI_ERROR(remi_fileset_register_metadata, rret, "Failed to register metadata in REMI fileset");
        // set block transfer size
        if(options.transferSize) {
            rret = remi_fileset_set_xfer_size(fileset, options.transferSize);
            HANDLE_REMI_ERROR(remi_fileset_set_xfer_size, rret, "Failed to set transfer size for REMI fileset");
        }
        // issue migration
        int remi_status = 0;
        rret = remi_fileset_migrate(remi_ph, fileset, options.newRoot.c_str(),
                REMI_KEEP_SOURCE, REMI_USE_MMAP, &remi_status);
        HANDLE_REMI_ERROR(remi_fileset_migrate, rret, "REMI failed to migrate fileset");
        if(remi_status) {
            result.success() = false;
            result.error() = fmt::format("Migration failed with status {}", remi_status);
            mh->cancel();
            return;
        }
#endif
        trace("Sucessfully executed migrateTarget for target {}", target_id.to_string());
    }

    void checkTargetRPC(const tl::request& req,
                        const UUID& target_id) {
        trace("Received checkTarget request for target {}", target_id.to_string());
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        trace("Successfully check for presence of target {}", target_id.to_string());
    }

    void createRPC(const tl::request& req,
                   const UUID& target_id,
                   size_t size) {
        trace("Received create request for target {}", target_id.to_string());
        Result<RegionID> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->create(size);
        if(!region.success()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        result = region.value()->getRegionID();
        trace("Successfully executed create on target {}", target_id.to_string());
    }

    void writeRPC(const tl::request& req,
                  const UUID& target_id,
                  const RegionID& region_id,
                  const std::vector<std::pair<size_t, size_t>>& regionOffsetSizes,
                  thallium::bulk data,
                  const std::string& address,
                  size_t bulkOffset,
                  bool persist) {
        trace("Received write request for target {}", target_id.to_string());
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->write(region_id, persist);
        if(!region.success()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        auto source = address.empty() ? req.get_endpoint() : m_engine.lookup(address);
        result = target->m_transfer_manager->pull(
                *region.value(), regionOffsetSizes, data, source, bulkOffset, persist);
        trace("Successfully executed write on target {}", target_id.to_string());
    }

    void writeEagerRPC(const tl::request& req,
                       const UUID& target_id,
                       const RegionID& region_id,
                       const std::vector<std::pair<size_t, size_t>>& regionOffsetSizes,
                       const BufferWrapper& buffer,
                       bool persist) {
        trace("Received write_eager request for target {}", target_id.to_string());
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->write(region_id, persist);
        if(!region.success()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        result = region.value()->write(regionOffsetSizes, buffer.data(), persist);
        trace("Successfully executed write_eager on target {}", target_id.to_string());
    }

    void persistRPC(const tl::request& req,
                    const UUID& target_id,
                    const RegionID& region_id,
                    const std::vector<std::pair<size_t, size_t>>& regionOffsetSizes) {
        trace("Received persist request for target {}", target_id.to_string());
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->write(region_id, true);
        if(!region.success()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        result = region.value()->persist(regionOffsetSizes);
        trace("Successfully executed persist on target {}", target_id.to_string());
    }

    void createWriteRPC(const tl::request& req,
                        const UUID& target_id,
                        thallium::bulk data,
                        const std::string& address,
                        size_t bulkOffset, size_t size,
                        bool persist) {
        trace("Received create_write request for target {}", target_id.to_string());
        Result<RegionID> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->create(size);
        if(!region.success()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        result = region.value()->getRegionID();
        auto source = address.empty() ? req.get_endpoint() : m_engine.lookup(address);
        Result<bool> writeResult;
        writeResult = target->m_transfer_manager->pull(
                *region.value(), {{0, size}}, data, source, bulkOffset, persist);
        if(!writeResult.success()) {
            result.success() = false;
            result.error() = writeResult.error();
        }
        trace("Successfully executed create_write on target {}", target_id.to_string());
    }

    void createWriteEagerRPC(const tl::request& req,
                             const UUID& target_id,
                             const BufferWrapper& buffer,
                             bool persist) {
        trace("Received create_write_eager request for target {}", target_id.to_string());
        Result<RegionID> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->create(buffer.size());
        if(!region.success()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        result = region.value()->getRegionID();
        auto writeResult = region.value()->write(
                {{0, buffer.size()}}, buffer.data(), persist);
        if(!writeResult.success()) {
            result.success() = false;
            result.error() = writeResult.error();
        }
        trace("Successfully executed create_write_eager on target {}", target_id.to_string());
    }

    void readRPC(const tl::request& req,
                 const UUID& target_id,
                 const RegionID& region_id,
                 const std::vector<std::pair<size_t, size_t>>& regionOffsetSizes,
                 thallium::bulk data,
                 const std::string& address,
                 size_t bulkOffset) {
        trace("Received read request for target {}", target_id.to_string());
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->read(region_id);
        if(!region.value()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        auto source = address.empty() ? req.get_endpoint() : m_engine.lookup(address);
        result = target->m_transfer_manager->push(
                *region.value(), regionOffsetSizes, data, source, bulkOffset);
        trace("Successfully executed read on target {}", target_id.to_string());
    }

    void readEagerRPC(const tl::request& req,
                      const UUID& target_id,
                      const RegionID& region_id,
                      const std::vector<std::pair<size_t, size_t>>& regionOffsetSizes) {
        trace("Received read_eager request for target {}", target_id.to_string());
        Result<BufferWrapper> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        auto region = (*target)->read(region_id);
        if(!region.value()) {
            result.success() = false;
            result.error() = region.error();
            return;
        }
        size_t size = std::accumulate(regionOffsetSizes.begin(), regionOffsetSizes.end(), (size_t)0,
                [](size_t acc, const std::pair<size_t, size_t>& p) { return acc + p.second; });
        result.value().allocate(size);
        auto ret = region.value()->read(regionOffsetSizes, result.value().data());
        if(!ret.success()) {
            result.success() = false;
            result.error() = ret.error();
        }
        trace("Successfully executed read_eager on target {}", target_id.to_string());
    }

    void eraseRPC(const tl::request& req,
                  const UUID& target_id,
                  const RegionID& region_id) {
        trace("Received erase request for target {}", target_id.to_string());
        Result<bool> result;
        AutoResponse<decltype(result)> response{req, result};
        FIND_TARGET(target);
        result = (*target)->erase(region_id);
        trace("Successfully executed erase on target {}", target_id.to_string());
    }

    static int32_t beforeMigrationCallback(remi_fileset_t fileset, void* uargs) {
        // the goal this callback is just to make sure the required metadata
        // is available and there  isn't any database with the same name yet,
        // so we can do the migration safely.
        ProviderImpl* provider = static_cast<ProviderImpl*>(uargs);

        const char* uuid = nullptr;
        const char* type = nullptr;
        const char* config = nullptr;
        const char* migration_config = nullptr;

        int rret;
        rret = remi_fileset_get_metadata(fileset, "uuid", &uuid);
        if(rret != REMI_SUCCESS) return rret;
        rret = remi_fileset_get_metadata(fileset, "type", &type);
        if(rret != REMI_SUCCESS) return rret;
        rret = remi_fileset_get_metadata(fileset, "config", &config);
        if(rret != REMI_SUCCESS) return rret;
        rret = remi_fileset_get_metadata(fileset, "migration_config", &migration_config);
        if(rret != REMI_SUCCESS) return rret;

        auto target_id = UUID::from_string(uuid);
        json config_json;
        json migration_config_json;
        try {
            config_json = json::parse(config);
            migration_config_json = json::parse(migration_config);
        } catch (...) {
            return 1;
        }
        config_json.update(migration_config_json, true);
        if(config_json.contains("transfer_manager")) {
            auto tm_name = config_json["transfer_manager"].get<std::string>();
            std::unique_lock<tl::mutex> lock{provider->m_transfer_managers_mtx};
            auto it = provider->m_transfer_managers.find(tm_name);
            if(it == provider->m_transfer_managers.end())
                return 2;
        }
        auto it = provider->m_targets.find(target_id);
        if(it != provider->m_targets.end())
            return 3;
        if(!TargetFactory::validateConfig(type, config_json).success())
            return 4;
        return 0;
    }

    static int32_t afterMigrationCallback(remi_fileset_t fileset, void* uargs) {
        ProviderImpl* provider = static_cast<ProviderImpl*>(uargs);

        const char* uuid = nullptr;
        const char* type = nullptr;
        const char* config = nullptr;
        const char* migration_config = nullptr;

        int rret;
        rret = remi_fileset_get_metadata(fileset, "uuid", &uuid);
        if(rret != REMI_SUCCESS) return rret;
        rret = remi_fileset_get_metadata(fileset, "type", &type);
        if(rret != REMI_SUCCESS) return rret;
        rret = remi_fileset_get_metadata(fileset, "config", &config);
        if(rret != REMI_SUCCESS) return rret;
        rret = remi_fileset_get_metadata(fileset, "migration_config", &migration_config);
        if(rret != REMI_SUCCESS) return rret;

        auto target_id = UUID::from_string(uuid);
        json config_json;
        json migration_config_json;
        try {
            config_json = json::parse(config);
            migration_config_json = json::parse(migration_config);
        } catch (...) {
            return 1;
        }
        config_json.update(migration_config_json, true);

        std::shared_ptr<TransferManager> tm;
        std::string tm_name;
        if(config_json.contains("transfer_manager")) {
            tm_name = config_json["transfer_manager"].get<std::string>();
            std::unique_lock<tl::mutex> lock{provider->m_transfer_managers_mtx};
            auto it = provider->m_transfer_managers.find(tm_name);
            tm = it->second;
        } else {
            tm_name = "__default__";
            tm = provider->m_transfer_managers["__default__"];
        }

        std::vector<std::string> files;
        rret = remi_fileset_walkthrough(fileset,
            [](const char* filename, void* uargs) {
                static_cast<std::vector<std::string>*>(uargs)->emplace_back(filename);
            }, &files);
        if(rret != REMI_SUCCESS) return 2;

        size_t root_size = 0;
        std::vector<char> root;
        rret = remi_fileset_get_root(fileset, nullptr, &root_size);
        if(rret != REMI_SUCCESS) return 3;
        root.resize(root_size);
        rret = remi_fileset_get_root(fileset, root.data(), &root_size);
        if(rret != REMI_SUCCESS) return 3;

        auto root_str = std::string{root.data()};
        if(root_str.empty() || root_str.back() != '/')
            root_str += "/";
        for(auto& filename : files) {
            filename = root_str + filename;
        }

        auto target = TargetFactory::recoverTarget(type, provider->m_engine, config_json, files);
        if(!target.success()) {
            provider->error("{}", target.error());
            return 4;
        }

        provider->m_targets[target_id] = std::make_shared<TargetEntry>(
                std::shared_ptr<Backend>(std::move(target.value())), tm, tm_name);

        return 0;
    }

};

}

#endif
