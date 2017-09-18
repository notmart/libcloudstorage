#include <json/json.h>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include "FuseHighLevel.h"
#include "FuseLowLevel.h"
#include "ICloudStorage.h"
#include "IHttpServer.h"
#include "Utility/CurlHttp.h"
#include "Utility/MicroHttpdServer.h"
#include "Utility/Utility.h"

using namespace std::placeholders;
using namespace cloudstorage;

#define OPTION(t, p) \
  { t, offsetof(struct options, p), 1 }

template <class T>
using pointer = std::unique_ptr<T, std::function<void(T *)>>;

struct options {
  ~options() {
    free(config_file);
    free(provider_label);
  }
  char *config_file;
  char *provider_label;
};

struct HttpServerData {
  std::string code_;
  std::string state_;
};

struct FuseLowLevel {
  FuseLowLevel(fuse_args *args, const char *mountpoint, void *userdata) {
    auto operations = cloudstorage::low_level_operations();
    session_ =
        fuse_session_new(args, &operations, sizeof(operations), userdata);
    fuse_set_signal_handlers(session_);
    fuse_session_mount(session_, mountpoint);
  }

  ~FuseLowLevel() {
    fuse_session_unmount(session_);
    fuse_remove_signal_handlers(session_);
    fuse_session_destroy(session_);
  }

  int run(bool singlethread, bool clone_fd) const {
    return singlethread ? fuse_session_loop(session_)
                        : fuse_session_loop_mt(session_, clone_fd);
  }

  fuse_session *session_;
};

class HttpServerCallback : public IHttpServer::ICallback {
 public:
  HttpServerCallback(std::promise<HttpServerData> &p) : promise_(p) {}

  IHttpServer::IResponse::Pointer handle(const IHttpServer::IRequest &request) {
    auto code = request.get("code");
    auto state = request.get("state");
    if (code && state) {
      promise_.set_value({code, state});
      return util::response_from_string(request, IHttpRequest::Ok, {},
                                        "token received");
    } else if (request.url() == "/login" && state) {
      return util::response_from_string(request, IHttpRequest::Ok, {},
                                        util::login_page(state));
    } else {
      return util::response_from_string(request, IHttpRequest::Bad, {},
                                        "error");
    }
  }

 private:
  std::promise<HttpServerData> &promise_;
};

class HttpWrapper : public IHttp {
 public:
  HttpWrapper(std::shared_ptr<IHttp> http) : http_(http) {}
  IHttpRequest::Pointer create(const std::string &url,
                               const std::string &method,
                               bool follow_redirect) const override {
    return http_->create(url, method, follow_redirect);
  }

 private:
  std::shared_ptr<IHttp> http_;
};

const struct fuse_opt option_spec[] = {OPTION("--config=%s", config_file),
                                       OPTION("--add=%s", provider_label),
                                       FUSE_OPT_END};

cloudstorage::ICloudProvider::Pointer create(std::shared_ptr<IHttp> http,
                                             std::string temporary_directory,
                                             Json::Value config) {
  class AuthCallback : public ICloudProvider::IAuthCallback {
    Status userConsentRequired(const ICloudProvider &) override {
      return Status::None;
    }
    void done(const ICloudProvider &, EitherError<void>) override {}
  };
  ICloudProvider::InitData init_data;
  init_data.callback_ = util::make_unique<AuthCallback>();
  init_data.token_ = config["token"].asString();
  init_data.http_engine_ = util::make_unique<HttpWrapper>(http);
  init_data.hints_["access_token"] = config["access_token"].asString();
  init_data.hints_["youtube_dl_url"] = config["youtube_dl_url"].asString();
  init_data.hints_["temporary_directory"] = temporary_directory;
  return ICloudStorage::create()->provider(config["type"].asString(),
                                           std::move(init_data));
}

std::vector<cloudstorage::IFileSystem::ProviderEntry> providers(
    const Json::Value &data, std::shared_ptr<IHttp> http,
    const std::string &temporary_directory) {
  std::vector<cloudstorage::IFileSystem::ProviderEntry> providers;
  for (auto &&p : data)
    providers.push_back(
        {p["label"].asString(), create(http, temporary_directory, p)});
  return std::move(providers);
}

int fuse_lowlevel(fuse_args *args, fuse_cmdline_opts *opts, Json::Value &json) {
  auto ctx = new IFileSystem *;
  FuseLowLevel fuse(args, opts->mountpoint, ctx);
  fuse_daemonize(opts->foreground);
  auto http = std::make_shared<curl::CurlHttp>();
  auto temporary_directory = json["temporary_directory"].asString();
  if (temporary_directory.empty()) temporary_directory = "/tmp/";
  auto p = providers(json["providers"], http, temporary_directory);
  *ctx = IFileSystem::create(p, util::make_unique<HttpWrapper>(http),
                             temporary_directory)
             .release();
  int ret = fuse.run(opts->singlethread, opts->clone_fd);
  for (size_t i = 0; i < p.size(); i++) {
    json["providers"][int(i)]["token"] = p[i].provider_->token();
    json["providers"][int(i)]["access_token"] =
        p[i].provider_->hints()["access_token"];
  }
  delete *ctx;
  delete ctx;
  return ret;
}

int main(int argc, char **argv) {
  pointer<fuse_args> args(new fuse_args(FUSE_ARGS_INIT(argc, argv)),
                          [](fuse_args *e) {
                            fuse_opt_free_args(e);
                            delete e;
                          });
  struct options options {};
  if (fuse_opt_parse(args.get(), &options, option_spec, nullptr) == -1)
    return 1;
  if (!options.config_file)
    options.config_file = strdup(
        (std::string(getenv("HOME")) + "/.libcloudstorage-fuse.json").c_str());
  pointer<fuse_cmdline_opts> opts(new fuse_cmdline_opts{},
                                  [](fuse_cmdline_opts *opts) {
                                    free(opts->mountpoint);
                                    delete opts;
                                  });
  if (fuse_parse_cmdline(args.get(), opts.get()) != 0) return 1;
  if (opts->show_help) {
    std::cerr << util::libcloudstorage_ascii_art() << "\n\n";
    std::cerr << "    --add=provider_label   add cloud provider with label\n";
    std::cerr << "    --config=config_path   path to configuration file\n";
    std::cerr << "                           (default: "
                 "~/.libcloudstorage-fuse.json)\n";
    std::cerr << "\n";
    fuse_cmdline_help();
    fuse_lowlevel_help();
    return 0;
  } else if (opts->show_version) {
    fuse_lowlevel_version();
    return 0;
  }
  std::stringstream stream;
  stream << std::ifstream(options.config_file).rdbuf();
  Json::Value json;
  Json::Reader().parse(stream.str(), json);
  if (options.provider_label) {
    std::cerr << cloudstorage::util::libcloudstorage_ascii_art() << "\n";
    auto storage = ICloudStorage::create();
    for (auto &&p : storage->providers()) {
      ICloudProvider::InitData init_data;
      init_data.hints_["state"] = p;
      std::cerr << "\n";
      std::cerr
          << p << ": "
          << storage->provider(p, std::move(init_data))->authorizeLibraryUrl()
          << "\n";
    }
    std::promise<HttpServerData> result;
    auto server = cloudstorage::MicroHttpdServerFactory().create(
        std::make_shared<HttpServerCallback>(result), "",
        IHttpServer::Type::Authorization);
    auto key = result.get_future().get();
    auto provider = storage->provider(key.state_, {});
    if (!provider) return 1;
    auto ret = provider->exchangeCodeAsync(key.code_)->result();
    if (auto token = ret.right()) {
      Json::Value p;
      p["label"] = options.provider_label;
      p["type"] = provider->name();
      p["token"] = token->token_;
      p["access_token"] = token->access_token_;
      json["providers"].append(p);
      std::ofstream(options.config_file) << json;
    } else
      std::cerr << "error " << ret.left()->code_ << ": "
                << ret.left()->description_ << "\n";
    return 0;
  }
  int ret = fuse_lowlevel(args.get(), opts.get(), json);
  std::ofstream(options.config_file) << json;
  return ret;
}