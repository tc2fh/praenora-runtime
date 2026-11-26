#include "discovery/manifest.hpp"

#include "core/sha256.hpp"
#include "discovery/manifest_reader.hpp"

#include <praenora/runtime/error.hpp>

#include <cstddef>
#include <utility>

namespace praenora::detail {

ModuleManifest load_manifest(const std::filesystem::path& file) {
    const ManifestReader reader(file, "module-manifest", "module manifest", "module manifests");
    const nlohmann::json& json = reader.json();

    ModuleManifest manifest;
    manifest.manifest_path = absolute_manifest_path(file);
    manifest.directory = manifest.manifest_path.parent_path();
    manifest.manifest_sha256 = sha256_hex(reader.bytes());

    manifest.name = json.at("module").at("name").get<std::string>();
    manifest.version = reader.version_at("/module/version");
    manifest.abi_major = reader.uint32_at("/abi/major");
    manifest.abi_minor = reader.uint32_at("/abi/minor");
    manifest.pmp_version = reader.version_at("/pmp_version");
    manifest.sdk_version = reader.version_at("/sdk_version");

    const auto& library = json.at("library");
    manifest.library_path =
        reader.relative_path_at("/library/path", manifest.directory, "library.path");
    if (library.contains("sha256")) {
        manifest.library_sha256 = library.at("sha256").get<std::string>();
    }
    if (library.contains("dependency_paths")) {
        const auto& paths = library.at("dependency_paths");
        for (std::size_t i = 0; i < paths.size(); ++i) {
            manifest.dependency_paths.push_back(
                reader.relative_path_at("/library/dependency_paths/" + std::to_string(i),
                                        manifest.directory, "dependency path"));
        }
    }

    manifest.os = json.at("platform").at("os").get<std::string>();
    manifest.arch = json.at("platform").at("arch").get<std::string>();

    const auto& capabilities = json.at("capabilities");
    for (std::size_t i = 0; i < capabilities.size(); ++i) {
        const std::string pointer = "/capabilities/" + std::to_string(i);
        CapabilityRef capability{capabilities[i].at("id").get<std::string>(),
                                 reader.version_at(pointer + "/version")};
        for (std::size_t k = 0; k < manifest.capabilities.size(); ++k) {
            const auto& earlier = manifest.capabilities[k];
            if (earlier.id == capability.id && earlier.version == capability.version) {
                reader.fail(ErrorCode::MANIFEST_INVALID, pointer,
                            "capability " + capability.id + "@" + to_string(capability.version) +
                                " is listed twice (also at /capabilities/" + std::to_string(k) +
                                ")");
            }
        }
        manifest.capabilities.push_back(std::move(capability));
    }

    if (json.contains("requires") && json.at("requires").contains("services")) {
        const auto& services = json.at("requires").at("services");
        for (std::size_t i = 0; i < services.size(); ++i) {
            const std::string pointer = "/requires/services/" + std::to_string(i);
            ServiceRequirement requirement;
            requirement.service.name = services[i].at("name").get<std::string>();
            requirement.service.major = reader.uint32_at(pointer + "/major");
            if (services[i].contains("minor")) {
                requirement.service.minor = reader.uint32_at(pointer + "/minor");
            }
            requirement.optional = services[i].value("optional", false);
            for (std::size_t k = 0; k < manifest.service_requirements.size(); ++k) {
                const ServiceVersion& earlier = manifest.service_requirements[k].service;
                if (earlier.name == requirement.service.name &&
                    earlier.major == requirement.service.major) {
                    reader.fail(ErrorCode::MANIFEST_INVALID, pointer,
                                "service " + service_key(requirement.service) +
                                    " is listed twice (also at /requires/services/" +
                                    std::to_string(k) + ")");
                }
            }
            manifest.service_requirements.push_back(std::move(requirement));
        }
    }

    manifest.parameters_schema = json.contains("parameters_schema")
                                     ? json.at("parameters_schema")
                                     : nlohmann::json{{"type", "object"}};
    try {
        manifest.parameters = std::make_shared<const CompiledSchema>(
            manifest.parameters_schema,
            manifest.name + "@" + to_string(manifest.version) + " parameters_schema");
    } catch (const Error& error) {
        reader.fail(ErrorCode::MANIFEST_INVALID, "/parameters_schema",
                    std::string("not a valid JSON Schema: ") + error.what());
    }

    if (json.contains("features")) {
        const auto& features = json.at("features");
        manifest.features.concurrency = features.value("concurrency", std::string("module"));
        manifest.features.cancellation = features.value("cancellation", false);
        manifest.features.diagnostics = features.value("diagnostics", false);
        manifest.features.self_test = features.value("self_test", false);
    }
    if (json.contains("tests") && json.at("tests").contains("vectors")) {
        manifest.test_vectors = reader.relative_path_at("/tests/vectors", manifest.directory,
                                                        "tests.vectors");
    }
    return manifest;
}

} // namespace praenora::detail
