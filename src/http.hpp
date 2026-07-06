#pragma once
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct HttpResponse {
    long        status = 0;
    std::string body;
    std::string final_url;
};

using Params  = std::map<std::string, std::string>;
using Headers = std::map<std::string, std::string>;

struct FormField {
    std::string name;
    std::string value;
};

// Cookie-aware HTTP session backed by libcurl.
// One HttpClient per logical "session" (e.g. one Timegrip session).
class HttpClient {
public:
    explicit HttpClient(bool follow_redirects = true);
    ~HttpClient();

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void set_user_agent(const std::string& ua);

    HttpResponse get(const std::string& url, const Params& params = {});

    // application/x-www-form-urlencoded POST
    HttpResponse post_form(const std::string& url,
                           const std::vector<FormField>& fields);

    // application/json POST
    HttpResponse post_json(const std::string& url,
                           const std::string& body,
                           const Headers& extra_headers = {});

private:
    void* curl_;   // CURL*

    static std::size_t write_cb(char* ptr, std::size_t size,
                                std::size_t nmemb, void* ud);

    std::string build_query(const Params& p) const;
    std::string url_encode(const std::string& s) const;
};

struct HttpError : std::runtime_error {
    long status;
    HttpError(long s, const std::string& msg)
        : std::runtime_error(msg), status(s) {}
};
