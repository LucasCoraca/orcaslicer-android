// Stand-in for slic3r/GUI/Jobs/OAuthJob.hpp: upstream's OAuth data types and the job's token request and callback
// server (extracted at build time) without the Job framework: the caller runs process() on its own thread.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <boost/asio/ip/basic_endpoint.hpp>
#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/Jobs/ThreadSafeQueue.hpp"

namespace Slic3r { namespace GUI {
#include "slic3r/GUI/OAuthTypes.inc"

struct OAuthJob
{
    // Job::Ctl: only the cancel flag is used
    struct Ctl
    {
        std::function<bool()> canceled;
        bool                  was_canceled() const { return canceled && canceled(); }
    };

    HttpServer local_authorization_server;
    OAuthData  _data;

    explicit OAuthJob(const OAuthData &input) : local_authorization_server(input.params.callback_port), _data(input) {}
    void process(Ctl &ctl);
    // OAuthJob::finalize: the server stops (the dialog is the caller's)
    void finalize() { local_authorization_server.stop(); }

    static void parse_token_response(const std::string &body, bool error, OAuthResult &result);
};
}} // namespace Slic3r::GUI
