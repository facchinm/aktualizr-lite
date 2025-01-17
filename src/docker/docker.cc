#include "docker.h"
#include <fstream>

#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include "crypto/crypto.h"
#include "http/httpclient.h"

namespace Docker {

Uri Uri::parseUri(const std::string& uri, bool factory_app) {
  // check whether uri is pinned
  auto split_pos = uri.find('@');
  if (split_pos == std::string::npos) {
    throw std::invalid_argument("Invalid URI: digest/'@' not found in " + uri);
  }

  auto digest = uri.substr(split_pos + 1);

  // find start of <name> (aka path) position
  auto name_pos_start = uri.find('/', 0);
  if (name_pos_start == std::string::npos) {
    throw std::invalid_argument("Invalid URI: image name/path is not found in " + uri);
  }

  if (split_pos <= (name_pos_start + 1)) {
    throw std::invalid_argument("Invalid URI: image name/path is not present before digest; uri: " + uri);
  }

  auto registry_hostname = uri.substr(0, name_pos_start);
  auto name = uri.substr(name_pos_start + 1, split_pos - name_pos_start - 1);

  auto app_pos_start = name.rfind('/');
  std::string factory;
  if (app_pos_start != std::string::npos) {
    factory = name.substr(0, app_pos_start);
  } else {
    app_pos_start = -1;
  }
  auto app = name.substr(app_pos_start + 1, name.size() - app_pos_start - 1);

  if (factory_app && (factory.empty() || factory.find('/') != std::string::npos)) {
    throw std::invalid_argument("Invalid URI: invalid name format of a factory image, must be <factory>/<repo>; uri: " +
                                uri);
  }

  return Uri{HashedDigest{digest}, app, factory, name, registry_hostname};
}

Uri Uri::createUri(const HashedDigest& digest_in) const {
  return Uri{HashedDigest{digest_in}, app, factory, repo, registryHostname};
}

const std::string HashedDigest::Type{"sha256:"};

HashedDigest::HashedDigest(const std::string& hash_digest) : digest_{boost::algorithm::to_lower_copy(hash_digest)} {
  if (Type != digest_.substr(0, Type.length())) {
    throw std::invalid_argument("Unsupported hash type: " + hash_digest);
  }

  hash_ = digest_.substr(Type.length());
  if (64 != hash_.size()) {
    throw std::invalid_argument("Invalid hash size: " + hash_digest);
  }

  short_hash_ = hash_.substr(0, 7);
}

const RegistryClient::HttpClientFactory RegistryClient::DefaultHttpClientFactory =
    [](const std::vector<std::string>* headers, const std::set<std::string>* response_header_names) {
      return std::make_shared<HttpClient>(headers, response_header_names);
    };

const std::string RegistryClient::ManifestEndpoint{"/manifests/"};
const std::string RegistryClient::BlobEndpoint{"/blobs/"};
const std::string RegistryClient::SupportedRegistryVersion{"/v2/"};

const std::string RegistryClient::BearerAuth::Header{"www-authenticate"};
const std::string RegistryClient::BearerAuth::AuthType{"bearer"};

RegistryClient::BearerAuth::BearerAuth(const std::string& auth_header_value) {
  if (!boost::starts_with(auth_header_value, AuthType)) {
    throw std::invalid_argument("Unsupported authentication type to access Registry: " + auth_header_value);
  }
  auto bearer_val{auth_header_value.substr(auth_header_value.rfind(AuthType) + AuthType.size())};
  boost::trim(bearer_val);

  std::unordered_map<std::string, std::string> bearer_params;
  std::size_t param_begin_pos{0};
  std::size_t param_eq_pos;
  while (std::string::npos != (param_eq_pos = bearer_val.find('=', param_begin_pos))) {
    std::size_t param_value_beg_pos = bearer_val.find('"', param_begin_pos);
    if (std::string::npos == param_value_beg_pos) {
      throw std::invalid_argument("Invalid value of Bearer auth parameters; missing openning `\"` in " +
                                  auth_header_value);
    }
    if (param_eq_pos > param_value_beg_pos) {
      throw std::invalid_argument("Invalid value of Bearer auth parameters; `\"` before `=` in " + auth_header_value);
    }

    // No any symbol should be between `=` and the opening `"` except a space
    const auto must_be_empty_or_space = bearer_val.substr(param_eq_pos + 1, param_value_beg_pos - param_eq_pos - 1);
    for (const auto& s : must_be_empty_or_space) {
      if (s != ' ') {
        throw std::invalid_argument("Invalid value of Bearer auth parameters; missing openning `\"` in " +
                                    auth_header_value);
      }
    }
    std::size_t param_value_end_pos = bearer_val.find('"', param_value_beg_pos + 1);
    if (std::string::npos == param_value_end_pos) {
      throw std::invalid_argument("Invalid value of Bearer auth parameters; missing clossing `\"` in " +
                                  auth_header_value);
    }

    auto param_name = bearer_val.substr(param_begin_pos, param_eq_pos - param_begin_pos);
    boost::trim(param_name);
    auto param_value = bearer_val.substr(param_value_beg_pos + 1, param_value_end_pos - param_value_beg_pos - 1);
    boost::trim(param_value);

    bearer_params[param_name] = param_value;
    param_begin_pos = bearer_val.find(',', param_value_end_pos);
    if (param_begin_pos != std::string::npos) {
      ++param_begin_pos;
    }
  }
  // The OAuth spec does not state that the following auth params are mandatory
  // https://www.rfc-editor.org/rfc/rfc6750#section-3, but, the Docker Registry v2 auth spec requires it
  // https://github.com/distribution/distribution/blob/263da70ea6a4e96f61f7a6770273ec6baac38941/docs/spec/auth/token.md
  const std::array<std::string, 3> required_params{"realm", "service", "scope"};
  std::vector<std::string> missing_params;
  for (const auto& param : required_params) {
    if (bearer_params.count(param) == 0) {
      missing_params.push_back(param);
    }
  }

  if (!missing_params.empty()) {
    throw std::invalid_argument("Missing required auth param(s): " + boost::algorithm::join(missing_params, ", "));
  }

  Realm = bearer_params[required_params[0]];
  Service = bearer_params[required_params[1]];
  Scope = bearer_params[required_params[2]];
}

RegistryClient::RegistryClient(std::shared_ptr<HttpInterface> ota_lite_client, std::string auth_creds_endpoint,
                               HttpClientFactory http_client_factory)
    : auth_creds_endpoint_{std::move(auth_creds_endpoint)},
      ota_lite_client_{std::move(ota_lite_client)},
      http_client_factory_{std::move(http_client_factory)} {}

std::string RegistryClient::getAppManifest(const Uri& uri, const std::string& format,
                                           boost::optional<std::int64_t> manifest_size) const {
  const std::string manifest_url{composeManifestUrl(uri)};
  LOG_DEBUG << "Downloading App manifest: " << manifest_url;

  std::vector<std::string> registry_repo_request_headers{"accept:" + format};
  const std::set<std::string> header_to_get{BearerAuth::Header};

  std::int64_t manifest_max_size{DefManifestMaxSize};
  if (!!manifest_size) {
    manifest_max_size = *manifest_size;
  }
  std::function<HttpResponse()> doGetManifestRequest = [&]() {
    auto registry_repo_client{http_client_factory_(&registry_repo_request_headers, &header_to_get)};
    return registry_repo_client->get(manifest_url, manifest_max_size);
  };

  auto manifest_resp = doGetManifestRequest();
  if (manifest_resp.http_status_code == 401) {
    if (manifest_resp.headers.empty() || manifest_resp.headers.count(BearerAuth::Header) == 0) {
      throw std::runtime_error("No `" + BearerAuth::Header + "` header found in the 401 response");
    }
    auto auth_header{getBearerAuthHeader(BearerAuth(manifest_resp.headers[BearerAuth::Header]))};
    registry_repo_request_headers.push_back(auth_header);
    manifest_resp = doGetManifestRequest();
  }

  if (!manifest_resp.isOk()) {
    throw std::runtime_error("Failed to download App manifest: " + manifest_resp.getStatusStr());
  }

  if (!!manifest_size) {
    if (manifest_resp.body.size() != *manifest_size) {
      throw std::runtime_error("Size of received App manifest doesn't match the expected one: " +
                               std::to_string(manifest_resp.body.size()) + " != " + std::to_string(*manifest_size));
    }
  } else {
    if (manifest_resp.body.size() > manifest_max_size) {
      throw std::runtime_error("Size of received App manifest exceeds the maximum allowed: " +
                               std::to_string(manifest_resp.body.size()) + " > " + std::to_string(manifest_max_size));
    }
  }

  auto received_manifest_hash{
      boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(manifest_resp.body)))};

  if (received_manifest_hash != uri.digest.hash()) {
    throw std::runtime_error(
        "Hash of received App manifest and the hash specified in Target"
        " do not match: " +
        received_manifest_hash + " != " + uri.digest.hash());
  }

  LOG_TRACE << "Received App manifest: \n" << manifest_resp.getJson();
  return manifest_resp.body;
}

struct DownloadCtx {
  DownloadCtx(std::ostream& out_stream_in, MultiPartHasher& hasher_in, std::size_t expected_size_in)
      : out_stream{out_stream_in}, hasher{hasher_in}, expected_size{expected_size_in} {}

  std::ostream& out_stream;
  MultiPartHasher& hasher;
  std::size_t expected_size;

  std::size_t written_size{0};
  std::size_t received_size{0};

  std::size_t write(const char* data, std::size_t size) {
    assert(data);

    received_size = written_size + size;
    if (received_size > expected_size) {
      LOG_ERROR << "!!! Received data size exceeds the expected size: " << received_size << " != " << expected_size;
      return (size + 1);  // returning value that is not equal to received data size will make curl fail
    }

    if (!out_stream.good()) {
      LOG_ERROR << "Output stream is at a bad state: " << out_stream.rdstate();
      return (size + 1);  // returning value that is not equal to received data size will make curl fail
    }

    auto start_pos = out_stream.tellp();
    out_stream.write(data, size);
    auto end_pos = out_stream.tellp();

    written_size += (end_pos - start_pos);
    hasher.update(reinterpret_cast<const unsigned char*>(data), size);
    return (end_pos - start_pos);
  }
  void reset() {
    out_stream.seekp(std::ios_base::beg);
    hasher.reset();
    written_size = 0;
    received_size = 0;
  }
};

static size_t DownloadHandler(char* data, size_t buf_size, size_t buf_numb, void* user_ctx) {
  assert(user_ctx);

  auto* download_ctx = reinterpret_cast<DownloadCtx*>(user_ctx);
  return download_ctx->write(data, (buf_size * buf_numb));
}

void RegistryClient::downloadBlob(const Uri& uri, const boost::filesystem::path& filepath, size_t expected_size) const {
  auto compose_app_blob_url{composeBlobUrl(uri)};

  LOG_DEBUG << "Downloading App blob: " << compose_app_blob_url;

  std::ofstream output_file{filepath.string(), std::ios_base::out | std::ios_base::binary};
  if (!output_file.is_open()) {
    throw std::runtime_error("Failed to open a file: " + filepath.string());
  }
  MultiPartSHA256Hasher hasher;
  DownloadCtx download_ctx{output_file, hasher, expected_size};

  const std::set<std::string> header_to_get{BearerAuth::Header};
  std::vector<std::string> registry_repo_request_headers;
  std::function<HttpResponse()> doDownloadBlobRequest = [&]() {
    auto registry_repo_client{http_client_factory_(&registry_repo_request_headers, &header_to_get)};
    return registry_repo_client->download(compose_app_blob_url, DownloadHandler, nullptr, &download_ctx, 0);
  };

  auto get_blob_resp = doDownloadBlobRequest();
  if (get_blob_resp.http_status_code == 401) {
    if (get_blob_resp.headers.empty() || get_blob_resp.headers.count(BearerAuth::Header) == 0) {
      throw std::runtime_error("No `" + BearerAuth::Header + "` header found in the 401 response");
    }
    auto auth_header{getBearerAuthHeader(BearerAuth(get_blob_resp.headers[BearerAuth::Header]))};
    registry_repo_request_headers.push_back(auth_header);
    download_ctx.reset();
    get_blob_resp = doDownloadBlobRequest();
  }
  if (!get_blob_resp.isOk()) {
    throw std::runtime_error("Failed to download App blob: " + get_blob_resp.getStatusStr());
  }

  output_file.close();
  std::size_t recv_blob_file_size{download_ctx.written_size};

  if (recv_blob_file_size != expected_size) {
    std::remove(filepath.c_str());
    throw std::runtime_error(
        "Size of downloaded App blob does not equal to "
        "the expected one: " +
        std::to_string(recv_blob_file_size) + " != " + std::to_string(expected_size));
  }

  auto recv_blob_hash{boost::algorithm::to_lower_copy(hasher.getHexDigest())};

  if (recv_blob_hash != uri.digest.hash()) {
    std::remove(filepath.c_str());
    throw std::runtime_error(
        "Hash of downloaded App blob does not equal to "
        "the expected one: " +
        recv_blob_hash + " != " + uri.digest.hash());
  }
}

std::string RegistryClient::getBasicAuthHeader() const {
  // TODO: to make it working against any Registry, not just FIO's one
  // we will need to make use of the Docker's mechanisms for it,
  // specifically in docker/config.json there should defined an auth material and/or credHelpers
  // for a given registry. If auth material is defined then just use it if not then try to invoke
  // a script/executbale defined in credHelpers  that is supposed to return an auth material
  LOG_DEBUG << "Getting Docker Registry credentials from " << auth_creds_endpoint_;

  auto creds_resp = ota_lite_client_->get(auth_creds_endpoint_, AuthMaterialMaxSize);

  if (!creds_resp.isOk()) {
    throw std::runtime_error("Failed to get Docker Registry credentials from " + auth_creds_endpoint_ +
                             "; error: " + creds_resp.getStatusStr());
  }

  auto creds_json = creds_resp.getJson();
  auto username = creds_json["Username"].asString();
  auto secret = creds_json["Secret"].asString();

  if (username.empty() || secret.empty()) {
    throw std::runtime_error("Got invalid Docker Registry credentials: " + creds_resp.body);
  }

  std::string auth_secret_str{username + ':' + secret};
  auto encoded_auth_secret = Utils::toBase64(auth_secret_str);

  LOG_DEBUG << "Got Docker Registry credentials, username: " << username;
  return "authorization: basic " + encoded_auth_secret;
}

std::string RegistryClient::getBearerAuthHeader(const BearerAuth& bearer) const {
  LOG_DEBUG << "Getting Docker Registry token from " << bearer.Realm;

  std::vector<std::string> auth_header = {getBasicAuthHeader()};
  auto registry_client{http_client_factory_(&auth_header, nullptr)};
  auto token_resp = registry_client->get(bearer.uri(), AuthMaterialMaxSize);

  if (!token_resp.isOk()) {
    throw std::runtime_error("Failed to get Auth Token at Docker Registry " + bearer.Realm +
                             "; error: " + token_resp.getStatusStr());
  }

  auto token = token_resp.getJson()["token"].asString();
  if (token.empty()) {
    throw std::runtime_error("Got invalid token from Docker Registry: " + token_resp.body);
  }

  LOG_DEBUG << "Got Docker Registry token: " << token;
  return "authorization: bearer " + token;
}

}  // namespace Docker
